"""cloudflared Quick Tunnel to the tunnel endpoint: start, learn the public hostname, restart with backoff."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from collections.abc import Callable
from pathlib import Path

from .config import Config
from .tunnel import PATH

log = logging.getLogger("claude_voice.cloudflared")
HOSTNAME = re.compile(r"[a-z0-9-]{1,63}\.trycloudflare\.com")
# the banner URL is a whole token, so `xhttps://…`, `….trycloudflare.com.evil` or `…/path` never match
BANNER = re.compile(r"(?<![^\s|])https://([a-z0-9-]{1,63}\.trycloudflare\.com)(?=\s|$)", re.MULTILINE)
POLL_S = 0.2
GRACE_S = 3.0  # the guard's SIGTERM-to-SIGKILL wait for cloudflared
STOP_MARGIN_S = 2.0  # the relay waits GRACE_S + this for the guard, so it never kills the guard first
BANNER_READ_MAX = 256 * 1024  # bytes of one run's log searched for the banner
HOST_CHARS = 80  # longest refused hostname kept for logs
EMPTY_CONFIG = "{}\n"  # content of the file passed as `--config`
GUARD = str(Path(__file__).with_name("cloudflared_guard.py"))  # holds cloudflared; stops it when the relay's pipe closes
ENV_PREFIX = "TUNNEL_"  # cloudflared reads its flags from these variables


def child_env() -> dict[str, str]:
    return {k: v for k, v in os.environ.items() if not k.startswith(ENV_PREFIX)}


def hostname_from_banner(text: str) -> str | None:
    m = BANNER.search(text)
    return m.group(1) if m else None


def remote_url(host: str) -> str:
    return f"wss://{host}{PATH}"


class Backoff:
    """Delay before the next start: doubles up to the maximum, back to the minimum after a long run."""

    def __init__(self, min_s: float, max_s: float) -> None:
        self._min, self._max = min_s, max_s
        self._next = min_s

    def after(self, uptime_s: float) -> float:
        if uptime_s > self._max:
            self._next = self._min
        delay = self._next
        self._next = min(delay * 2, self._max)
        return delay


class Cloudflared:
    """Supervises one cloudflared child; never raises into the relay, which keeps working without it."""

    def __init__(
        self, cfg: Config, port: int, on_url: Callable[[str | None], None], grace_s: float = GRACE_S
    ) -> None:
        self._cfg = cfg
        self._port = port
        self._on_url = on_url
        self._grace = grace_s
        self._proc: asyncio.subprocess.Process | None = None
        self._spawn: asyncio.Task[asyncio.subprocess.Process] | None = None
        self._log_file = None
        self._log_path = Path(cfg.log_dir) / "cloudflared.log"
        self._task: asyncio.Task | None = None
        self._logged: str | None = None
        self._config_dir: str | None = None
        self.url: str | None = None

    def start(self) -> None:
        self._task = asyncio.get_running_loop().create_task(self._supervise())

    async def _supervise(self) -> None:
        cfg = self._cfg
        backoff = Backoff(cfg.cloudflared_restart_min_s, cfg.cloudflared_restart_max_s)
        while True:
            started = time.monotonic()
            try:
                await self._run_once()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.warning("cloudflared failed: %s (see cloudflared.log)", e)
            finally:
                await self._stop_child()
                self._set_url(None)
            delay = backoff.after(time.monotonic() - started)
            log.info("restarting cloudflared in %g s", delay)
            await asyncio.sleep(delay)

    async def _run_once(self) -> None:
        cfg = self._cfg
        if self._log_file is None:
            self._log_path.parent.mkdir(parents=True, exist_ok=True)
            self._log_file = open(self._log_path, "ab")
        offset = self._log_file.seek(0, 2)
        config = self._write_config()
        args = [
            sys.executable, "-I", GUARD, f"{self._grace:g}", cfg.cloudflared_bin,
            "tunnel", "--no-autoupdate", "--config", config, "--url", f"http://127.0.0.1:{self._port}",
        ]
        # the spawn lives on `self` so a cancelled run never drops a started guard; `_stop_child` reaps it
        self._spawn = asyncio.get_running_loop().create_task(asyncio.create_subprocess_exec(
            *args, stdin=subprocess.PIPE, stdout=self._log_file, stderr=self._log_file, env=child_env(),
            start_new_session=True,
        ))
        self._proc = await asyncio.shield(self._spawn)
        self._spawn = None
        host = await self._wait_hostname(offset)
        self._set_url(remote_url(host))
        code = await self._proc.wait()
        log.warning("cloudflared exited with %s (see cloudflared.log)", code)

    def _write_config(self) -> str:
        """Rewrite the empty config in the supervisor's private directory and return its path."""
        if self._config_dir is None or not os.path.isdir(self._config_dir):
            self._config_dir = tempfile.mkdtemp(prefix="clawd-cloudflared-")
        path = os.path.join(self._config_dir, "config.yml")
        with open(path, "w", encoding="utf-8") as f:
            f.write(EMPTY_CONFIG)
        return path

    async def _wait_hostname(self, offset: int) -> str:
        """The hostname from this run's own banner, read from the log after `offset`."""
        assert self._proc is not None
        deadline = time.monotonic() + self._cfg.cloudflared_url_timeout_s
        refused: set[str] = set()
        while True:
            if self._proc.returncode is not None:
                raise RuntimeError(f"exited with {self._proc.returncode} before the tunnel was ready")
            found = self._from_banner(offset)
            if found is not None:
                if HOSTNAME.fullmatch(found):
                    return found
                if found not in refused:
                    refused.add(found)
                    log.warning("cloudflared hostname %r is not a Quick Tunnel host; ignored", found[:HOST_CHARS])
            if time.monotonic() > deadline:
                raise RuntimeError(f"no tunnel hostname within {self._cfg.cloudflared_url_timeout_s:g} s")
            await asyncio.sleep(POLL_S)

    def _from_banner(self, offset: int) -> str | None:
        try:
            with open(self._log_path, "rb") as f:
                f.seek(offset)
                text = f.read(BANNER_READ_MAX).decode(errors="replace")
        except OSError:
            return None
        return hostname_from_banner(text)

    def _set_url(self, url: str | None) -> None:
        if url == self.url:
            return
        self.url = url
        if url is not None and url != self._logged:
            self._logged = url
            log.info("cloudflared tunnel ready: %s", url)
        try:
            self._on_url(url)
        except Exception as e:
            log.error("remote URL update failed: %r", e)

    async def _stop_child(self) -> None:
        """Stop the guard, waiting for an unfinished spawn first; the handles are cleared only once it is reaped."""
        if self._spawn is not None:
            with contextlib.suppress(Exception):  # a failed spawn was already reported by `_run_once`
                self._proc = await asyncio.shield(self._spawn)
            self._spawn = None
        proc = self._proc
        if proc is None or proc.returncode is not None:
            self._proc = None
            return
        with contextlib.suppress(ProcessLookupError):
            proc.terminate()
        try:
            await asyncio.wait_for(asyncio.shield(proc.wait()), self._grace + STOP_MARGIN_S)
        except TimeoutError:
            with contextlib.suppress(ProcessLookupError):
                proc.kill()
            await proc.wait()
        self._proc = None

    async def close(self) -> None:
        task, self._task = self._task, None
        if task is not None:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        await self._stop_child()
        if self._log_file is not None:
            self._log_file.close()
            self._log_file = None
        if self._config_dir is not None:
            shutil.rmtree(self._config_dir, ignore_errors=True)
            self._config_dir = None
