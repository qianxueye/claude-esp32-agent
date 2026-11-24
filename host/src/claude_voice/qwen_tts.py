"""Qwen3-TTS through an mlx-audio speech server, here or on another Mac over ssh, with `say` as the fallback."""

from __future__ import annotations

import asyncio
import base64
import contextlib
import json
import logging
import math
import os
import subprocess
import tempfile
import threading
import time
from collections.abc import Callable
from pathlib import Path

import httpx

from .audio import wav_to_pcm
from .cloudflared import Backoff, reaped
from .config import Config
from .stt import free_port
from .tts import SayTts, TtsError

log = logging.getLogger("claude_voice.qwen_tts")

WARMUP_TEXT = "你好。"
GUARD = Path(__file__).with_name("speech_guard.py")
TAG = "clawd-speech"  # prefix of the guard's status lines; other stdout lines go to the server log
# runs the guard from argv; it holds no quote or backslash, so every login shell parses it alike
BOOTSTRAP = "import sys,base64;exec(base64.b64decode(sys.argv[1]))"
GRACE_S = 5.0  # the guard's SIGTERM-to-SIGKILL wait for the server group
STOP_MARGIN_S = 2.0  # the relay waits GRACE_S + this for ssh, so it never cuts the guard's stop short
BEATS = 5  # heartbeats per qwen_link_timeout_s; ssh's ServerAliveCountMax uses the same count
STATUS_POLL_S = 0.2
DRAIN_S = 1.0  # after ssh exits its output is already in the pipe; this only bounds a stuck reader
FORWARDS = ("localforward", "remoteforward", "dynamicforward")  # keywords of `ssh -G` output


class Outage(TtsError):
    """Why an ssh session failed; `reason` groups failures for logging, `level` is the first report's level."""

    def __init__(self, reason: str, message: str, level: int = logging.WARNING) -> None:
        super().__init__(message)
        self.reason = reason
        self.level = level


def heartbeat(pipe, interval_s: float, stop: threading.Event) -> None:
    """Write one byte per interval until stopped, then close `pipe`; owns it, so only this thread closes it.
    Writes never block: a full pipe skips the beat, so a stop is never held up by ssh."""
    fd = pipe.fileno()
    os.set_blocking(fd, False)
    try:
        while not stop.wait(interval_s):
            try:
                os.write(fd, b".")
            except BlockingIOError:
                pass
            except OSError:
                return
    finally:
        pipe.close()


def read_lines(pipe, loop: asyncio.AbstractEventLoop, on_line: Callable, stop: threading.Event) -> None:
    """Hand each line to the loop, then None at EOF; owns `pipe`, so only this thread closes it."""
    try:
        for line in iter(pipe.readline, b""):
            if not stop.is_set():
                loop.call_soon_threadsafe(on_line, line)
        if not stop.is_set():
            loop.call_soon_threadsafe(on_line, None)
    except (RuntimeError, OSError, ValueError):  # RuntimeError: the loop has closed
        pass
    finally:
        pipe.close()


