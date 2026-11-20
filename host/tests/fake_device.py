"""Device side of wire protocol v3 over an in-process or TLS transport, for tests and the e2e run."""

from __future__ import annotations

import asyncio
import json
import math
import random
import ssl
import struct
import time
from array import array
from collections.abc import Awaitable, Callable

from claude_voice.link import MemoryTransport, Transport, TransportClosed
from claude_voice.protocol import Decoder, Input, State, T, VolMode, encode
from claude_voice.wifi import CIPHER, TcpTransport

BYTES_PER_S = 32000
TICK_BYTES = BYTES_PER_S // 10
MOD = 1 << 32
FAKE_SERIAL = "AA:BB:CC:DD:EE:FF"
ROM_TEXT = b"ESP-ROM:esp32s3-20210327\r\nBuild:Mar 27 2021\r\n\xc1\xc1\xad rst:0x1 (POWERON)\r\n"


def speech_pcm(lead_ms=400, speech_ms=900, amp=4000, freq=0.25) -> bytes:
    rng = random.Random(7)
    lead = array("h", (rng.randint(-30, 30) for _ in range(16 * lead_ms)))
    body = array("h", (int(amp * math.sin(i * freq)) + rng.randint(-30, 30) for i in range(16 * speech_ms)))
    return (lead + body).tobytes()


def client_context(identity: str, psk: bytes, *, cipher: str = CIPHER, tls13: bool = False) -> ssl.SSLContext:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    version = ssl.TLSVersion.TLSv1_3 if tls13 else ssl.TLSVersion.TLSv1_2
    ctx.minimum_version = ctx.maximum_version = version
    if not tls13:
        ctx.set_ciphers(cipher)
    ctx.set_psk_client_callback(lambda hint: (identity, psk))
    return ctx


async def tls_connect(port: int, identity: str, psk: bytes, **kw) -> TcpTransport:
    reader, writer = await asyncio.wait_for(
        asyncio.open_connection("127.0.0.1", port, ssl=client_context(identity, psk, **kw), server_hostname=None), 5
    )
    return TcpTransport(reader, writer, 1 << 20)


