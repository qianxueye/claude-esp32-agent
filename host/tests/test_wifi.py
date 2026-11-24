import asyncio
import contextlib
import dataclasses
import logging
import shutil
import socket
import ssl
import struct
import subprocess

import pytest
from fake_device import FAKE_SERIAL, FakeDevice, client_context, tls_connect
from test_relay import make, settle, until

from claude_voice.config import Config
from claude_voice.keychain import MemoryStore, key_id, psk_identity, valid_identity, wire_identity
from claude_voice.link import TransportClosed
from claude_voice.protocol import Input, State, T, encode
from claude_voice.wifi import (
    CIPHER,
    TcpTransport,
    WifiServer,
    load_keys,
    parse_wire_identity,
    server_context,
)

BOARD = psk_identity(FAKE_SERIAL)
KEY = bytes(range(32))
IDENTITY = wire_identity(BOARD, key_id(KEY))  # what the board sends in the handshake
OPENSSL = "/opt/homebrew/opt/openssl@3/bin/openssl"


def wifi_cfg(**over):
    return dataclasses.replace(Config(), wifi_host="127.0.0.1", wifi_port=0, **over)


class Recorder:
    def __init__(self):
        self.sessions = []

    async def __call__(self, t):
        self.sessions.append(t)
        with contextlib.suppress(TransportClosed):
            while True:
                await t.read(0.05)


@contextlib.asynccontextmanager
async def server(run_session, keys=None, cfg=None):
    srv = WifiServer(cfg or wifi_cfg(), {IDENTITY: KEY} if keys is None else keys, run_session)
    await srv.start()
    try:
        yield srv
    finally:
        await srv.close()


async def test_python_client_completes_the_exact_handshake():
    rec = Recorder()
    async with server(rec) as srv:
        t = await tls_connect(srv.port, IDENTITY, KEY)
        tls = t._writer.get_extra_info("ssl_object")
        assert tls.version() == "TLSv1.2" and tls.cipher()[0] == CIPHER
        await until(lambda: rec.sessions)
        sock = rec.sessions[0]._writer.get_extra_info("socket")
        assert sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY)
        await t.close()


def test_server_context_allows_tls12_and_one_suite_only():
    ctx = server_context({IDENTITY: KEY})
    assert ctx.minimum_version == ctx.maximum_version == ssl.TLSVersion.TLSv1_2
    tls12 = [c["name"] for c in ctx.get_ciphers() if c["protocol"] != "TLSv1.3"]
    assert tls12 == [CIPHER]


@pytest.mark.parametrize(
    "identity,key,kw",
    [
        (IDENTITY, bytes(32), {}),
        (wire_identity(psk_identity("AA:BB:CC:DD:EE:00"), key_id(KEY)), KEY, {}),
        (BOARD, KEY, {}),
        (wire_identity(BOARD, "00000000"), KEY, {}),
        (IDENTITY.lower(), KEY, {}),
        ("not-an-identity", KEY, {}),
        (IDENTITY, KEY, {"cipher": "ECDHE-PSK-AES128-CBC-SHA256"}),
        (IDENTITY, KEY, {"cipher": "PSK-CHACHA20-POLY1305"}),
        (IDENTITY, KEY, {"tls13": True}),
    ],
    ids=["wrong-key", "wrong-identity", "bare-board-identity", "other-key-id", "lowercase-key-id", "malformed-identity",
         "other-cipher", "no-ecdhe", "tls13"],
)
async def test_handshake_fails_and_nothing_is_processed(identity, key, kw):
    rec = Recorder()
    async with server(rec) as srv:
        with pytest.raises((ssl.SSLError, ConnectionError, TransportClosed, TimeoutError)):
            t = await tls_connect(srv.port, identity, key, **kw)
            t.write(encode(T.HELLO, b'{"proto":3}'))
            await t.read(1.0)
            await t.read(1.0)
        await asyncio.sleep(0.05)
        assert rec.sessions == []


def s_client(port, *args):
    return subprocess.run(
        [OPENSSL, "s_client", "-connect", f"127.0.0.1:{port}", *args],
        stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10,
    )


