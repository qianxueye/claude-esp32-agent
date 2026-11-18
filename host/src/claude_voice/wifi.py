"""Wi-Fi link: TLS 1.2 PSK server for the board (docs/protocol.md, Wi-Fi)."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import socket
import ssl
import time
from collections.abc import Awaitable, Callable

from .config import Config
from .keychain import PskStore, psk_identity, valid_identity
from .link import Transport, TransportClosed

log = logging.getLogger("claude_voice.wifi")
CIPHER = "ECDHE-PSK-CHACHA20-POLY1305"
PSK_BYTES = 32


class WarnLimiter:
    """At most one warning per interval; the next one logged carries the count suppressed meanwhile."""

    def __init__(self, logger: logging.Logger, interval_s: float, clock: Callable[[], float] = time.monotonic) -> None:
        self._log = logger
        self._interval = interval_s
        self._clock = clock
        self._last: float | None = None
        self._suppressed = 0
        self._pending: tuple[str, tuple[object, ...]] = ("", ())

    def warn(self, msg: str, *args: object) -> None:
        now = self._clock()
        if self._last is not None and now - self._last < self._interval:
            self._suppressed += 1
            self._pending = (msg, args)
            return
        if self._suppressed:
            msg, args = msg + " (%d more suppressed)", (*args, self._suppressed)
        self._log.warning(msg, *args)
        self._last = now
        self._suppressed = 0

    def flush(self) -> None:
        """Log the latest suppressed warning and how many others were suppressed before it."""
        if not self._suppressed:
            return
        msg, args = self._pending
        if self._suppressed > 1:
            msg, args = msg + " (%d more suppressed)", (*args, self._suppressed - 1)
        self._log.warning(msg, *args)
        self._suppressed = 0


def server_context(keys: dict[str, bytes], warn: Callable[..., None] = log.warning) -> ssl.SSLContext:
    """TLS 1.2 only, one cipher suite, PSK from `keys`; any other identity fails the handshake."""
    if not ssl.HAS_PSK:
        raise RuntimeError("this Python's OpenSSL has no PSK support")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers(CIPHER)
    ctx.options |= ssl.OP_NO_TICKET

    def lookup(identity: str | None) -> bytes:
        key = keys.get(identity) if valid_identity(identity) else None
        if key is None:
            warn("Wi-Fi handshake with unknown PSK identity %r", (identity or "")[:40])
            return b""
        return key

    ctx.set_psk_server_callback(lookup)
    return ctx


def load_keys(cfg: Config, store: PskStore) -> dict[str, bytes]:
    """The paired board's identity and PSK, or nothing when it is not paired."""
    identity = psk_identity(cfg.device_serial) if cfg.device_serial else store.find_any()
    if not identity:
        return {}
    key = store.get(identity)
    if key is None or len(key) != PSK_BYTES:
        return {}
    return {identity: key}


class TcpTransport:
    """A TLS stream; a frame is dropped whole, before its first byte, while the send buffer is full."""

    def __init__(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, max_buffer: int, close_timeout: float = 2.0
    ) -> None:
        self._reader = reader
        self._close_timeout = close_timeout
        self._writer = writer
        self._max = max_buffer
        self._closed = False

    async def read(self, timeout: float) -> bytes:
        if self._closed:
            raise TransportClosed
        try:
            data = await asyncio.wait_for(self._reader.read(4096), timeout)
        except TimeoutError:
            return b""
        except (OSError, ssl.SSLError) as e:
            raise TransportClosed(type(e).__name__) from None
        if not data:
            raise TransportClosed
        return data

    def write(self, data: bytes) -> bool:
        if self._closed or self._writer.is_closing():
            raise TransportClosed
        if self._writer.transport.get_write_buffer_size() > self._max:
            return False
        self._writer.write(data)
        return True

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._writer.close()
        with contextlib.suppress(Exception):
            await asyncio.wait_for(self._writer.wait_closed(), self._close_timeout)


SessionRunner = Callable[[Transport], Awaitable[None]]
PeerKey = Callable[[asyncio.BaseTransport], str | None]


def peer_ip(transport: asyncio.BaseTransport) -> str | None:
    """The peer's IP address without the port, or None when the socket has no peer."""
    peer = transport.get_extra_info("peername")
    return str(peer[0]) if peer else None


