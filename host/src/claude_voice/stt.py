"""Speech to text through a managed, kept-warm whisper-server child."""

from __future__ import annotations

import asyncio
import logging
import re
import socket
import subprocess
import time
from pathlib import Path
from typing import Protocol

import httpx

from .audio import pcm_to_wav, silence
from .config import Config

log = logging.getLogger("claude_voice.stt")
_NON_SPEECH = re.compile(r"^[\s\[\(（【].*[\]\)）】]\s*$")


class Stt(Protocol):
    async def transcribe(self, pcm: bytes) -> str: ...


class SttError(RuntimeError):
    pass


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def normalise(text: str) -> str:
    """Drop whisper's bracketed non-speech markers such as [BLANK_AUDIO]."""
    text = text.strip()
    return "" if not text or _NON_SPEECH.match(text) else text


class WhisperStt:
    def __init__(self, cfg: Config) -> None:
        self._cfg = cfg
        self._proc: subprocess.Popen | None = None
        self._log_file = None
        self._http: httpx.AsyncClient | None = None
        self.port = 0

    async def start(self) -> None:
        cfg = self._cfg
        if not cfg.whisper_model:
            raise SttError("whisper_model is not set: put the path of a ggml model file in host/config.toml")
        model = str(Path(cfg.whisper_model).expanduser())
        if not Path(model).is_file():
            raise SttError(f"whisper model not found: {model} (set whisper_model in host/config.toml)")
        if not Path(cfg.whisper_server).is_file():
            raise SttError(f"whisper-server not found: {cfg.whisper_server}")
        self.port = free_port()
        log_path = Path(cfg.log_dir)
        log_path.mkdir(parents=True, exist_ok=True)
        self._log_file = open(log_path / "whisper-server.log", "ab")
        args = [
            cfg.whisper_server, "-m", model, "--host", "127.0.0.1", "--port", str(self.port),
            "-l", cfg.whisper_language, "-t", str(cfg.whisper_threads), "-nt", "-sns",
        ]
        if cfg.whisper_prompt:
            args += ["--prompt", cfg.whisper_prompt]
        self._proc = subprocess.Popen(args, stdout=self._log_file, stderr=self._log_file, stdin=subprocess.DEVNULL)
        self._http = httpx.AsyncClient(base_url=f"http://127.0.0.1:{self.port}", timeout=cfg.whisper_timeout_s)
        t0 = time.monotonic()
        try:
            while True:
                if self._proc.poll() is not None:
                    raise SttError(f"whisper-server exited with {self._proc.returncode} (see whisper-server.log)")
                if time.monotonic() - t0 > cfg.whisper_ready_timeout_s:
                    raise SttError("whisper-server not ready in time")
                try:
                    r = await self._http.get("/", timeout=1.0)
                    if r.status_code < 500:
                        break
                except httpx.HTTPError:
                    pass
                await asyncio.sleep(0.5)
        except BaseException:
            await self.close()
            raise
        log.info("whisper-server ready on port %d after %.1f s", self.port, time.monotonic() - t0)
        await self.transcribe(silence(1000))

    async def transcribe(self, pcm: bytes) -> str:
        if self._http is None:
            raise SttError("whisper not started")
        try:
            r = await self._http.post(
                "/inference",
                files={"file": ("speech.wav", pcm_to_wav(pcm), "audio/wav")},
                data={"response_format": "json", "temperature": "0.0", "language": self._cfg.whisper_language},
            )
            r.raise_for_status()
            return normalise(r.json().get("text", ""))
        except (httpx.HTTPError, ValueError) as e:
            raise SttError(f"whisper request failed: {e!r}") from e

    async def close(self) -> None:
        if self._http is not None:
            await self._http.aclose()
            self._http = None
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