@pytest.mark.skipif(not shutil.which(OPENSSL), reason="Homebrew openssl@3 not installed")
@pytest.mark.parametrize(
    "args,ok",
    [
        (["-tls1_2", "-cipher", CIPHER, "-psk_identity", IDENTITY, "-psk", KEY.hex()], True),
        (["-tls1_2", "-cipher", CIPHER, "-psk_identity", IDENTITY, "-psk", "00" * 32], False),
        (["-tls1_2", "-cipher", CIPHER, "-psk_identity", "clawd-000000000000", "-psk", KEY.hex()], False),
        (["-tls1_2", "-cipher", "PSK-AES128-CBC-SHA256", "-psk_identity", IDENTITY, "-psk", KEY.hex()], False),
        (["-tls1_3", "-psk_identity", IDENTITY, "-psk", KEY.hex()], False),
    ],
    ids=["exact", "wrong-key", "wrong-identity", "other-cipher", "tls13"],
)
async def test_openssl_s_client(args, ok):
    rec = Recorder()
    async with server(rec) as srv:
        r = await asyncio.to_thread(s_client, srv.port, *args)
        await asyncio.sleep(0.05)
        # s_client prints the negotiated suite even when the server then rejects its Finished
        established = r.returncode == 0 and f"Cipher is {CIPHER}" in r.stdout and "TLSv1.2" in r.stdout
        assert established is ok, r.stdout[-400:]
        assert bool(rec.sessions) is ok


def test_key_id_and_wire_identity_of_one_fixed_psk():
    assert key_id(bytes(range(32))) == "630DCD29"
    assert IDENTITY == BOARD + "-630DCD29" and parse_wire_identity(IDENTITY) == (BOARD, "630DCD29")
    for other in (BOARD, IDENTITY.lower(), IDENTITY + "0", IDENTITY[:-1], "x" + IDENTITY, None, ""):
        assert parse_wire_identity(other) is None, other
    assert valid_identity(BOARD) and not valid_identity(IDENTITY)


def test_load_keys_by_pinned_serial_or_single_item():
    store = MemoryStore()
    assert load_keys(Config(), store) == {}
    store.set(BOARD, KEY)
    assert load_keys(Config(), store) == {IDENTITY: KEY}
    assert load_keys(dataclasses.replace(Config(), device_serial=FAKE_SERIAL), store) == {IDENTITY: KEY}
    assert load_keys(dataclasses.replace(Config(), device_serial="AA:BB:CC:DD:EE:00"), store) == {}
    store.set(BOARD, b"short")
    assert load_keys(Config(), store) == {}


class WifiHarness:
    def __init__(self, **over):
        self.h = make(**{"wifi_host": "127.0.0.1", "wifi_port": 0, "wifi_silence_timeout_s": 3.0, **over})
        self.h.allow_open = False
        self.srv = WifiServer(self.h.cfg, {IDENTITY: KEY}, self.h.link.serve_wifi)
        self.boards = []

    async def __aenter__(self):
        await self.srv.start()
        self.h.task = asyncio.create_task(self.h.link.run())
        return self

    async def board(self, **kw):
        t = await tls_connect(self.srv.port, IDENTITY, KEY)
        d = FakeDevice(t, speed=self.h.speed, mic=self.h.mic, link="wifi", **kw)
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


async def test_full_turn_over_wifi():
    async with WifiHarness() as w:
        d = await w.board()
        assert d.host_hellos == 1 and d.hellos == ["reply"] and w.h.link.active_kind == "wifi"
        d.inject(Input.BOOT_BUTTON)
        await until(lambda: d.states[-1:] == [State.IDLE] and State.SPEAKING in d.states)
        assert w.h.stt.calls and w.h.agent.asked == ["你好"]
        assert d.max_queued <= 64000 and d.flushes == 1


async def test_plain_tcp_frames_are_never_processed():
    async with WifiHarness() as w:
        _r, wr = await asyncio.open_connection("127.0.0.1", w.srv.port)
        wr.write(encode(T.HELLO, b'{"proto":3,"reason":"boot"}') + encode(T.INPUT, bytes([Input.BOOT_BUTTON])))
        await wr.drain()
        await asyncio.sleep(0.3)
        assert w.h.link.active_kind is None and w.h.relay.state == State.IDLE and not w.h.stt.calls
        wr.close()


