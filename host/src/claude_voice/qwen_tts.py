"""Qwen3-TTS through an mlx-audio speech server, with `say` as the per-sentence fallback."""

from __future__ import annotations

import asyncio
import logging
import os
import subprocess
import tempfile
import time
from pathlib import Path

import httpx

from .audio import wav_to_pcm
from .config import Config
from .stt import free_port
from .tts import SayTts, TtsError

log = logging.getLogger("claude_voice.qwen_tts")

WARMUP_TEXT = "你好。"


class QwenTts:
    def __init__(
        self, cfg: Config, fallback: SayTts, transport: httpx.AsyncBaseTransport | None = None
    ) -> None:
        self._cfg = cfg
        self._fallback = fallback
        self._transport = transport
        self._http: httpx.AsyncClient | None = None
        self._proc: subprocess.Popen | None = None
        self._log_file = None
        self._skip_until = 0.0
        self._failing = False

    async def start(self) -> None:
        """Bring the server up; never raises, so a broken server leaves the relay on `say`."""
        cfg = self._cfg
        try:
            if cfg.qwen_server_cmd:
                base = await self._spawn()
            elif cfg.qwen_url:
                base = cfg.qwen_url
            else:
                log.error("qwen3 speech off: set qwen_url or qwen_server_cmd; using say")
                return
            self._http = self._client(base)
            t0 = time.monotonic()
            await self._request(WARMUP_TEXT, timeout=cfg.qwen_ready_timeout_s)
            log.info("qwen3 speech server ready, warm-up %.1f s", time.monotonic() - t0)
        except Exception as e:
            log.error("qwen3 speech server unavailable; using say: %r", e)
            await self._stop_server()
            if cfg.qwen_server_cmd:
                await self._drop_client()
            else:
                self._mark_failed()

    def _client(self, base: str) -> httpx.AsyncClient:
        t = httpx.Timeout(self._cfg.tts_timeout_s, connect=self._cfg.qwen_connect_timeout_s)
        return httpx.AsyncClient(base_url=base, timeout=t, transport=self._transport)

    async def _spawn(self) -> str:
        cfg = self._cfg
        port = free_port()
        log_dir = Path(cfg.log_dir)
        log_dir.mkdir(parents=True, exist_ok=True)
        self._log_file = open(log_dir / "qwen-tts-server.log", "ab")
        args = [*cfg.qwen_server_cmd, "--host", "127.0.0.1", "--port", str(port)]
        self._proc = subprocess.Popen(
            args, stdout=self._log_file, stderr=self._log_file, stdin=subprocess.DEVNULL
        )
        base = f"http://127.0.0.1:{port}"
        t0 = time.monotonic()
        async with httpx.AsyncClient(timeout=1.0) as probe:
            while True:
                if self._proc.poll() is not None:
                    raise TtsError(f"server exited with {self._proc.returncode} (see qwen-tts-server.log)")
                if time.monotonic() - t0 > cfg.qwen_ready_timeout_s:
                    raise TtsError("server not ready in time")
                try:
                    if (await probe.get(base + "/")).status_code < 500:
                        return base
                except httpx.HTTPError:
                    pass
                await asyncio.sleep(0.5)

    async def _drop_client(self) -> None:
        http, self._http = self._http, None
        if http is not None:
            await http.aclose()

    async def close(self) -> None:
        await self._drop_client()
        await self._stop_server()

    async def _stop_server(self) -> None:
        proc, self._proc = self._proc, None
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                await asyncio.to_thread(proc.wait, 5)
            except subprocess.TimeoutExpired:
                proc.kill()
                await asyncio.to_thread(proc.wait)
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None

    def _mark_failed(self) -> None:
        self._skip_until = time.monotonic() + self._cfg.qwen_skip_s
        self._failing = True

    async def synth(self, text: str) -> bytes:
        if self._http is None or time.monotonic() < self._skip_until:
            return await self._fallback.synth(text)
        t0 = time.monotonic()
        try:
            pcm = await self._request(text)
        except Exception as e:
            if not self._failing:
                log.warning("qwen3 synthesis failed; using say for the next %g s: %r", self._cfg.qwen_skip_s, e)
            self._mark_failed()
            return await self._fallback.synth(text)
        self._failing = False
        log.debug("qwen3 synth %d chars in %.2f s", len(text), time.monotonic() - t0)
        return pcm

    async def _request(self, text: str, timeout: float | None = None) -> bytes:
        cfg = self._cfg
        assert self._http is not None
        lang = cfg.qwen_lang_code
        body = {
            "model": cfg.qwen_model,
            "input": text,
            "voice": cfg.qwen_voices.get(lang, cfg.qwen_voice),
            "lang_code": lang,
            "response_format": "wav",
        }
        extra = {} if timeout is None else {"timeout": httpx.Timeout(timeout, connect=cfg.qwen_connect_timeout_s)}
        r = await self._http.post("/v1/audio/speech", json=body, **extra)
        r.raise_for_status()
        pcm = await self._to_16k(r.content)
        if not pcm:
            raise TtsError("server returned empty audio")
        return pcm

    async def _to_16k(self, wav: bytes) -> bytes:
        fd, src = tempfile.mkstemp(suffix=".wav", prefix="claude-voice-qwen-in-")
        os.close(fd)
        fd, dst = tempfile.mkstemp(suffix=".wav", prefix="claude-voice-qwen-out-")
        os.close(fd)
        proc = None
        try:
            Path(src).write_bytes(wav)
            proc = await asyncio.create_subprocess_exec(
                self._cfg.afconvert_bin, "-f", "WAVE", "-d", "LEI16@16000", "-c", "1", src, dst,
                stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE,
            )
            _, err = await asyncio.wait_for(proc.communicate(), self._cfg.tts_timeout_s)
            if proc.returncode != 0:
                raise TtsError(f"afconvert exited {proc.returncode}: {err.decode(errors='replace').strip()}")
            return wav_to_pcm(dst)
        except TimeoutError as e:
            raise TtsError("afconvert timed out") from e
        finally:
            if proc is not None and proc.returncode is None:
                proc.kill()
                await proc.wait()
            os.unlink(src)
            os.unlink(dst)
