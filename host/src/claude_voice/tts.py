"""Text to speech through macOS `say`, one sentence per call."""

from __future__ import annotations

import asyncio
import os
import tempfile
from typing import Protocol

from .audio import wav_to_pcm
from .config import Config


class Tts(Protocol):
    async def synth(self, text: str) -> bytes: ...


class TtsError(RuntimeError):
    pass


class SayTts:
    def __init__(self, cfg: Config) -> None:
        self._cfg = cfg

    async def synth(self, text: str) -> bytes:
        cfg = self._cfg
        fd, path = tempfile.mkstemp(suffix=".wav", prefix="claude-voice-")
        os.close(fd)
        args = [cfg.say_bin, "-v", cfg.voice]
        if cfg.rate:
            args += ["-r", str(cfg.rate)]
        args += ["--file-format=WAVE", "--data-format=LEI16@16000", "-o", path, "--", text]
        proc = await asyncio.create_subprocess_exec(
            *args, stdout=asyncio.subprocess.DEVNULL, stderr=asyncio.subprocess.PIPE
        )
        try:
            _, err = await asyncio.wait_for(proc.communicate(), cfg.tts_timeout_s)
            if proc.returncode != 0:
                raise TtsError(f"say exited {proc.returncode}: {err.decode(errors='replace').strip()}")
            return wav_to_pcm(path)
        except TimeoutError as e:
            raise TtsError("say timed out") from e
        finally:
            if proc.returncode is None:
                proc.kill()
                await proc.wait()
            os.unlink(path)