async def test_failed_handshake_leaves_live_session_intact():
    async with WifiHarness() as w:
        d = await w.board()
        with pytest.raises((ssl.SSLError, ConnectionError)):
            await tls_connect(w.srv.port, IDENTITY, bytes(32))
        pings = d.pings
        await until(lambda: d.pings > pings + 2)
        assert w.h.link.active_kind == "wifi" and not d.t._closed
        d.inject(Input.BOOT_BUTTON)
        await settle(d, State.LISTENING)


async def test_new_wifi_session_replaces_old():
    async with WifiHarness() as w:
        first = await w.board()
        second = await w.board()
        await until(lambda: first.t._closed or first.t._writer.is_closing())
        with pytest.raises(TransportClosed):
            for _ in range(20):
                await first.t.read(0.05)
        second.inject(Input.BOOT_BUTTON)
        await settle(second, State.LISTENING)


async def test_usb_present_refuses_wifi():
    async with WifiHarness() as w:
        w.h.allow_open = True
        await until(lambda: w.h.devices and T.SET_STATE in w.h.dev.received)
        t = await tls_connect(w.srv.port, IDENTITY, KEY)
        d = FakeDevice(t, link="wifi")
        d.start()
        w.boards.append(d)
        with pytest.raises(TransportClosed):
            for _ in range(40):
                await t.read(0.05)
        assert d.host_hellos == 0 and w.h.link.active_kind == "usb"


async def test_usb_takes_over_from_wifi():
    async with WifiHarness() as w:
        d = await w.board()
        w.h.allow_open = True
        await until(lambda: w.h.devices and T.SET_STATE in w.h.dev.received)
        assert w.h.link.active_kind == "usb"
        await until(lambda: d.t._closed or d.t._writer.is_closing())
        d.inject(Input.BOOT_BUTTON)
        await asyncio.sleep(0.2)
        assert w.h.relay.state == State.IDLE
        w.h.dev.inject(Input.BOOT_BUTTON)
        await settle(w.h.dev, State.LISTENING)


async def test_tcp_keeps_partial_frame_beyond_200ms():
    async with WifiHarness(wifi_silence_timeout_s=5.0) as w:
        t = await tls_connect(w.srv.port, IDENTITY, KEY)
        t.write(encode(T.HELLO, b'{"proto":3,"reason":"reply","link":"wifi"}'))
        await until(lambda: w.h.link.connected)
        frame = encode(T.SPK_STATUS, struct.pack("<IBI", 0, 0, 1234))
        t.write(frame[:3])
        await asyncio.sleep(0.35)
        t.write(frame[3:])
        await until(lambda: w.h.relay.speaker.synced)
        await t.close()


async def test_silent_wifi_session_is_closed():
    async with WifiHarness(wifi_silence_timeout_s=0.5) as w:
        t = await tls_connect(w.srv.port, IDENTITY, KEY)
        await until(lambda: w.h.link.active_kind == "wifi")
        await until(lambda: w.h.link.active_kind is None, timeout=3)
        await t.close()


async def _closed_within(reader, seconds):
    try:
        return await asyncio.wait_for(reader.read(1), seconds) == b""
    except TimeoutError:
        return False
    except ConnectionError:
        return True


async def test_idle_connections_over_the_cap_close_at_once_and_spare_the_live_session():
    async with WifiHarness(wifi_preauth_max=4, wifi_preauth_per_addr_max=12, wifi_handshake_timeout_s=1.0) as w:
        d = await w.board()
        idle = [await asyncio.open_connection("127.0.0.1", w.srv.port) for _ in range(12)]
        closed = await asyncio.gather(*(_closed_within(r, 0.3) for r, _ in idle))
        assert sum(closed) == 8 and not any(closed[:4]), closed
        pings = d.pings
        d.inject(Input.BOOT_BUTTON)
        await settle(d, State.LISTENING)
        await until(lambda: d.pings > pings + 2)
        assert w.h.link.active_kind == "wifi" and not d.t._closed
        async with asyncio.timeout(4):
            while True:
                with contextlib.suppress(ssl.SSLError, ConnectionError, TimeoutError):
                    t = await tls_connect(w.srv.port, IDENTITY, KEY)
                    break
                await asyncio.sleep(0.1)
        assert t._writer.get_extra_info("ssl_object").cipher()[0] == CIPHER
        w.boards.append(FakeDevice(t, link="wifi"))
        for _, wr in idle:
            wr.close()


