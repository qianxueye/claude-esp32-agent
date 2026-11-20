"""Tunnel link: the board's TLS-PSK session inside WebSocket binary messages (docs/protocol.md, Tunnel)."""

from __future__ import annotations

import asyncio
import contextlib
import ipaddress
import logging
import ssl
from collections.abc import Awaitable, Callable

from websockets.datastructures import Headers
from websockets.frames import CloseCode, Opcode
from websockets.http11 import Request, Response
from websockets.protocol import OPEN
from websockets.server import ServerProtocol

from .config import Config
from .link import Transport, TransportClosed
from .wifi import Admission, WarnLimiter, acceptable, server_context

log = logging.getLogger("claude_voice.tunnel")
HOST = "127.0.0.1"
PATH = "/clawd/v1"
HEADER_CHARS = 64  # longest header value kept for logs
READ_CHUNK = 4096

SessionRunner = Callable[[Transport], Awaitable[None]]


class _QuietLog(logging.LoggerAdapter):
    """Passes only warnings and errors: the library's debug and info lines can carry request headers."""

    def isEnabledFor(self, level: int) -> bool:
        return level >= logging.WARNING and self.logger.isEnabledFor(level)


class _WsProtocol(ServerProtocol):
    """Sends every HTTP answer other than the 101 upgrade as a 404 without a body."""

    def send_response(self, response: Response) -> None:
        if response.status_code != 101:
            response = Response(404, "Not Found", Headers([("Content-Length", "0"), ("Connection", "close")]))
        super().send_response(response)


def _header(headers: Headers, name: str) -> str | None:
    values = headers.get_all(name)
    return values[0].strip() if len(values) == 1 else None


def peer_key(headers: Headers) -> str:
    """Admission key from CF-Connecting-IP; `tunnel:?` when it is missing or not an IP address."""
    value = _header(headers, "CF-Connecting-IP")
    try:
        return "tunnel:" + str(ipaddress.ip_address(value or ""))
    except ValueError:
        return "tunnel:?"


class TunnelTransport:
    """The inner TLS stream; a frame is dropped whole, before encryption, while the socket backlog is full."""

    def __init__(self, conn: _Conn, max_buffer: int, close_timeout: float) -> None:
        self._conn = conn
        self._max = max_buffer
        self._close_timeout = close_timeout
        self._closed = False

    async def read(self, timeout: float) -> bytes:
        if self._closed:
            raise TransportClosed
        try:
            data = await asyncio.wait_for(self._conn.reader.read(READ_CHUNK), timeout)
        except TimeoutError:
            return b""
        if not data:
            raise TransportClosed
        return data

    def write(self, data: bytes) -> bool:
        conn = self._conn
        if self._closed or not conn.open:
            raise TransportClosed
        if conn.backlog() > self._max:
            return False
        conn.send(data)
        return True

    async def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        await self._conn.shutdown(self._close_timeout)


