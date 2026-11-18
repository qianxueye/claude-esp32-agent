"""Energy voice-activity detector with an adaptive noise floor."""

from __future__ import annotations

import enum
import math
from array import array

from .config import Config


class VadEvent(enum.Enum):
    NONE = 0
    SPEECH_START = 1
    END = 2


def rms(frame: bytes) -> float:
    samples = array("h")
    samples.frombytes(frame[: len(frame) & ~1])
    if not samples:
        return 0.0
    return math.sqrt(sum(s * s for s in samples) / len(samples))


class Vad:
    """Speech counts after min_speech_ms of voiced audio; it ends after end_silence_ms of silence."""

    def __init__(self, cfg: Config) -> None:
        self._cfg = cfg
        self.noise = math.inf
        self.started = False
        self._frames = 0
        self._voiced_ms = 0.0
        self._silence_ms = 0.0

    @property
    def threshold(self) -> float:
        return max(self._cfg.vad_min_rms, self.noise * self._cfg.vad_noise_factor)

    def feed(self, frame: bytes) -> VadEvent:
        cfg = self._cfg
        level = rms(frame)
        ms = (len(frame) // 2) * 1000 / 16000
        self._frames += 1
        if self._frames <= cfg.vad_noise_init_frames:
            self.noise = min(self.noise, level)
            return VadEvent.NONE

        voiced = level > self.threshold
        if not voiced and not self.started:
            alpha = 0.2 if level < self.noise else cfg.vad_noise_rise
            self.noise += (level - self.noise) * alpha

        if voiced:
            self._silence_ms = 0.0
            self._voiced_ms += ms
            if not self.started and self._voiced_ms >= cfg.vad_min_speech_ms:
                self.started = True
                return VadEvent.SPEECH_START
            return VadEvent.NONE

        self._silence_ms += ms
        if self.started:
            if self._silence_ms >= cfg.vad_end_silence_ms:
                return VadEvent.END
        elif self._silence_ms >= cfg.vad_end_silence_ms:
            self._voiced_ms = 0.0
        return VadEvent.NONE
