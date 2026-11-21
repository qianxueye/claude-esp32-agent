"""Wi-Fi link: TLS 1.2 PSK server for the board (docs/protocol.md, Wi-Fi)."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import re
import socket
import ssl
import time
from collections.abc import Awaitable, Callable

from .config import Config
from .keychain import KeychainValueError, PskStore, earlier_account, key_id, psk_identity, wire_identity
from .link import Transport, TransportClosed
from .protocol import PSK_IDENTITY_PREFIX

log = logging.getLogger("claude_voice.wifi")
CIPHER = "ECDHE-PSK-CHACHA20-POLY1305"
PSK_BYTES = 32
_WIRE = re.compile(f"({re.escape(PSK_IDENTITY_PREFIX)}[0-9A-Fa-f]{{12}})-([0-9A-F]{{8}})")


def parse_wire_identity(identity: str | None) -> tuple[str, str] | None:
    """The board identity and key id of a handshake identity `clawd-<MAC>-<KID>`, or None."""
    m = _WIRE.fullmatch(identity) if identity else None
    return (m.group(1), m.group(2)) if m else None


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


class Keyring:
    """PSKs by wire identity, shared by the listeners; `removed` counts key removals (docs/protocol.md, Revocation)."""

    def __init__(self, keys: dict[str, bytes] | None = None) -> None:
        self.keys = {} if keys is None else keys
        self.removed = 0

    def __bool__(self) -> bool:
        return bool(self.keys)


def as_keyring(keys: Keyring | dict[str, bytes]) -> Keyring:
    """`keys` itself, or a keyring over the same dict."""
    return keys if isinstance(keys, Keyring) else Keyring(keys)


def server_context(
    keys: dict[str, bytes], warn: Callable[..., None] = log.warning, name: str = "Wi-Fi"
) -> ssl.SSLContext:
    """TLS 1.2 only, one cipher suite, PSK from `keys`; any other identity fails the handshake."""
    if not ssl.HAS_PSK:
        raise RuntimeError("this Python's OpenSSL has no PSK support")
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2
    ctx.set_ciphers(CIPHER)
    ctx.options |= ssl.OP_NO_TICKET

    def lookup(identity: str | None) -> bytes:
        key = keys.get(identity) if parse_wire_identity(identity) else None
        if key is None:
            warn("%s handshake with unknown PSK identity %r", name, (identity or "")[:40])
            return b""
        return key

    ctx.set_psk_server_callback(lookup)
    return ctx


def acceptable(tls: ssl.SSLObject | None, on_resumed: Callable[[], None]) -> bool:
    """True for a TLS 1.2 session with the one suite that is not a resumption."""
    if tls is None or tls.version() != "TLSv1.2" or tls.cipher()[0] != CIPHER:
        return False
    if tls.session_reused:
        on_resumed()
        return False
    return True


class Admission:
    """Counts connections still in the handshake, in total and per peer key, and refuses those over a cap."""

    def __init__(self, logger: logging.Logger, name: str, total_max: int, per_key_max: int, interval_s: float) -> None:
        self._name = name
        self._max = total_max
        self._per_key_max = per_key_max
        self._warn_cap, self._warn_key = (WarnLimiter(logger, interval_s) for _ in range(2))
        self.count = 0
        self._by: dict[str, int] = {}

    def admit(self, key: str) -> bool:
        if self.count >= self._max:
            self._warn_cap.warn("%s connection refused: %d already in the TLS handshake", self._name, self.count)
            return False
        if self._by.get(key, 0) >= self._per_key_max:
            self._warn_key.warn("%s connection from %s refused: its handshake limit is reached", self._name, key)
            return False
        self.count += 1
        self._by[key] = self._by.get(key, 0) + 1
        return True

    def release(self, key: str) -> None:
        self.count -= 1
        left = self._by.pop(key) - 1
        if left:
            self._by[key] = left

    def flush(self) -> None:
        self._warn_cap.flush()
        self._warn_key.flush()


def board_keys(store: PskStore, identity: str, bad: Callable[[str], None]) -> dict[str, bytes]:
    """Every PSK the store keeps for one board, by wire identity with the key id derived from the key.
    A value that is not a key goes to `bad` and is skipped; any other read error propagates."""
    reads: list[tuple[str, Callable[[], bytes | None]]] = [(identity, lambda: store.get(identity))]
    reads += [(earlier_account(identity, kid), lambda kid=kid: store.get_earlier(identity, kid)) for kid in store.earlier(identity)]
    found: dict[str, bytes] = {}
    for name, read in reads:
        try:
            key = read()
        except KeychainValueError:
            bad(name)
            continue
        if key is None:
            continue
        if len(key) != PSK_BYTES:
            bad(name)
            continue
        found[wire_identity(identity, key_id(key))] = key
    return found


def load_keys(cfg: Config, store: PskStore) -> dict[str, bytes]:
    """The paired board's PSKs by wire identity, or nothing when it is not paired."""
    identity = psk_identity(cfg.device_serial) if cfg.device_serial else store.find_any()
    if not identity:
        return {}
    return board_keys(store, identity, lambda name: log.warning("Keychain item %s is not a %d-byte hex key", name, PSK_BYTES))


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
        self, cfg: Config, keys: Keyring | dict[str, bytes], run_session: SessionRunner, peer_key: PeerKey = peer_ip
    ) -> None:
        self._cfg = cfg
        self._ring = as_keyring(keys)
        self._admission = Admission(
            log, "Wi-Fi", cfg.wifi_preauth_max, cfg.wifi_preauth_per_addr_max, cfg.wifi_warn_interval_s
        )
        self._warn_identity, self._warn_resumed = (WarnLimiter(log, cfg.wifi_warn_interval_s) for _ in range(2))
        self._ctx = server_context(self._ring.keys, self._warn_identity.warn)
        self._run_session = run_session
        self._server: asyncio.Server | None = None
        self._peer_key = peer_key
        self._handshakes: set[asyncio.Task] = set()
        self._live: set[TcpTransport] = set()

    @property
    def _preauth(self) -> int:
        return self._admission.count

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
        return self._admission.admit(key)

    def _spawn(self, transport: asyncio.BaseTransport, key: str) -> None:
        task = asyncio.get_running_loop().create_task(self._handshake(transport, key, self._ring.removed))
        self._handshakes.add(task)
        task.add_done_callback(self._handshakes.discard)

    async def _handshake(self, raw: asyncio.BaseTransport, key: str, admitted: int) -> None:
        reader = asyncio.StreamReader()
        proto = asyncio.StreamReaderProtocol(reader, lambda r, w: self._client(r, w, admitted))
        try:
            tls = await asyncio.get_running_loop().start_tls(
                raw, proto, self._ctx, server_side=True, ssl_handshake_timeout=self._cfg.wifi_handshake_timeout_s,
            )
        except Exception:
            tls = None
        finally:
            self._admission.release(key)
        # start_tls returns None when the peer closes during the handshake
        if tls is None or not self._acceptable(tls.get_extra_info("ssl_object")):
            (tls or raw).abort()  # type: ignore[attr-defined]
            return
        proto.connection_made(tls)

    def _acceptable(self, tls: ssl.SSLObject | None) -> bool:
        return acceptable(tls, lambda: self._warn_resumed.warn("Wi-Fi session resumption refused"))

    async def _client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter, admitted: int) -> None:
        if self._ring.removed != admitted:
            log.info("Wi-Fi session refused: a key was removed during its handshake")
            writer.transport.abort()
            return
        cfg = self._cfg
        transport = TcpTransport(reader, writer, cfg.wifi_write_buffer_max, cfg.wifi_close_timeout_s)
        self._live.add(transport)
        sock = writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        peer = writer.get_extra_info("peername")
        log.info("Wi-Fi session authenticated from %s", peer[0] if peer else "?")
        try:
            await self._run_session(transport)
        finally:
            self._live.discard(transport)

    async def close_sessions(self) -> None:
        """Close every authenticated session, including one still waiting to become the active link."""
        await asyncio.gather(*(t.close() for t in list(self._live)))

    async def close(self) -> None:
        if self._server is not None:
            self._server.close()
            self._server.close_clients()
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(self._server.wait_closed(), self._cfg.wifi_close_timeout_s)
            self._server = None
        self._admission.flush()
        for limiter in (self._warn_identity, self._warn_resumed):
            limiter.flush()
