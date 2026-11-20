"""Wire protocol v3: frame encoding and a resynchronising decoder (docs/protocol.md)."""

from __future__ import annotations

import enum
import struct

SYNC = b"\xc1\xad"
MAX_PAYLOAD = 4096
PARTIAL_TIMEOUT = 0.2
SAMPLE_RATE = 16000
PROTO_VERSION = 3
WIFI_PORT = 47801
PSK_IDENTITY_PREFIX = "clawd-"
PAIR_PAYLOAD_MAX = 1023  # the device keeps 1024 bytes for the PAIR text and its terminator
PAIR_NETWORKS_MAX = 4


class T(enum.IntEnum):
    HELLO = 0x01
    LOG = 0x02
    MIC_AUDIO = 0x10
    INPUT = 0x20
    SPK_STATUS = 0x21
    VOLUME = 0x22
    PONG = 0x30
    SELFTEST_RESULT = 0x31
    SNAP_BEGIN = 0x32
    SNAP_DATA = 0x33
    SNAP_END = 0x34
    PAIR_RESULT = 0x35
    WIFI_STATUS = 0x36
    FORGET_RESULT = 0x37
    HOST_HELLO = 0x81
    SET_STATE = 0x82
    SET_VOLUME = 0x83
    PAIR = 0x84
    FORGET = 0x85
    SET_REMOTE = 0x86
    SPK_AUDIO = 0x90
    SPK_FLUSH = 0x91
    SPK_END = 0x92
    PING = 0xA0
    SELFTEST = 0xA2
    SNAPSHOT = 0xA3


class State(enum.IntEnum):
    IDLE = 0
    LISTENING = 1
    THINKING = 2
    SPEAKING = 3
    ERROR = 4
    OFFLINE = 5


class VolMode(enum.IntEnum):
    TRANSIENT = 0
    PERSIST = 1
    SEED = 2


class Input(enum.IntEnum):
    TAP = 1
    LONG_PRESS = 2
    BOOT_BUTTON = 3


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload {len(payload)} exceeds {MAX_PAYLOAD}")
    body = struct.pack("<BH", ftype, len(payload)) + payload
    return SYNC + body + struct.pack("<H", crc16(body))


class Decoder:
    """Stream decoder; time is injected so the 200 ms partial-frame rule is testable.

    `partial_timeout=None` disables the rule, as required over TCP.
    """

    def __init__(self, partial_timeout: float | None = PARTIAL_TIMEOUT) -> None:
        self._buf = bytearray()
        self._timeout = partial_timeout
        self._last_data = 0.0

    def feed(self, data: bytes, now: float) -> list[tuple[int, bytes]]:
        if data:
            self._buf += data
            self._last_data = now
        return self._scan(now)

    def poll(self, now: float) -> list[tuple[int, bytes]]:
        return self._scan(now)

    def _scan(self, now: float) -> list[tuple[int, bytes]]:
        frames: list[tuple[int, bytes]] = []
        buf = self._buf
        while True:
            i = buf.find(SYNC)
            if i < 0:
                # keep a trailing first-SYNC byte: the second one may arrive next
                keep = 1 if buf.endswith(SYNC[:1]) else 0
                del buf[: len(buf) - keep]
                break
            if i:
                del buf[:i]
            if len(buf) < 5:
                if self._expired(now):
                    del buf[:1]
                    continue
                break
            length = buf[3] | (buf[4] << 8)
            if length > MAX_PAYLOAD:
                del buf[:1]
                continue
            total = 5 + length + 2
            if len(buf) < total:
                if self._expired(now):
                    del buf[:1]
                    continue
                break
            body = bytes(buf[2 : 5 + length])
            if struct.unpack_from("<H", buf, 5 + length)[0] != crc16(body):
                del buf[:1]
                continue
            frames.append((buf[2], bytes(buf[5 : 5 + length])))
            del buf[:total]
            self._last_data = now
        return frames

    def _expired(self, now: float) -> bool:
        return self._timeout is not None and now - self._last_data > self._timeout