class FakePeers:
    """Maps each client's local port to a fake peer address, set before the client connects."""

    def __init__(self):
        self.addr = {}

    def __call__(self, transport):
        return self.addr[transport.get_extra_info("peername")[1]]

    async def connect(self, port, addr, **kw):
        sock = socket.socket()
        sock.bind(("127.0.0.1", 0))
        self.addr[sock.getsockname()[1]] = addr
        sock.setblocking(False)
        await asyncio.get_running_loop().sock_connect(sock, ("127.0.0.1", port))
        return await asyncio.open_connection(sock=sock, **kw)

    async def tls(self, port, addr, key=KEY):
        r, wr = await asyncio.wait_for(
            self.connect(port, addr, ssl=client_context(IDENTITY, key), server_hostname="relay"), 5
        )
        return TcpTransport(r, wr, 1 << 20)


async def test_one_address_at_its_limit_does_not_block_another():
    peers, rec = FakePeers(), Recorder()
    c = wifi_cfg(wifi_preauth_max=4, wifi_preauth_per_addr_max=2, wifi_handshake_timeout_s=5.0)
    srv = WifiServer(c, {IDENTITY: KEY}, rec, peer_key=peers)
    await srv.start()
    try:
        idle = [await peers.connect(srv.port, "10.0.0.5") for _ in range(6)]
        closed = await asyncio.gather(*(_closed_within(r, 0.3) for r, _ in idle))
        assert closed == [False, False, True, True, True, True], closed
        t = await peers.tls(srv.port, "10.0.0.6")
        await until(lambda: rec.sessions)
        await t.close()
        for _, wr in idle:
            wr.close()
    finally:
        await srv.close()


async def test_address_reconnects_after_more_failed_handshakes_than_its_limit():
    peers, rec = FakePeers(), Recorder()
    c = wifi_cfg(wifi_preauth_per_addr_max=2, wifi_handshake_timeout_s=5.0)
    srv = WifiServer(c, {IDENTITY: KEY}, rec, peer_key=peers)
    await srv.start()
    try:
        for _ in range(3):
            with pytest.raises((ssl.SSLError, ConnectionError)):
                await peers.tls(srv.port, "10.0.0.5", key=bytes(32))
            _r, wr = await peers.connect(srv.port, "10.0.0.5")
            wr.write(b"GET / HTTP/1.0\r\n\r\n")
            wr.close()
        await until(lambda: srv._preauth == 0)
        t = await peers.tls(srv.port, "10.0.0.5")
        await until(lambda: rec.sessions)
        await t.close()
    finally:
        await srv.close()


async def test_close_returns_with_connections_still_in_the_handshake():
    rec = Recorder()
    srv = WifiServer(wifi_cfg(), {IDENTITY: KEY}, rec)
    await srv.start()
    idle = [await asyncio.open_connection("127.0.0.1", srv.port) for _ in range(3)]
    async with asyncio.timeout(1.5):
        await srv.close()
    for _, wr in idle:
        wr.close()


def _resume_twice(port):
    ctx = client_context(IDENTITY, KEY)
    session, reused = None, []
    for _ in range(2):
        with socket.create_connection(("127.0.0.1", port), timeout=2) as raw:
            with ctx.wrap_socket(raw, server_hostname=None, session=session) as tls:
                reused.append(tls.session_reused)
                session = tls.session
                tls.sendall(encode(T.HELLO, b'{"proto":3}'))
                with contextlib.suppress(OSError):
                    tls.recv(16)
    return reused


async def test_resumed_tls_session_is_closed_without_processing():
    received = []

    async def run(t):
        received.append(await t.read(0.5))

    async with server(run) as srv:
        reused = await asyncio.to_thread(_resume_twice, srv.port)
        await asyncio.sleep(0.2)
    assert reused == [False, True], "the client must resume the first session"
    assert len(received) == 1