class FakeDevice:
    """Board model: queue, consumed counter, HELLO reasons, USB reset on open, PAIR/FORGET on USB."""

    def __init__(
        self,
        transport: Transport,
        speed: float = 1.0,
        mic: bytes = b"",
        *,
        link: str = "usb",
        proto: int = 3,
        vol: int | None = None,
        boot_delay: float | None = 0.05,
        consumed0: int = 0,
        relay_connector: Callable[[dict], Awaitable[str]] | None = None,
    ) -> None:
        self.t = transport
        self.speed = speed
        self.mic = mic
        self.link = link
        self.proto = proto
        self.boot_delay = boot_delay
        self.consumed0 = consumed0
        self.relay_connector = relay_connector
        self.leaky_log = False
        self.refuse_pair = False
        self.pair_reply = True  # False: save and join without sending PAIR_RESULT
        self.ignore_pair = False
        self.forget_reply: bytes | None = b'{"ok":true}'  # None: erase without FORGET_RESULT
        self.forget_erases = True
        self.wifi = "off"
        self.relay = "none"
        self.dec = Decoder(0.2 if link == "usb" else None)
        self.state = State.IDLE
        self.states: list[State] = []
        self.queued = 0
        self.consumed = consumed0
        self.max_queued = 0
        self.spk = bytearray()
        self.clips: list[int] = []
        self.ended = False
        self.flushes = 0
        self.hellos: list[str] = []
        self.host_hellos = 0
        self.pings = 0
        self.vol = 70 if vol is None else vol
        self.vol_set = vol is not None
        self.override: int | None = None
        self.mic_frames_sent = 0
        self.paired: dict | None = None
        self.forgets = 0
        self.received: list[int] = []
        self.set_volumes: list[bytes] = []
        self.remotes: list[dict] = []
        self._booting = False
        self._mic_pos = 0
        self._tasks: list[asyncio.Task] = []
        self._mic_task: asyncio.Task | None = None
        self._drain_reported = False

    def start(self) -> None:
        self._tasks = [asyncio.create_task(self._rx()), asyncio.create_task(self._player())]
        if self.link == "usb" and self.boot_delay is not None:
            self._booting = True
            self._tasks.append(asyncio.create_task(self._boot_after(self.boot_delay)))

    async def _boot_after(self, delay: float) -> None:
        await asyncio.sleep(delay)
        self.reset()

    async def stop(self) -> None:
        tasks = self._tasks + ([self._mic_task] if self._mic_task else [])
        for t in tasks:
            t.cancel()
        for t in tasks:
            try:
                await t
            except BaseException:
                pass

    def send(self, ftype: int, payload: bytes = b"") -> None:
        try:
            self.t.write(encode(ftype, payload))
        except TransportClosed:
            pass

    def hello(self, reason: str) -> None:
        self.hellos.append(reason)
        info = {"proto": self.proto, "fw": "fake", "rate": 16000, "reason": reason, "link": self.link,
                "paired": self.paired is not None, "vol": self.vol, "vol_set": self.vol_set}
        self.send(T.HELLO, json.dumps(info).encode())

    def reset(self) -> None:
        """Reboot: ROM text, then HELLO `boot`; the counters restart."""
        self._booting = False
        self.override = None
        self.state = State.IDLE
        self.queued = 0
        self.consumed = self.consumed0
        self._mic_pos = 0
        self.ended = False
        if self.link == "usb":
            self.t.write(ROM_TEXT)
        self.hello("boot")

    def inject(self, code: Input) -> None:
        self.send(T.INPUT, bytes([int(code)]))

    def status(self, flags: int) -> None:
        self.send(T.SPK_STATUS, struct.pack("<IBI", self.queued, flags, self.consumed % MOD))

    async def wait_state(self, state: State, timeout=5.0) -> None:
        async with asyncio.timeout(timeout):
            while self.state != state:
                await asyncio.sleep(0.005)

    async def _rx(self) -> None:
        try:
            while True:
                data = await self.t.read(0.05)
                for ftype, payload in self.dec.feed(data, time.monotonic()):
                    if not self._booting:
                        self._handle(ftype, payload)
        except TransportClosed:
            return

    def _handle(self, ftype: int, payload: bytes) -> None:
        self.received.append(ftype)
        if ftype == T.HOST_HELLO:
            self.override = None
            self.host_hellos += 1
            self.hello("reply")
            self.wifi_status()
        elif ftype == T.PING:
            self.pings += 1
            self.send(T.PONG, payload)
        elif ftype == T.SET_STATE:
            new = State(payload[0])
            self.states.append(new)
            if new == State.LISTENING and self.state != State.LISTENING:
                self._mic_pos = 0
                self._mic_task = asyncio.create_task(self._mic_loop())
            self.state = new
        elif ftype == T.SET_VOLUME:
            self._set_volume(payload)
        elif ftype == T.SET_REMOTE:
            self.remotes.append(json.loads(payload))
        elif ftype == T.SPK_AUDIO:
            self.queued += len(payload)
            self.max_queued = max(self.max_queued, self.queued)
            self.spk += payload
            self.ended = False
            self._drain_reported = False
        elif ftype == T.SPK_FLUSH:
            self.flushes += 1
            self.consumed += self.queued
            self.queued = 0
            self.ended = False
            self.status(0)
        elif ftype == T.SPK_END:
            self.ended = True
            self.clips.append(len(self.spk))
        elif ftype == T.PAIR and self.link == "usb":
            self._pair(payload)
        elif ftype == T.FORGET and self.link == "usb":
            self.forgets += 1
            if self.forget_erases:
                self.paired = None
                if (self.wifi, self.relay) != ("off", "none"):
                    self.wifi_status("off", "none")
            if self.forget_reply is not None:
                self.send(T.FORGET_RESULT, self.forget_reply)

    def _set_volume(self, payload: bytes) -> None:
        v, mode = min(payload[0], 100), payload[1] if len(payload) > 1 else VolMode.TRANSIENT
        if mode == VolMode.TRANSIENT:
            self.override = v
        elif mode == VolMode.PERSIST or (mode == VolMode.SEED and not self.vol_set):
            self.vol, self.vol_set, self.override = v, True, None
            self.send(T.VOLUME, bytes([v]))
        self.set_volumes.append(bytes(payload))

    def knob(self, v: int) -> None:
        """The user turned the knob: the board keeps and reports v."""
        self.vol, self.vol_set, self.override = v, True, None
        self.send(T.VOLUME, bytes([v]))

    def wifi_status(self, wifi: str | None = None, relay: str | None = None) -> None:
        """Send WIFI_STATUS, first recording a new state when one is given."""
        self.wifi = wifi or self.wifi
        self.relay = relay or self.relay
        body = {"wifi": self.wifi, "ip": "10.9.8.7" if self.wifi == "connected" else "", "rssi": -50, "relay": self.relay}
        self.send(T.WIFI_STATUS, json.dumps(body).encode())

    def _pair(self, payload: bytes) -> None:
        if self.ignore_pair:
            return
        try:
            info = json.loads(payload)
            ok = len(bytes.fromhex(info["psk"])) == 32 and bool(info["ssid"]) and bool(info["relay_port"])
        except (ValueError, KeyError, TypeError):
            ok = False
        if not ok:
            self.send(T.PAIR_RESULT, b'{"ok":false,"error":"bad payload"}')
            return
        self.send(T.LOG, f"pair: ssid={info['ssid']}".encode())
        if self.leaky_log:
            self.send(T.LOG, f"careless: {info['password']} {info['psk']}".encode())
        if self.refuse_pair:
            self.send(T.PAIR_RESULT, b'{"ok":false,"error":"nvs full"}')
            return
        self.paired = info
        if self.pair_reply:
            self.send(T.PAIR_RESULT, b'{"ok":true}')
        self._tasks.append(asyncio.create_task(self._join(info)))

    async def _join(self, info: dict) -> None:
        self.wifi_status("connecting", "none")
        await asyncio.sleep(0.01)
        self.wifi_status("connected", "connecting")
        outcome = await self.relay_connector(info) if self.relay_connector else "connected"
        self.wifi_status("connected", outcome)

    async def _player(self) -> None:
        """Drain the queue in real time and report SPK_STATUS every 100 ms."""
        tick = 0.1 / self.speed
        while True:
            await asyncio.sleep(tick)
            if self.queued > 0:
                played = min(self.queued, TICK_BYTES)
                self.queued -= played
                self.consumed += played
                self.status(1)
            elif self.ended and not self._drain_reported:
                self._drain_reported = True
                self.status(2)

    async def _mic_loop(self) -> None:
        tick = 0.02 / self.speed
        while self.state == State.LISTENING:
            chunk = self.mic[self._mic_pos : self._mic_pos + 640]
            self._mic_pos += 640
            if len(chunk) < 640:
                chunk += bytes(640 - len(chunk))
            self.mic_frames_sent += 1
            self.send(T.MIC_AUDIO, chunk)
            await asyncio.sleep(tick)


__all__ = ["FAKE_SERIAL", "FakeDevice", "MemoryTransport", "client_context", "speech_pcm", "tls_connect"]