class QwenTts:
    def __init__(
        self, cfg: Config, fallback: SayTts, transport: httpx.AsyncBaseTransport | None = None,
        grace_s: float = GRACE_S, stop_margin_s: float = STOP_MARGIN_S,
    ) -> None:
        self._cfg = cfg
        self._fallback = fallback
        self._transport = transport
        self._grace = grace_s
        self._stop_margin = stop_margin_s
        self._http: httpx.AsyncClient | None = None
        self._proc: subprocess.Popen | None = None
        self._ssh: subprocess.Popen[bytes] | None = None
        self._supervisor: asyncio.Task | None = None
        self._beat_stop = threading.Event()
        self._threads: list[threading.Thread] = []
        self._warned = False
        self._log_file = None
        self._skip_until = 0.0
        self._failing = False

    async def start(self) -> None:
        """Bring the server up; never raises, so a broken server leaves the relay on `say`."""
        cfg = self._cfg
        if cfg.qwen_ssh_host:
            log.info("starting qwen3 speech server on %s over ssh; using say until it is ready", cfg.qwen_ssh_host)
            self._supervisor = asyncio.get_running_loop().create_task(self._supervise())
            return
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
        return httpx.AsyncClient(base_url=base, timeout=t, transport=self._transport, trust_env=self._trust_env())

    def _trust_env(self) -> bool:
        """Proxy settings apply except to the ssh forward, which carries reply text that must stay in the tunnel."""
        return not self._cfg.qwen_ssh_host

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
        await self._probe(base, self._proc, "server")
        return base

    async def _probe(self, base: str, proc: subprocess.Popen, name: str) -> None:
        """Poll `base` until it answers below HTTP 500; fails when `proc` exits or the ready timeout passes."""
        t0 = time.monotonic()
        last = "no response"
        async with httpx.AsyncClient(timeout=1.0, trust_env=self._trust_env()) as probe:
            while True:
                if proc.poll() is not None:
                    raise TtsError(f"{name} exited with {proc.returncode} (see qwen-tts-server.log)")
                if time.monotonic() - t0 > self._cfg.qwen_ready_timeout_s:
                    raise TtsError(f"{name} not ready in time; last probe: {last}")
                try:
                    status = (await probe.get(base + "/")).status_code
                    if status < 500:
                        return
                    last = f"HTTP {status}"
                except httpx.HTTPError as e:
                    last = repr(e)
                await asyncio.sleep(0.5)

    async def _drop_client(self) -> None:
        http, self._http = self._http, None
        if http is not None:
            await http.aclose()

    def _open_log(self):
        if self._log_file is None:
            log_dir = Path(self._cfg.log_dir)
            log_dir.mkdir(parents=True, exist_ok=True)
            self._log_file = open(log_dir / "qwen-tts-server.log", "ab")
        return self._log_file

    def _ssh_argv(self, lport: int) -> list[str]:
        cfg = self._cfg
        forward = f"127.0.0.1:{lport}:127.0.0.1:{cfg.qwen_remote_port}"
        return [cfg.qwen_ssh_bin, "-T", *self._ssh_opts(), "-L", forward, "--", cfg.qwen_ssh_host, self._remote_cmd()]

    def _ssh_opts(self) -> list[str]:
        cfg = self._cfg
        opts = {
            "BatchMode": "yes", "StrictHostKeyChecking": "yes", "UpdateHostKeys": "no", "ExitOnForwardFailure": "yes",
            "ConnectTimeout": math.ceil(cfg.qwen_ssh_connect_timeout_s),
            "ServerAliveInterval": max(1, math.ceil(cfg.qwen_link_timeout_s / BEATS)),
            "ServerAliveCountMax": BEATS,
            "ControlMaster": "no", "ControlPath": "none", "RemoteCommand": "none", "RequestTTY": "no",
            "SessionType": "default", "StdinNull": "no", "ForkAfterAuthentication": "no",
            "ForwardAgent": "no", "ForwardX11": "no", "PermitLocalCommand": "no",
        }
        return [arg for key, value in opts.items() for arg in ("-o", f"{key}={value}")]

    async def _check_forwards(self, log_file) -> None:
        """Refuse an alias whose config adds forwards; `ssh -G` prints the effective config without connecting."""
        cfg = self._cfg
        proc = await asyncio.create_subprocess_exec(
            cfg.qwen_ssh_bin, "-G", *self._ssh_opts(), "--", cfg.qwen_ssh_host,
            stdin=asyncio.subprocess.DEVNULL, stdout=asyncio.subprocess.PIPE, stderr=log_file,
        )
        try:
            out, _ = await asyncio.wait_for(proc.communicate(), cfg.qwen_ssh_connect_timeout_s)
        except TimeoutError as e:
            raise TtsError("ssh -G did not finish in time") from e
        finally:
            if proc.returncode is None:
                proc.kill()
                await proc.wait()
        if proc.returncode != 0:
            raise TtsError(f"ssh -G exited with {proc.returncode} (see qwen-tts-server.log)")
        for line in out.decode(errors="replace").splitlines():
            word = line.split(maxsplit=1)[0].lower() if line.strip() else ""
            if word in FORWARDS:
                raise Outage(
                    "forward", f"the ssh config of {cfg.qwen_ssh_host} sets {word}; remove it from that alias",
                    logging.ERROR,
                )

    def _remote_cmd(self) -> str:
        """The remote shell's command: fixed text, the validated python path and two base64 words."""
        cfg = self._cfg
        port = cfg.qwen_remote_port
        payload = {
            "cmd": [*cfg.qwen_server_cmd, "--host", "127.0.0.1", "--port", str(port)], "port": port,
            "deadline_s": cfg.qwen_link_timeout_s, "grace_s": self._grace,
        }
        guard = base64.b64encode(GUARD.read_bytes()).decode()
        data = base64.b64encode(json.dumps(payload).encode()).decode()
        return f'exec {cfg.qwen_remote_python} -I -c "{BOOTSTRAP}" {guard} {data}'

    async def _supervise(self) -> None:
        cfg = self._cfg
        backoff = Backoff(cfg.qwen_restart_min_s, cfg.qwen_restart_max_s)
        while True:
            started = time.monotonic()
            try:
                await self._attempt()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                if not self._warned:
                    self._warned = True
                    log.log(getattr(e, "level", logging.WARNING),
                            "qwen3 speech on %s unavailable; using say and retrying: %s", cfg.qwen_ssh_host, e)
                else:
                    log.debug("qwen3 speech attempt failed: %s", e)
            finally:
                self._send_eof()
                await self._drop_client()
                await self._stop_ssh()
            delay = backoff.after(time.monotonic() - started)
            log.info("retrying qwen3 speech on %s in %g s", cfg.qwen_ssh_host, delay)
            await asyncio.sleep(delay)

    async def _attempt(self) -> None:
        """One ssh session: wait for the server, warm up, serve until ssh exits; always ends by raising."""
        cfg = self._cfg
        log_file = self._open_log()
        log_file.write(f"--- qwen3 speech over ssh, {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n".encode())
        log_file.flush()
        await self._check_forwards(log_file)
        lport = free_port()
        statuses: list[str] = []
        changed, eof = asyncio.Event(), asyncio.Event()

        def on_line(line: bytes | None) -> None:
            if line is not None and line.startswith(TAG.encode() + b" "):
                statuses.append(line[len(TAG) + 1:].decode(errors="replace").strip())
            elif line is not None and self._log_file is not None:
                self._log_file.write(line if line.endswith(b"\n") else line + b"\n")
                self._log_file.flush()
            if line is None:
                eof.set()
            changed.set()

        self._beat_stop = stop = threading.Event()
        # no await between the fork and `self._ssh`, so a cancellation can never orphan a started ssh
        self._ssh = proc = subprocess.Popen(
            self._ssh_argv(lport), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log_file,
            start_new_session=True,
        )
        loop = asyncio.get_running_loop()
        self._threads = [
            threading.Thread(target=heartbeat, args=(proc.stdin, cfg.qwen_link_timeout_s / BEATS, stop), daemon=True),
            threading.Thread(target=read_lines, args=(proc.stdout, loop, on_line, stop), daemon=True),
        ]
        for t in self._threads:
            t.start()
        deadline = time.monotonic() + cfg.qwen_ready_timeout_s
        while "listening" not in statuses:
            for s in statuses:
                if s.startswith(("busy", "exit", "nolsof")):
                    raise TtsError(self._ended(proc, statuses))
            if proc.poll() is not None:
                await self._drain(eof)
                raise TtsError(self._ended(proc, statuses))
            if time.monotonic() > deadline:
                raise TtsError("speech server did not listen in time (see qwen-tts-server.log)")
            changed.clear()
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(changed.wait(), STATUS_POLL_S)
        base = f"http://127.0.0.1:{lport}"
        await self._probe(base, proc, "ssh")
        http = self._client(base)
        try:
            t0 = time.monotonic()
            await self._request(WARMUP_TEXT, timeout=cfg.qwen_ready_timeout_s, http=http)
        except BaseException:
            await http.aclose()
            raise
        self._http = http
        self._skip_until = 0.0
        self._failing = False
        self._warned = False
        log.info("qwen3 speech server on %s ready, warm-up %.1f s", cfg.qwen_ssh_host, time.monotonic() - t0)
        await reaped(proc)
        await self._drain(eof)
        raise TtsError(self._ended(proc, statuses))

    @staticmethod
    async def _drain(eof: asyncio.Event) -> None:
        """Wait for the status lines ssh wrote before it exited, so the error names the guard's reason."""
        with contextlib.suppress(TimeoutError):
            await asyncio.wait_for(eof.wait(), DRAIN_S)

    def _ended(self, proc: subprocess.Popen, statuses: list[str]) -> str:
        for s in reversed(statuses):
            if s == "nolsof":
                return f"{self._cfg.qwen_ssh_host} has no lsof, so the speech server's port cannot be checked"
            if s.startswith("busy"):
                return f"port {s.split()[-1]} on {self._cfg.qwen_ssh_host} is in use, maybe by an earlier server"
            if s.startswith("exit"):
                return f"speech server exited with {s.split()[-1]} (see qwen-tts-server.log)"
        return f"ssh exited with {proc.poll()} (see qwen-tts-server.log)"

    def _send_eof(self) -> None:
        """Stop the heartbeat, which then closes ssh's stdin; from here the guard stops the server by itself."""
        self._beat_stop.set()

    async def _stop_ssh(self) -> None:
        """Stop and reap ssh; the handle is cleared only once reaped, so `close()` repeats a cut-off stop."""
        proc = self._ssh
        if proc is None:
            return
        self._send_eof()
        if not await reaped(proc, self._grace + self._stop_margin):
            proc.terminate()
            if not await reaped(proc, self._stop_margin):
                proc.kill()
                await reaped(proc)
        self._ssh = None

    async def close(self) -> None:
        self._send_eof()
        task, self._supervisor = self._supervisor, None
        if task is not None:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        await self._drop_client()
        await self._stop_ssh()
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
        if self._proc is not None and self._proc.poll() is not None:
            log.error(
                "qwen3 speech server exited with %s; using say until the relay restarts (see qwen-tts-server.log)",
                self._proc.returncode,
            )
            await self._stop_server()
            await self._drop_client()
        ssh = self._ssh  # the supervisor notices an exit only at its next poll, and it owns the cleanup
        if self._http is None or time.monotonic() < self._skip_until or (ssh is not None and ssh.poll() is not None):
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

    async def _request(
        self, text: str, timeout: float | None = None, http: httpx.AsyncClient | None = None
    ) -> bytes:
        cfg = self._cfg
        if http is None:
            http = self._http
        assert http is not None
        lang = cfg.qwen_lang_code
        body = {
            "model": cfg.qwen_model,
            "input": text,
            "voice": cfg.qwen_voices.get(lang, cfg.qwen_voice),
            "lang_code": lang,
            "response_format": "wav",
        }
        extra = {} if timeout is None else {"timeout": httpx.Timeout(timeout, connect=cfg.qwen_connect_timeout_s)}
        r = await http.post("/v1/audio/speech", json=body, **extra)
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