class _Conn(asyncio.Protocol):
    """One tunnel connection: HTTP upgrade, then the inner TLS handshake, then the session."""

    def __init__(self, srv: TunnelServer) -> None:
        self._srv = srv
        cfg = srv.cfg
        self._ws = _WsProtocol(
            max_size=cfg.tunnel_ws_max_message, logger=_QuietLog(logging.getLogger("websockets.server"), {})
        )
        self._tcp: asyncio.Transport | None = None
        self._received = 0
        self._key: str | None = None
        self._incoming = ssl.MemoryBIO()
        self._outgoing = ssl.MemoryBIO()
        self._tls: ssl.SSLObject | None = None
        self._timer: asyncio.TimerHandle | None = None
        self.reader = asyncio.StreamReader()
        self.authenticated = False
        self.lost = asyncio.get_running_loop().create_future()
        self.peer = "?"
        self.ray = "?"

    @property
    def open(self) -> bool:
        return self._tcp is not None and not self._tcp.is_closing() and self._ws.state is OPEN

    def connection_made(self, transport: asyncio.BaseTransport) -> None:
        assert isinstance(transport, asyncio.Transport)
        self._tcp = transport
        self.reader.set_transport(transport)
        self._arm(self._srv.cfg.tunnel_upgrade_timeout_s)

    def data_received(self, data: bytes) -> None:
        try:
            self._receive(data)
        except Exception as e:
            self._srv.warn_error.warn("tunnel connection closed after %s", type(e).__name__)
            self._abort()

    def eof_received(self) -> bool | None:
        with contextlib.suppress(Exception):
            self._ws.receive_eof()
            self._flush_ws()
        return None

    def connection_lost(self, exc: Exception | None) -> None:
        self._disarm()
        self._release()
        self.reader.feed_eof()
        if not self.lost.done():
            self.lost.set_result(None)

    def _receive(self, data: bytes) -> None:
        if not self.authenticated:
            self._received += len(data)
            if self._received > self._srv.cfg.tunnel_preauth_bytes_max:
                self._abort()
                return
        self._ws.receive_data(data)
        for event in self._ws.events_received():
            if self._tcp is None or self._tcp.is_closing():
                return
            if isinstance(event, Request):
                self._on_request(event)
            elif event.opcode in (Opcode.BINARY, Opcode.CONT):
                self._on_bytes(event.data)
            elif event.opcode is Opcode.TEXT:
                self._ws.fail(CloseCode.UNSUPPORTED_DATA)
        self._flush_ws()

    def _on_request(self, request: Request) -> None:
        if request.method != "GET" or request.path != PATH:
            self._ws.send_response(self._ws.reject(404, ""))
            return
        response = self._ws.accept(request)
        if response.status_code != 101:
            self._ws.send_response(response)
            return
        key = peer_key(request.headers)
        if not self._srv.admission.admit(key):
            self._abort()
            return
        self._key = key
        self.peer = key.removeprefix("tunnel:")
        self.ray = (_header(request.headers, "CF-Ray") or "?")[:HEADER_CHARS]
        self._ws.send_response(response)
        self._tls = self._srv.ctx.wrap_bio(self._incoming, self._outgoing, server_side=True)
        self._arm(self._srv.cfg.wifi_handshake_timeout_s)

    def _on_bytes(self, data: bytes) -> None:
        if self._tls is None:
            return
        self._incoming.write(data)
        if not self.authenticated:
            try:
                self._tls.do_handshake()
            except ssl.SSLWantReadError:
                self._flush_tls()
                return
            except (ssl.SSLError, OSError):
                self._flush_tls()
                self._abort()
                return
            self._flush_tls()
            self._release()
            self._disarm()
            if not acceptable(self._tls, self._srv.warn_resumed_refused):
                self._abort()
                return
            self.authenticated = True
            self._srv.spawn(self)
        self._drain_tls()

    def _drain_tls(self) -> None:
        assert self._tls is not None
        while True:
            try:
                data = self._tls.read(READ_CHUNK)
            except ssl.SSLWantReadError:
                break
            except (ssl.SSLError, OSError):
                self._abort()
                return
            if not data:
                self.reader.feed_eof()
                break
            self.reader.feed_data(data)
        self._flush_tls()

    def _flush_tls(self) -> None:
        records = self._outgoing.read()
        if records and self._ws.state is OPEN:
            self._ws.send_binary(records)
            self._flush_ws()

    def _flush_ws(self) -> None:
        for chunk in self._ws.data_to_send():
            if self._tcp is None or self._tcp.is_closing():
                return
            if chunk:
                self._tcp.write(chunk)
            else:
                self._tcp.close()

    def backlog(self) -> int:
        assert self._tcp is not None
        return self._tcp.get_write_buffer_size()

    def send(self, frame: bytes) -> None:
        """Encrypt one frame and send its records as one WebSocket message."""
        assert self._tls is not None
        try:
            self._tls.write(frame)
        except (ssl.SSLError, OSError):
            self._abort()
            raise TransportClosed from None
        self._flush_tls()

    async def shutdown(self, timeout: float) -> None:
        if self._tcp is None:
            return
        if self.open:
            with contextlib.suppress(Exception):
                self._ws.send_close(CloseCode.NORMAL_CLOSURE)
                self._flush_ws()
        self._tcp.close()
        try:
            await asyncio.wait_for(asyncio.shield(self.lost), timeout)
        except TimeoutError:
            self._abort()

    def _arm(self, seconds: float) -> None:
        self._disarm()
        self._timer = asyncio.get_running_loop().call_later(seconds, self._abort)

    def _disarm(self) -> None:
        if self._timer is not None:
            self._timer.cancel()
            self._timer = None

    def _release(self) -> None:
        if self._key is not None:
            self._srv.admission.release(self._key)
            self._key = None

    def _abort(self) -> None:
        self._disarm()
        self._release()
        if self._tcp is not None:
            self._tcp.abort()


class TunnelServer:
    """Accepts WebSocket upgrades on 127.0.0.1 and hands each authenticated inner session to `run_session`."""

    def __init__(self, cfg: Config, keys: dict[str, bytes], run_session: SessionRunner) -> None:
        self.cfg = cfg
        interval = cfg.wifi_warn_interval_s
        self.admission = Admission(log, "Tunnel", cfg.tunnel_preauth_max, cfg.tunnel_preauth_per_addr_max, interval)
        self._warn_identity, self._warn_resumed, self.warn_error = (WarnLimiter(log, interval) for _ in range(3))
        self.ctx = server_context(keys, self._warn_identity.warn, "Tunnel")
        self._run_session = run_session
        self._server: asyncio.Server | None = None
        self._sessions: set[asyncio.Task] = set()

    @property
    def port(self) -> int:
        assert self._server is not None
        return self._server.sockets[0].getsockname()[1]

    @property
    def address(self) -> str:
        assert self._server is not None
        return self._server.sockets[0].getsockname()[0]

    async def start(self) -> None:
        logging.getLogger("websockets").setLevel(logging.WARNING)
        self._server = await asyncio.get_running_loop().create_server(
            lambda: _Conn(self), HOST, self.cfg.tunnel_port, reuse_address=True,
        )

    def warn_resumed_refused(self) -> None:
        self._warn_resumed.warn("Tunnel session resumption refused")

    def spawn(self, conn: _Conn) -> None:
        task = asyncio.get_running_loop().create_task(self._session(conn))
        self._sessions.add(task)
        task.add_done_callback(self._sessions.discard)

    async def _session(self, conn: _Conn) -> None:
        cfg = self.cfg
        log.info("tunnel session open from %r, CF-Ray %r", conn.peer, conn.ray)
        try:
            await self._run_session(TunnelTransport(conn, cfg.tunnel_write_buffer_max, cfg.wifi_close_timeout_s))
        finally:
            log.info("tunnel session closed from %r, CF-Ray %r", conn.peer, conn.ray)

    async def close(self) -> None:
        if self._server is not None:
            self._server.close()
            self._server.close_clients()
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(self._server.wait_closed(), self.cfg.wifi_close_timeout_s)
            self._server = None
        self.admission.flush()
        for limiter in (self._warn_identity, self._warn_resumed, self.warn_error):
            limiter.flush()
