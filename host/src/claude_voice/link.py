"""Device link: USB and Wi-Fi sessions, active-link choice, handshake and ping (docs/protocol.md)."""

from __future__ import annotations

import asyncio
import json
import logging
import struct
import threading
import time
from collections.abc import Callable
from typing import Protocol

from .config import Config
from .protocol import PARTIAL_TIMEOUT, PROTO_VERSION, Decoder, State, T, encode

log = logging.getLogger("claude_voice.link")


class TransportClosed(Exception):
    pass


class Transport(Protocol):
    async def read(self, timeout: float) -> bytes:
        """Return received bytes, or b'' on timeout; raise TransportClosed when gone."""

    def write(self, data: bytes) -> bool:
        """Write a whole frame or nothing; False when it was dropped."""

    async def close(self) -> None: ...


class Handler(Protocol):
    def on_connect(self) -> None: ...
    def on_disconnect(self) -> None: ...
    def on_hello(self, reason: str, info: dict) -> None: ...
    def on_frame(self, ftype: int, payload: bytes) -> None: ...
    def current_state(self) -> int: ...


Opener = Callable[[], "Transport | None"]


class MemoryTransport:
    """In-process byte pipe end, used with the fake device."""

    def __init__(self) -> None:
        self.rx: asyncio.Queue[bytes | None] = asyncio.Queue()
        self.peer: MemoryTransport | None = None
        self.closed = False

    async def read(self, timeout: float) -> bytes:
        if self.closed and self.rx.empty():
            raise TransportClosed
        try:
            item = await asyncio.wait_for(self.rx.get(), timeout)
        except TimeoutError:
            return b""
        if item is None:
            raise TransportClosed
        return item

    def write(self, data: bytes) -> bool:
        if self.closed:
            raise TransportClosed
        if self.peer is not None and not self.peer.closed:
            self.peer.rx.put_nowait(data)
        return True

    async def close(self) -> None:
        self.closed = True
        self.rx.put_nowait(None)
        if self.peer is not None and not self.peer.closed:
            self.peer.closed = True
            self.peer.rx.put_nowait(None)


def memory_pair() -> tuple[MemoryTransport, MemoryTransport]:
    a, b = MemoryTransport(), MemoryTransport()
    a.peer, b.peer = b, a
    return a, b


def list_boards(cfg: Config) -> list[tuple[str, str]]:
    """(port, USB serial) of every connected board; only the configured serial when one is set."""
    from serial.tools import list_ports

    want = cfg.device_serial.lower()
    found = []
    for p in list_ports.comports():
        serial_number = p.serial_number or ""
        if p.vid == cfg.usb_vid and p.pid == cfg.usb_pid and (not want or serial_number.lower() == want):
            found.append((p.device, serial_number))
    return found


def find_port(cfg: Config) -> str | None:
    boards = list_boards(cfg)
    if len(boards) > 1:
        log.error("%d boards connected; set device_serial in config.toml", len(boards))
        return None
    return boards[0][0] if boards else None


class SerialTransport:
    """pyserial port read by a helper thread; DTR and RTS stay deasserted across the open."""

    def __init__(self, port: str) -> None:
        import serial

        self._loop = asyncio.get_running_loop()
        self._rx: asyncio.Queue[bytes | None] = asyncio.Queue()
        self._ser = serial.Serial()
        self._ser.port = port
        self._ser.baudrate = 115200
        self._ser.timeout = 0.1
        self._ser.write_timeout = 0.5
        self._ser.exclusive = True
        self._ser.dtr = False
        self._ser.rts = False
        self._ser.open()
        self._ser.dtr = False
        self._ser.rts = False
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._reader, name="serial-rx", daemon=True)
        self._thread.start()

    def _reader(self) -> None:
        try:
            while not self._stop.is_set():
                data = self._ser.read(4096)
                if data:
                    self._loop.call_soon_threadsafe(self._rx.put_nowait, data)
        except Exception as e:  # port vanished
            log.info("serial read ended: %s", e)
        try:
            self._loop.call_soon_threadsafe(self._rx.put_nowait, None)
        except RuntimeError:
            pass  # loop already closed

    async def read(self, timeout: float) -> bytes:
        try:
            item = await asyncio.wait_for(self._rx.get(), timeout)
        except TimeoutError:
            return b""
        if item is None:
            self._rx.put_nowait(None)
            raise TransportClosed
        return item

    def write(self, data: bytes) -> bool:
        try:
            self._ser.write(data)
        except Exception as e:
            raise TransportClosed(type(e).__name__) from None
        return True

    async def close(self) -> None:
        self._stop.set()
        try:
            self._ser.close()
        except Exception:
            pass
        await asyncio.to_thread(self._thread.join, 1.0)


def serial_opener(cfg: Config) -> Opener:
    def open_port() -> Transport | None:
        port = find_port(cfg)
        if port is None:
            return None
        return SerialTransport(port)

    return open_port


def parse_hello(payload: bytes) -> dict:
    try:
        info = json.loads(payload)
    except ValueError:
        return {}
    return info if isinstance(info, dict) else {}


class Session:
    def __init__(self, kind: str, transport: Transport, label: str | None = None) -> None:
        self.kind = kind
        self.label = label or kind  # names the session in logs only
        self.t = transport
        self.dec = Decoder(PARTIAL_TIMEOUT if kind == "usb" else None)
        self.got_hello = False
        self.ready = False
        self.refused = False
        self.done = asyncio.Event()