async def test_failed_handshake_warnings_are_rate_limited(caplog):
    caplog.set_level(logging.WARNING, logger="claude_voice.wifi")
    async with server(Recorder(), cfg=wifi_cfg(wifi_warn_interval_s=60.0)) as srv:
        for _ in range(5):
            with pytest.raises((ssl.SSLError, ConnectionError)):
                await tls_connect(srv.port, psk_identity("AA:BB:CC:DD:EE:00"), KEY)
    unknown = f"Wi-Fi handshake with unknown PSK identity {psk_identity('AA:BB:CC:DD:EE:00')!r}"
    assert [r.getMessage() for r in caplog.records if r.name == "claude_voice.wifi"] == [
        unknown, unknown + " (3 more suppressed)",
    ]


async def test_warning_kinds_are_limited_apart_and_pending_counts_logged_at_close(caplog):
    caplog.set_level(logging.WARNING, logger="claude_voice.wifi")
    stranger = psk_identity("AA:BB:CC:DD:EE:00")
    c = wifi_cfg(wifi_preauth_max=1, wifi_warn_interval_s=60.0)
    async with server(Recorder(), cfg=c) as srv:
        for _ in range(2):
            _r, held = await asyncio.open_connection("127.0.0.1", srv.port)
            await until(lambda: srv._preauth == 1)
            refused, _w = await asyncio.open_connection("127.0.0.1", srv.port)
            assert await _closed_within(refused, 1.0)
            held.close()
            await until(lambda: srv._preauth == 0)
            with pytest.raises((ssl.SSLError, ConnectionError)):
                await tls_connect(srv.port, stranger, KEY)
    cap = "Wi-Fi connection refused: 1 already in the TLS handshake"
    unknown = f"Wi-Fi handshake with unknown PSK identity {stranger!r}"
    assert sorted(r.getMessage() for r in caplog.records if r.name == "claude_voice.wifi") == sorted(
        [cap, unknown, cap, unknown]
    )


def test_warn_limiter_reports_suppressed_count(caplog):
    from claude_voice.wifi import WarnLimiter

    now = [0.0]
    limiter = WarnLimiter(logging.getLogger("claude_voice.wifi"), 10.0, clock=lambda: now[0])
    caplog.set_level(logging.WARNING, logger="claude_voice.wifi")
    for t in (0.0, 1.0, 2.0, 9.9, 10.0, 11.0):
        now[0] = t
        limiter.warn("bad handshake from %s", "x")
    assert [r.getMessage() for r in caplog.records] == [
        "bad handshake from x", "bad handshake from x (3 more suppressed)",
    ]


def test_warn_limiter_flush_logs_the_pending_count_once(caplog):
    from claude_voice.wifi import WarnLimiter

    now = [0.0]
    limiter = WarnLimiter(logging.getLogger("claude_voice.wifi"), 10.0, clock=lambda: now[0])
    caplog.set_level(logging.WARNING, logger="claude_voice.wifi")
    limiter.flush()
    for t, who in ((0.0, "a"), (1.0, "b"), (2.0, "c"), (3.0, "d")):
        now[0] = t
        limiter.warn("bad handshake from %s", who)
    limiter.flush()
    limiter.flush()
    assert [r.getMessage() for r in caplog.records] == [
        "bad handshake from a", "bad handshake from d (2 more suppressed)",
    ]


async def test_peers_dropping_mid_handshake_log_nothing(caplog):
    caplog.set_level(logging.DEBUG, logger="asyncio")
    caplog.set_level(logging.WARNING, logger="claude_voice")
    rec = Recorder()
    async with server(rec) as srv:
        for payload in (b"", b"\x16\x03\x01\x00\x05hello", b"GET / HTTP/1.0\r\n\r\n"):
            for _ in range(5):
                _r, wr = await asyncio.open_connection("127.0.0.1", srv.port)
                wr.write(payload)
                wr.close()
        await asyncio.sleep(0.3)
        t = await tls_connect(srv.port, IDENTITY, KEY)
        await until(lambda: rec.sessions)
        await t.close()
    assert [r.getMessage() for r in caplog.records if r.levelno >= logging.WARNING] == []