class _Gate(asyncio.Protocol):
    """Refuses an accepted socket while a handshake cap is full, else hands it over unread for TLS."""

    def __init__(self, server: WifiServer) -> None:
        self._srv = server

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        key = self._srv._peer_key(transport)
        if key is None or not self._srv._admit(key):
            transport.abort()
            return
        # paused before the loop adds the reader, so no byte is read until TLS takes the socket
        transport.pause_reading()  # type: ignore[attr-defined]
        self._srv._spawn(transport, key)


class WifiServer:
    """Accepts TLS-PSK connections and hands each authenticated one to `run_session`."""

    def __init__(
        self, cfg: Config, keys: dict[str, bytes], run_session: SessionRunner, peer_key: PeerKey = peer_ip
    ) -> None:
        self._cfg = cfg
        self._warn_cap, self._warn_addr, self._warn_identity, self._warn_resumed = (
            WarnLimiter(log, cfg.wifi_warn_interval_s) for _ in range(4)
        )
        self._ctx = server_context(keys, self._warn_identity.warn)
        self._run_session = run_session
        self._server: asyncio.Server | None = None
        self._preauth = 0
        self._preauth_by: dict[str, int] = {}
        self._peer_key = peer_key
        self._handshakes: set[asyncio.Task] = set()

    @property
    def port(self) -> int:
        assert self._server is not None
        return self._server.sockets[0].getsockname()[1]

    async def start(self) -> None:
        cfg = self._cfg
        self._server = await asyncio.get_running_loop().create_server(
            lambda: _Gate(self), cfg.wifi_host, cfg.wifi_port, reuse_address=True,
        )

    def _admit(self, key: str) -> bool:
        if self._preauth >= self._cfg.wifi_preauth_max:
            self._warn_cap.warn("Wi-Fi connection refused: %d already in the TLS handshake", self._preauth)
            return False
        if self._preauth_by.get(key, 0) >= self._cfg.wifi_preauth_per_addr_max:
            self._warn_addr.warn("Wi-Fi connection from %s refused: its handshake limit is reached", key)
            return False
        self._preauth += 1
        self._preauth_by[key] = self._preauth_by.get(key, 0) + 1
        return True

    def _release(self, key: str) -> None:
        self._preauth -= 1
        left = self._preauth_by.pop(key) - 1
        if left:
            self._preauth_by[key] = left

    def _spawn(self, transport: asyncio.BaseTransport, key: str) -> None:
        task = asyncio.get_running_loop().create_task(self._handshake(transport, key))
        self._handshakes.add(task)
        task.add_done_callback(self._handshakes.discard)

    async def _handshake(self, raw: asyncio.BaseTransport, key: str) -> None:
        reader = asyncio.StreamReader()
        proto = asyncio.StreamReaderProtocol(reader, self._client)
        try:
            tls = await asyncio.get_running_loop().start_tls(
                raw, proto, self._ctx, server_side=True, ssl_handshake_timeout=self._cfg.wifi_handshake_timeout_s,
            )
        except Exception:
            tls = None
        finally:
            self._release(key)
        # start_tls returns None when the peer closes during the handshake
        if tls is None or not self._acceptable(tls.get_extra_info("ssl_object")):
            (tls or raw).abort()  # type: ignore[attr-defined]
            return
        proto.connection_made(tls)

    def _acceptable(self, tls: ssl.SSLObject | None) -> bool:
        if tls is None or tls.version() != "TLSv1.2" or tls.cipher()[0] != CIPHER:
            return False
        if tls.session_reused:
            self._warn_resumed.warn("Wi-Fi session resumption refused")
            return False
        return True

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        peer = writer.get_extra_info("peername")
        log.info("Wi-Fi session authenticated from %s", peer[0] if peer else "?")
        cfg = self._cfg
        await self._run_session(TcpTransport(reader, writer, cfg.wifi_write_buffer_max, cfg.wifi_close_timeout_s))

    async def close(self) -> None:
        if self._server is not None:
            self._server.close()
            self._server.close_clients()
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(self._server.wait_closed(), self._cfg.wifi_close_timeout_s)
            self._server = None
        for limiter in (self._warn_cap, self._warn_addr, self._warn_identity, self._warn_resumed):
            limiter.flush()