class Link:
    """Holds at most one board session; USB is preferred, Wi-Fi is used only while USB is closed."""

    def __init__(self, cfg: Config, opener: Opener, handler: Handler) -> None:
        self._cfg = cfg
        self._opener = opener
        self._h = handler
        self._active: Session | None = None
        self._switch = asyncio.Lock()
        self._nonce = 0

    @property
    def connected(self) -> bool:
        s = self._active
        return s is not None and s.ready

    @property
    def active_kind(self) -> str | None:
        s = self._active
        return s.kind if s is not None else None

    def send(self, ftype: int, payload: bytes = b"") -> bool:
        s = self._active
        if s is None or not s.ready:
            return False
        return self._write(s, ftype, payload)

    def send_state(self, state: int) -> None:
        self.send(T.SET_STATE, bytes([int(state)]))

    def _write(self, s: Session, ftype: int, payload: bytes = b"") -> bool:
        try:
            return s.t.write(encode(ftype, payload))
        except TransportClosed:
            return False

    def _send_host_hello(self, s: Session) -> None:
        self._write(s, T.HOST_HELLO, json.dumps({"proto": PROTO_VERSION}).encode())

    async def run(self) -> None:
        """Open the USB port whenever the board is present and run its session."""
        cfg = self._cfg
        backoff = cfg.reconnect_backoff_min_s
        while True:
            try:
                t = self._opener()
            except Exception as e:
                log.warning("open failed: %s", e)
                t = None
            if t is None:
                await asyncio.sleep(backoff)
                backoff = min(backoff * 2, cfg.reconnect_backoff_max_s)
                continue
            heard = await self._session("usb", t)
            backoff = cfg.reconnect_backoff_min_s if heard else cfg.reconnect_backoff_max_s
            if not heard:
                await asyncio.sleep(backoff)

    async def serve_wifi(self, t: Transport, label: str = "wifi") -> None:
        """Run an authenticated network session; it replaces an older network session but never USB."""
        await self._session("wifi", t, label)

    async def _activate(self, s: Session) -> bool:
        async with self._switch:
            old = self._active
            if s.kind == "wifi" and old is not None and old.kind == "usb":
                log.info("%s session refused: the board is on USB", s.label)
                return False
            if old is not None:
                log.info("%s session replaces the %s session", s.label, old.label)
                await old.t.close()
                await old.done.wait()
            self._active = s
            return True

    async def _session(self, kind: str, t: Transport, label: str | None = None) -> bool:
        """Run one session; True if the board sent HELLO on it."""
        s = Session(kind, t, label)
        try:
            if not await self._activate(s):
                return False
            await self._run_session(s)
        finally:
            await t.close()
            s.done.set()
        return s.got_hello

    async def _run_session(self, s: Session) -> None:
        cfg = self._cfg
        log.info("%s link open", s.label)
        self._h.on_connect()
        pinger: asyncio.Task | None = None
        now = time.monotonic()
        next_hello = now
        open_deadline = now + cfg.usb_open_timeout_s
        last_valid = now
        if s.kind == "wifi":
            self._send_host_hello(s)
            pinger = asyncio.create_task(self._ping_loop(s))
        try:
            while True:
                if s.kind == "usb" and not s.got_hello:
                    # opening the port resets the board, which discards what arrives before it boots
                    if now >= open_deadline:
                        log.warning("no HELLO within %.0f s of opening the port", cfg.usb_open_timeout_s)
                        return
                    if now >= next_hello:
                        self._send_host_hello(s)
                        next_hello = now + cfg.usb_hello_retry_s
                data = await s.t.read(cfg.read_poll_s)
                now = time.monotonic()
                frames = s.dec.feed(data, now)
                if frames:
                    last_valid = now
                for ftype, payload in frames:
                    if ftype == T.HELLO:
                        first = not s.got_hello
                        self._on_hello(s, payload)
                        if first:
                            pinger = asyncio.create_task(self._ping_loop(s)) if pinger is None else pinger
                    else:
                        self._dispatch(s, ftype, payload)
                if s.kind == "wifi" and now - last_valid > cfg.wifi_silence_timeout_s:
                    log.warning("no frame on %s for %.0f s; closing the session", s.label, cfg.wifi_silence_timeout_s)
                    return
        except TransportClosed:
            pass
        finally:
            if pinger is not None:
                pinger.cancel()
            log.info("%s link closed", s.label)
            if self._active is s:
                self._active = None
                self._h.on_disconnect()

    def _on_hello(self, s: Session, payload: bytes) -> None:
        info = parse_hello(payload)
        first = not s.got_hello
        s.got_hello = True
        if info.get("proto") != PROTO_VERSION:
            if not s.refused:
                log.error("board speaks protocol %r, the relay needs %d; ignoring it", info.get("proto"), PROTO_VERSION)
                s.refused = True
                s.ready = False
                self._write(s, T.SET_STATE, bytes([State.ERROR]))
            return
        if s.refused:
            return
        log.info("board HELLO on %s: %s", s.label, payload.decode(errors="replace"))
        if info.get("paired") is False and s.kind == "usb":
            log.info("board is not paired for Wi-Fi; run `claude-voice pair`")
        s.ready = True
        reason = str(info.get("reason", ""))
        self._h.on_hello(reason, info)
        self.send_state(self._h.current_state())
        if s.kind == "usb" and first and reason == "boot":
            # the board reset when the port opened, so it never saw the HOST_HELLO of this open
            self._send_host_hello(s)

    def _dispatch(self, s: Session, ftype: int, payload: bytes) -> None:
        if ftype == T.LOG:
            log.info("[device] %s", payload.decode(errors="replace").rstrip())
        elif ftype == T.PONG or not s.ready or self._active is not s:
            return
        else:
            self._h.on_frame(ftype, payload)

    async def _ping_loop(self, s: Session) -> None:
        while True:
            await asyncio.sleep(self._cfg.ping_interval_s)
            self._nonce = (self._nonce + 1) & 0xFFFFFFFF
            self._write(s, T.PING, struct.pack("<I", self._nonce))
