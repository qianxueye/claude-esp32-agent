import asyncio
import contextlib
import dataclasses
import logging
import socket
import ssl
import struct

import pytest
from fake_device import FAKE_SERIAL, FakeDevice, client_context, tls_connect
from test_relay import make, settle, until
from websockets.client import ClientProtocol
from websockets.datastructures import Headers
from websockets.frames import Opcode
from websockets.http11 import Response
from websockets.protocol import OPEN
from websockets.uri import parse_uri

from claude_voice.config import Config, load
from claude_voice.keychain import psk_identity
from claude_voice.link import TransportClosed
from claude_voice.protocol import Decoder, Input, State, T, encode
from claude_voice.tunnel import PATH, TunnelServer, peer_key
from claude_voice.wifi import CIPHER, WifiServer

IDENTITY = psk_identity(FAKE_SERIAL)
KEY = bytes(range(32))
TURN_INPUT = Input.TAP  # the input that starts a turn
_client_log = logging.getLogger("tunnel-test-client")
_client_log.disabled = True


def tunnel_cfg(**over):
    return dataclasses.replace(Config(), **{"tunnel_port": 0, "wifi_host": "127.0.0.1", "wifi_port": 0, **over})


class WsBoard:
    """Board end of the tunnel: a WebSocket client carrying the inner TLS-PSK session, usable as a link Transport."""

    def __init__(self, reader, writer, key=KEY, *, path=PATH, split=False, ctx=None, session=None):
        self._r, self._w = reader, writer
        self.ws = ClientProtocol(parse_uri(f"ws://relay{path}"), max_size=None, logger=_client_log)
        self._in, self._out = ssl.MemoryBIO(), ssl.MemoryBIO()
        ctx = ctx or client_context(IDENTITY, key)
        self.tls = ctx.wrap_bio(self._in, self._out, server_side=False, session=session)
        self.split = split
        self.response: Response | None = None
        self.upgraded = asyncio.Event()
        self.handshaken = asyncio.Event()
        self.rx: asyncio.Queue[bytes | None] = asyncio.Queue()
        self.error: Exception | None = None
        self.eof = asyncio.Event()
        self._closed = False
        self._pump = asyncio.create_task(self._run())

    @classmethod
    async def open(cls, port, *, ip="203.0.113.7", headers=(), **kw):
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        board = cls(reader, writer, **kw)
        req = board.ws.connect()
        if ip is not None:
            req.headers["CF-Connecting-IP"] = ip
        for k, v in headers:
            req.headers[k] = v
        board.ws.send_request(req)
        board._flush_ws()
        return board

    @classmethod
    async def connect(cls, port, **kw):
        board = await cls.open(port, **kw)
        await asyncio.wait_for(board.upgraded.wait(), 3)
        assert board.response is not None and board.response.status_code == 101
        board._handshake()
        try:
            async with asyncio.timeout(3):
                while not board.handshaken.is_set():
                    if board.eof.is_set():
                        raise ConnectionError("closed during the inner handshake")
                    await asyncio.sleep(0.005)
        except BaseException:
            await board.close()
            raise
        return board

    def _flush_ws(self):
        for chunk in self.ws.data_to_send():
            if chunk and not self._w.is_closing():
                self._w.write(chunk)

    def _send_records(self):
        records = self._out.read()
        if not records or self.ws.state is not OPEN:
            return
        parts = [records[: len(records) // 2], records[len(records) // 2 :]] if self.split else [records]
        for part in parts:
            self.ws.send_binary(part)
        self._flush_ws()

    def _handshake(self):
        if self.handshaken.is_set():
            return
        try:
            self.tls.do_handshake()
        except ssl.SSLWantReadError:
            pass
        except ssl.SSLError as e:
            self.error = e
        else:
            self.handshaken.set()
        self._send_records()

    async def _run(self):
        try:
            while True:
                data = await self._r.read(65536)
                if not data:
                    break
                self.ws.receive_data(data)
                for event in self.ws.events_received():
                    if isinstance(event, Response):
                        self.response = event
                        self.upgraded.set()
                    elif event.opcode in (Opcode.BINARY, Opcode.CONT):
                        self._in.write(event.data)
                        self._handshake()
                        self._drain()
                self._flush_ws()
        except (ConnectionError, OSError):
            pass
        finally:
            self.upgraded.set()
            self.eof.set()
            self.rx.put_nowait(None)

    def _drain(self):
        if not self.handshaken.is_set():
            return
        while True:
            try:
                data = self.tls.read(65536)
            except ssl.SSLWantReadError:
                return
            except ssl.SSLError as e:
                self.error = e
                return
            if not data:
                return
            self.rx.put_nowait(data)

    def send_raw(self, data: bytes):
        """One binary message with bytes that bypass the inner TLS session."""
        self.ws.send_binary(data)
        self._flush_ws()

    async def read(self, timeout):
        if self._closed:
            raise TransportClosed
        try:
            item = await asyncio.wait_for(self.rx.get(), timeout)
        except TimeoutError:
            return b""
        if item is None:
            self.rx.put_nowait(None)
            raise TransportClosed
        return item

    def write(self, data):
        if self._closed or self.eof.is_set() or self.ws.state is not OPEN:
            raise TransportClosed
        self.tls.write(data)
        self._send_records()
        return True

    def write_coalesced(self, frames):
        for f in frames:
            self.tls.write(f)
        self._send_records()

    @property
    def closed(self):
        return self._closed or self.eof.is_set()

    async def close(self):
        if self._closed:
            return
        self._closed = True
        self._w.close()
        self._pump.cancel()
        with contextlib.suppress(BaseException):
            await self._pump


class Recorder:
    def __init__(self):
        self.sessions = []

    async def __call__(self, t):
        self.sessions.append(t)
        with contextlib.suppress(TransportClosed):
            while True:
                await t.read(0.05)


@contextlib.asynccontextmanager
async def tunnel(run_session, cfg=None):
    srv = TunnelServer(cfg or tunnel_cfg(), {IDENTITY: KEY}, run_session)
    await srv.start()
    try:
        yield srv
    finally:
        await srv.close()


async def _closed_within(board: WsBoard, seconds):
    try:
        await asyncio.wait_for(board.eof.wait(), seconds)
        return True
    except TimeoutError:
        return False


class Harness:
    """Relay with fake STT, agent and TTS behind a LAN server and a tunnel server."""

    def __init__(self, **over):
        self.h = make(**{"wifi_host": "127.0.0.1", "wifi_port": 0, "tunnel_port": 0, "wifi_silence_timeout_s": 3.0, **over})
        self.h.allow_open = False
        self.lan = WifiServer(self.h.cfg, {IDENTITY: KEY}, self.h.link.serve_wifi)
        self.srv = TunnelServer(self.h.cfg, {IDENTITY: KEY}, lambda t: self.h.link.serve_wifi(t, "tunnel"))
        self.boards = []

    async def __aenter__(self):
        await self.lan.start()
        await self.srv.start()
        self.h.task = asyncio.create_task(self.h.link.run())
        return self

    async def board(self, **kw):
        t = await WsBoard.connect(self.srv.port, **kw)
        d = FakeDevice(t, speed=self.h.speed, mic=self.h.mic, link="tunnel")
        d.start()
        self.boards.append(d)
        await until(lambda: T.SET_STATE in d.received)
        return d

    async def lan_board(self):
        t = await tls_connect(self.lan.port, IDENTITY, KEY)
        d = FakeDevice(t, speed=self.h.speed, mic=self.h.mic, link="wifi")
        d.start()
        self.boards.append(d)
        await until(lambda: T.SET_STATE in d.received)
        return d

    async def __aexit__(self, *exc):
        self.h.task.cancel()
        for d in self.boards + self.h.devices:
            await d.stop()
            await d.t.close()
        await self.srv.close()
        await self.lan.close()


def _session_logs(caplog):
    return [r.getMessage() for r in caplog.records if "link open" in r.getMessage() or "session open" in r.getMessage()]


def _lan_closed(d):
    return d.t._closed or d.t._writer.is_closing()


async def test_listens_on_loopback_only_with_the_exact_inner_handshake():
    rec = Recorder()
    async with tunnel(rec, tunnel_cfg(wifi_host="0.0.0.0")) as srv:
        assert srv.address == "127.0.0.1"
        b = await WsBoard.connect(srv.port, headers=[("Sec-WebSocket-Extensions", "permessage-deflate")])
        assert "Sec-WebSocket-Extensions" not in b.response.headers
        assert b.tls.version() == "TLSv1.2" and b.tls.cipher()[0] == CIPHER and not b.tls.session_reused
        await until(lambda: rec.sessions)
        await b.close()


async def test_full_turn_over_tunnel():
    async with Harness() as w:
        d = await w.board()
        assert d.host_hellos == 1 and d.hellos == ["reply"] and w.h.link.active_kind == "wifi"
        d.inject(TURN_INPUT)
        await until(lambda: d.states[-1:] == [State.IDLE] and State.SPEAKING in d.states)
        assert w.h.stt.calls and w.h.agent.asked == ["你好"]
        assert d.max_queued <= 64000 and d.flushes == 1


@pytest.mark.parametrize("split", [False, True], ids=["one-message-per-record", "records-split-across-messages"])
async def test_message_boundaries_do_not_matter(split):
    async with Harness() as w:
        t = await WsBoard.connect(w.srv.port, split=split)
        w.boards.append(FakeDevice(t, link="tunnel"))
        hello = encode(T.HELLO, b'{"proto":3,"reason":"reply","link":"tunnel"}')
        status = encode(T.SPK_STATUS, struct.pack("<IBI", 0, 0, 1234))
        t.write_coalesced([hello, status[:4]])
        t.write(status[4:])
        await until(lambda: w.h.link.connected and w.h.relay.speaker.synced)


@pytest.mark.parametrize("key", [bytes(32), KEY[:-1] + b"\x00"], ids=["zero-key", "one-byte-off"])
async def test_wrong_psk_fails_and_nothing_reaches_link(key, caplog):
    caplog.set_level(logging.INFO, logger="claude_voice")
    async with Harness() as w:
        with pytest.raises(ConnectionError):
            await WsBoard.connect(w.srv.port, key=key)
        await asyncio.sleep(0.1)
        assert w.h.link.active_kind is None and w.h.relay.state == State.IDLE and w.srv.admission.count == 0
    assert not _session_logs(caplog)


async def test_plain_frames_inside_messages_are_never_processed(caplog):
    caplog.set_level(logging.INFO, logger="claude_voice")
    async with Harness() as w:
        b = await WsBoard.open(w.srv.port)
        await asyncio.wait_for(b.upgraded.wait(), 3)
        assert b.response.status_code == 101
        b.send_raw(encode(T.HELLO, b'{"proto":3,"reason":"boot","link":"tunnel"}'))
        b.send_raw(encode(T.INPUT, bytes([TURN_INPUT])))
        assert await _closed_within(b, 1.0)
        assert w.h.link.active_kind is None and w.h.relay.state == State.IDLE and not w.h.stt.calls
        assert b.rx.qsize() == 1 and b.rx.get_nowait() is None
        await b.close()
    assert not _session_logs(caplog)


async def test_resumed_inner_session_is_refused():
    rec = Recorder()
    async with tunnel(rec) as srv:
        ctx = client_context(IDENTITY, KEY)
        first = await WsBoard.connect(srv.port, ctx=ctx)
        await until(lambda: rec.sessions)
        session = first.tls.session
        await first.close()
        second = await WsBoard.open(srv.port, ctx=ctx, session=session)
        await asyncio.wait_for(second.upgraded.wait(), 3)
        second._handshake()
        assert await _closed_within(second, 2.0)
        assert second.tls.session_reused, "the client must resume the first session"
        assert len(rec.sessions) == 1
        await second.close()


async def _upgrade(port, ip):
    b = await WsBoard.open(port, ip=ip)
    await asyncio.wait_for(b.upgraded.wait(), 3)
    return b


async def test_tunnel_caps_per_address_and_in_total_spare_lan_handshakes(caplog):
    caplog.set_level(logging.WARNING, logger="claude_voice")
    c = tunnel_cfg(tunnel_preauth_max=4, tunnel_preauth_per_addr_max=2, wifi_preauth_max=4, wifi_handshake_timeout_s=5.0)
    rec = Recorder()
    lan = WifiServer(c, {IDENTITY: KEY}, rec)
    await lan.start()
    try:
        async with tunnel(Recorder(), c) as srv:
            same = [await _upgrade(srv.port, "198.51.100.1") for _ in range(3)]
            assert [b.response is not None for b in same] == [True, True, False]
            others = [await _upgrade(srv.port, ip) for ip in ("198.51.100.2", "198.51.100.3", "198.51.100.4")]
            assert [b.response is not None for b in others] == [True, True, False]
            assert srv.admission.count == 4
            t = await tls_connect(lan.port, IDENTITY, KEY)
            await until(lambda: rec.sessions)
            await t.close()
            for b in same + others:
                await b.close()
            await until(lambda: srv.admission.count == 0)
            ok = await WsBoard.connect(srv.port, ip="198.51.100.1")
            await ok.close()
    finally:
        await lan.close()
    messages = [r.getMessage() for r in caplog.records if r.name == "claude_voice.tunnel"]
    assert "Tunnel connection from tunnel:198.51.100.1 refused: its handshake limit is reached" in messages
    assert "Tunnel connection refused: 4 already in the TLS handshake" in messages


async def test_missing_or_invalid_connecting_ip_shares_one_key(caplog):
    caplog.set_level(logging.WARNING, logger="claude_voice")
    assert peer_key(Headers()) == "tunnel:?"
    assert peer_key(Headers([("CF-Connecting-IP", "not an address")])) == "tunnel:?"
    assert peer_key(Headers([("CF-Connecting-IP", "a"), ("CF-Connecting-IP", "b")])) == "tunnel:?"
    assert peer_key(Headers([("cf-connecting-ip", " 2001:DB8::1 ")])) == "tunnel:2001:db8::/64"
    c = tunnel_cfg(tunnel_preauth_max=4, tunnel_preauth_per_addr_max=2, wifi_handshake_timeout_s=5.0)
    async with tunnel(Recorder(), c) as srv:
        held = [await _upgrade(srv.port, ip) for ip in (None, "garbage", None)]
        assert [b.response is not None for b in held] == [True, True, False]
        other = await _upgrade(srv.port, "198.51.100.9")
        assert other.response is not None
        for b in held + [other]:
            await b.close()
    assert "Tunnel connection from tunnel:? refused: its handshake limit is reached" in [
        r.getMessage() for r in caplog.records
    ]


def _key(ip):
    return peer_key(Headers([("CF-Connecting-IP", ip)]))


def test_ipv6_keys_by_its_64_network_and_mapped_ipv4_by_the_ipv4_address():
    assert _key("2001:db8:1:2::1") == _key("2001:db8:1:2:ffff:ffff:ffff:ffff") == "tunnel:2001:db8:1:2::/64"
    assert _key("2001:db8:1:2::1") != _key("2001:db8:1:3::1")
    assert _key("::ffff:198.51.100.7") == _key("198.51.100.7") == "tunnel:198.51.100.7"
    assert _key("198.51.100.7") != _key("198.51.100.8")


async def test_one_ipv6_64_shares_the_per_address_cap():
    c = tunnel_cfg(tunnel_preauth_max=8, tunnel_preauth_per_addr_max=2, wifi_handshake_timeout_s=5.0)
    async with tunnel(Recorder(), c) as srv:
        same = [await _upgrade(srv.port, ip) for ip in ("2001:db8:0:1::1", "2001:db8:0:1::2", "2001:db8:0:1:a::3")]
        assert [b.response is not None for b in same] == [True, True, False]
        other = [await _upgrade(srv.port, ip) for ip in ("2001:db8:0:2::1", "2001:db8:0:3::1")]
        assert [b.response is not None for b in other] == [True, True]
        for b in same + other:
            await b.close()


async def test_upgrade_deadline_closes_a_silent_connection():
    async with tunnel(Recorder(), tunnel_cfg(tunnel_upgrade_timeout_s=0.3)) as srv:
        reader, writer = await asyncio.open_connection("127.0.0.1", srv.port)
        writer.write(b"GET /clawd/v1 HTTP/1.1\r\nHost: relay\r\n")
        assert await asyncio.wait_for(reader.read(), 2) == b""
        writer.close()


async def test_inner_handshake_deadline_closes_and_releases_the_slot():
    c = tunnel_cfg(tunnel_upgrade_timeout_s=5.0, wifi_handshake_timeout_s=0.3)
    async with tunnel(Recorder(), c) as srv:
        b = await _upgrade(srv.port, "198.51.100.1")
        assert b.response.status_code == 101 and srv.admission.count == 1
        assert not await _closed_within(b, 0.15)
        assert await _closed_within(b, 1.5)
        assert srv.admission.count == 0
        await b.close()


async def test_message_over_the_size_limit_closes_the_connection():
    rec = Recorder()
    async with tunnel(rec, tunnel_cfg(tunnel_ws_max_message=1024)) as srv:
        b = await WsBoard.connect(srv.port)
        await until(lambda: rec.sessions)
        b.send_raw(bytes(1025))
        assert await _closed_within(b, 1.0)
        assert b.ws.close_rcvd is not None and b.ws.close_rcvd.code == 1009
        await b.close()


async def test_text_message_closes_the_connection():
    rec = Recorder()
    async with tunnel(rec) as srv:
        b = await WsBoard.connect(srv.port)
        await until(lambda: rec.sessions)
        b.ws.send_text(b"hello")
        b._flush_ws()
        assert await _closed_within(b, 1.0)
        assert b.ws.close_rcvd is not None and b.ws.close_rcvd.code == 1003
        await b.close()


async def test_binary_frame_after_a_text_frame_in_the_same_chunk_is_never_processed(caplog):
    caplog.set_level(logging.INFO, logger="claude_voice")
    async with Harness() as w:
        conns = []
        run = w.srv._run_session
        w.srv._run_session = lambda t: (conns.append(t._conn), run(t))[1]
        d = await w.board()
        b = d.t
        b.ws.send_text(b"hello")
        b.tls.write(encode(T.INPUT, bytes([TURN_INPUT])))
        b.ws.send_binary(b._out.read())
        conns[0].data_received(b"".join(b.ws.data_to_send()))  # both frames in one call, not via the socket
        assert await _closed_within(b, 1.0)
        await until(lambda: any("tunnel session closed" in r.getMessage() for r in caplog.records))
        assert b.ws.close_rcvd is not None and b.ws.close_rcvd.code == 1003
        assert w.h.relay.state == State.IDLE and not w.h.stt.calls
    assert not [r for r in caplog.records if r.getMessage().startswith("input ")]


def _padded_request(n, size):
    pad = "".join(f"X-Pad-{i}: {'a' * size}\r\n" for i in range(n))
    return (
        "GET /clawd/v1 HTTP/1.1\r\nHost: relay\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" + pad + "\r\n"
    ).encode()


async def test_preauth_byte_cap_counts_request_headers():
    async with tunnel(Recorder(), tunnel_cfg(tunnel_preauth_bytes_max=4096)) as srv:
        for n, ok in ((4, True), (12, False)):
            reader, writer = await asyncio.open_connection("127.0.0.1", srv.port)
            writer.write(_padded_request(n, 400))
            head = await asyncio.wait_for(reader.read(12), 2)
            assert head.startswith(b"HTTP/1.1 101") is ok, head
            writer.close()


async def test_preauth_byte_cap_counts_messages_before_authentication():
    async with tunnel(Recorder(), tunnel_cfg(tunnel_preauth_bytes_max=4096, wifi_handshake_timeout_s=5.0)) as srv:
        b = await _upgrade(srv.port, "198.51.100.1")
        assert b.response.status_code == 101
        for _ in range(60):
            if b.eof.is_set():
                break
            b.ws.send_ping(bytes(100))
            b._flush_ws()
            await asyncio.sleep(0.005)
        assert await _closed_within(b, 1.0)
        assert srv.admission.count == 0
        await b.close()


async def _http(port, request: bytes) -> bytes:
    reader, writer = await asyncio.open_connection("127.0.0.1", port)
    writer.write(request)
    try:
        return await asyncio.wait_for(reader.read(), 2)
    finally:
        writer.close()


UPGRADE = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"


@pytest.mark.parametrize(
    "request_line,headers",
    [
        ("GET / HTTP/1.1", UPGRADE),
        ("GET /clawd/v2 HTTP/1.1", UPGRADE),
        ("GET /clawd/v1?x=1 HTTP/1.1", UPGRADE),
        ("GET /clawd/v1/ HTTP/1.1", UPGRADE),
        ("POST /clawd/v1 HTTP/1.1", UPGRADE),
        ("GET /clawd/v1 HTTP/1.1", ""),
        ("GET /clawd/v1 HTTP/1.1", "Upgrade: websocket\r\nConnection: Upgrade\r\n"),
        ("GET /clawd/v1 HTTP/1.1", UPGRADE + "X-Long: " + "a" * 9000 + "\r\n"),
        ("GET /clawd/v1 HTTP/1.1", UPGRADE + "Content-Length: 5\r\n"),
        ("GET /clawd/v1 HTTP/9.9", UPGRADE),
    ],
    ids=["root", "other-version", "query", "trailing-slash", "post", "plain-get", "no-key", "header-too-long",
         "with-body", "bad-http-version"],
)
async def test_every_other_request_gets_an_empty_404(request_line, headers):
    async with tunnel(Recorder(), tunnel_cfg(tunnel_preauth_bytes_max=65536)) as srv:
        answer = await _http(srv.port, f"{request_line}\r\nHost: relay\r\n{headers}\r\n".encode())
    head, _, body = answer.partition(b"\r\n\r\n")
    assert head.startswith(b"HTTP/1.1 404 "), answer[:80]
    assert b"\r\ncontent-length: 0" in head.lower() and body == b""


async def test_backlog_drops_whole_frames_before_encryption_and_session_survives():
    rec = Recorder()
    async with tunnel(rec, tunnel_cfg(tunnel_write_buffer_max=1)) as srv:
        b = await WsBoard.connect(srv.port)
        await until(lambda: rec.sessions)
        t = rec.sessions[0]
        b._w.transport.pause_reading()
        sent, dropped = [], 0
        for i in range(4000):
            frame = encode(T.SPK_AUDIO, struct.pack("<I", i) + bytes(4092))
            if t.write(frame):
                sent.append(i)
            else:
                dropped += 1
                if dropped >= 3:
                    break
        assert dropped >= 3, "the socket backlog never filled"
        b._w.transport.resume_reading()
        dec, got = Decoder(None), []
        async with asyncio.timeout(10):
            while len(got) < len(sent):
                data = await b.read(0.5)
                got += [struct.unpack("<I", p[:4])[0] for _, p in dec.feed(data, 0.0)]
        assert got == sent and b.error is None
        await until(lambda: t._conn.backlog() <= 1)
        assert t.write(encode(T.PING, struct.pack("<I", 99)))
        async with asyncio.timeout(3):
            frames = []
            while not frames:
                frames = dec.feed(await b.read(0.5), 0.0)
        assert frames == [(T.PING, struct.pack("<I", 99))] and b.error is None
        await b.close()


async def test_tunnel_session_replaces_lan_session():
    async with Harness() as w:
        lan = await w.lan_board()
        remote = await w.board()
        await until(lambda: _lan_closed(lan))
        remote.inject(TURN_INPUT)
        await settle(remote, State.LISTENING)


async def test_lan_session_replaces_tunnel_session():
    async with Harness() as w:
        remote = await w.board()
        lan = await w.lan_board()
        await until(lambda: remote.t.closed)
        lan.inject(TURN_INPUT)
        await settle(lan, State.LISTENING)


async def test_usb_takes_over_from_tunnel_and_refuses_a_new_one():
    async with Harness() as w:
        d = await w.board()
        w.h.allow_open = True
        await until(lambda: w.h.devices and T.SET_STATE in w.h.dev.received)
        assert w.h.link.active_kind == "usb"
        await until(lambda: d.t.closed)
        t = await WsBoard.connect(w.srv.port)
        late = FakeDevice(t, link="tunnel")
        late.start()
        w.boards.append(late)
        with pytest.raises(TransportClosed):
            for _ in range(40):
                await t.read(0.05)
        assert late.host_hellos == 0 and w.h.link.active_kind == "usb"
        w.h.dev.inject(TURN_INPUT)
        await settle(w.h.dev, State.LISTENING)


async def test_access_secret_header_never_reaches_the_log(caplog):
    secret = "s3cr3t-access-value-0123456789"
    headers = [("CF-Access-Client-Id", "client-id-x"), ("CF-Access-Client-Secret", secret), ("CF-Ray", "8f00aa-LAX")]
    rec = Recorder()
    async with tunnel(rec) as srv:
        caplog.set_level(logging.DEBUG)
        caplog.set_level(logging.DEBUG, logger="websockets")
        b = await WsBoard.connect(srv.port, headers=headers)
        await until(lambda: rec.sessions)
        b.write(encode(T.LOG, secret.encode()))
        await b.close()
        other = await WsBoard.open(srv.port, headers=headers, path="/other")
        await asyncio.wait_for(other.eof.wait(), 2)
        await other.close()
        await asyncio.sleep(0.1)
    text = "\n".join(f"{r.name} {r.getMessage()}" for r in caplog.records)
    assert "tunnel session open from '203.0.113.7', CF-Ray '8f00aa-LAX'" in text
    assert secret not in text and "client-id-x" not in text


async def test_closing_the_server_ends_sessions_and_pending_handshakes():
    rec = Recorder()
    srv = TunnelServer(tunnel_cfg(), {IDENTITY: KEY}, rec)
    await srv.start()
    b = await WsBoard.connect(srv.port)
    idle = await _upgrade(srv.port, "198.51.100.1")
    await until(lambda: rec.sessions)
    async with asyncio.timeout(3):
        await srv.close()
    assert await _closed_within(b, 1.0) and await _closed_within(idle, 1.0)
    await b.close()
    await idle.close()


async def test_start_tunnel_needs_keys_and_survives_a_bind_failure(caplog):
    from claude_voice import cli
    from claude_voice.link import Link

    c = tunnel_cfg()
    link = Link(c, lambda: None, None)
    keys = {IDENTITY: KEY}
    assert await cli.start_tunnel(c, link, {}) is None
    assert await cli.start_tunnel(dataclasses.replace(c, tunnel_enabled=False), link, keys) is None
    assert await cli.start_tunnel(dataclasses.replace(c, wifi_enabled=False), link, keys) is None
    srv = await cli.start_tunnel(c, link, keys)
    assert srv is not None and srv.address == "127.0.0.1"
    try:
        with socket.socket() as busy:
            busy.bind(("127.0.0.1", 0))
            busy.listen()
            caplog.set_level(logging.ERROR)
            taken = dataclasses.replace(c, tunnel_port=busy.getsockname()[1])
            assert await cli.start_tunnel(taken, link, keys) is None
        assert any("tunnel link off: cannot listen on port" in r.getMessage() for r in caplog.records)
    finally:
        await srv.close()


@pytest.mark.parametrize(
    "line",
    ["tunnel_port = 65536", "tunnel_port = -1", "tunnel_preauth_max = 0", "tunnel_ws_max_message = -5",
     "tunnel_upgrade_timeout_s = 0.0", "tunnel_write_buffer_max = 0"],
)
def test_config_rejects_bad_tunnel_values(tmp_path, line):
    p = tmp_path / "c.toml"
    p.write_text(line + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match=line.split()[0]):
        load(p)


def test_config_accepts_tunnel_defaults_and_port_zero(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text("tunnel_port = 0\n", encoding="utf-8")
    assert load(p).tunnel_port == 0
    assert Config().tunnel_port == 47802 and Config().tunnel_enabled
