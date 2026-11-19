"""PCM helpers and generated earcons (s16le mono 16 kHz)."""

from __future__ import annotations

import io
import math
import wave
from array import array

from .protocol import SAMPLE_RATE


def pcm_to_wav(pcm: bytes, rate: int = SAMPLE_RATE) -> bytes:
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)
    return out.getvalue()


def wav_to_pcm(data: bytes | str) -> bytes:
    src = io.BytesIO(data) if isinstance(data, bytes) else data
    with wave.open(src, "rb") as w:
        if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (1, 2, SAMPLE_RATE):
            raise ValueError(f"expected 16 kHz mono s16 WAV, got {w.getparams()}")
        return w.readframes(w.getnframes())


def duration_s(pcm: bytes) -> float:
    return len(pcm) / 2 / SAMPLE_RATE


def tone(freq: float, ms: int, gain: float, fade_ms: int = 8) -> bytes:
    n = SAMPLE_RATE * ms // 1000
    fade = max(1, SAMPLE_RATE * fade_ms // 1000)
    samples = array("h")
    for i in range(n):
        env = min(1.0, i / fade, (n - 1 - i) / fade)
        samples.append(int(32767 * gain * env * math.sin(2 * math.pi * freq * i / SAMPLE_RATE)))
    return samples.tobytes()


def silence(ms: int) -> bytes:
    return bytes(2 * (SAMPLE_RATE * ms // 1000))


def earcons(gain: float) -> dict[str, bytes]:
    return {
        "listen_start": tone(660, 70, gain) + tone(880, 90, gain),
        "listen_end": tone(880, 60, gain) + tone(587, 90, gain),
        "error": tone(220, 140, gain) + silence(60) + tone(165, 220, gain),
    }
