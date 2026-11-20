import asyncio
import contextlib
import dataclasses
import logging
import ssl
import subprocess
import sys

import pytest
from fake_device import FAKE_SERIAL, FakeDevice, client_context, tls_connect
from test_relay import make, settle, until
from test_tunnel import TURN_INPUT, WsBoard

from claude_voice import cli
from claude_voice import keychain as keychain_mod
from claude_voice.config import Config
from claude_voice.keychain import KeychainError, KeychainStore, KeychainValueError, MemoryStore, psk_identity
from claude_voice.link import TransportClosed
from claude_voice.pairing import PairError
from claude_voice.protocol import State, T
from claude_voice.revoke import KeyReloader, revoke
from claude_voice.tunnel import TunnelServer
from claude_voice.wifi import Keyring, WifiServer

IDENTITY = psk_identity(FAKE_SERIAL)
OTHER = psk_identity("AA:BB:CC:DD:EE:01")
KEY = bytes(range(32))
KEY2 = bytes(range(1, 33))
HANDSHAKE_ERRORS = (ssl.SSLError, ConnectionError, TransportClosed, TimeoutError)


def split_records(data: bytes) -> list[bytes]:
    out, i = [], 0
    while i < len(data):
        n = int.from_bytes(data[i + 3 : i + 5], "big")
        out.append(data[i : i + 5 + n])
        i += 5 + n
    return out


def hold_from_ccs(data: bytes) -> tuple[bytes, bytes]:
    """Split a client flight before its ChangeCipherSpec: (ClientKeyExchange and before, CCS and Finished)."""
    records = split_records(data)
    at = next((i for i, r in enumerate(records) if r[0] == 0x14), len(records))
    return b"".join(records[:at]), b"".join(records[at:])


class HeldLanClient:
    """TLS-PSK client on a raw socket that holds back ChangeCipherSpec and Finished until released."""

    def __init__(self, reader, writer, identity, key):
        self.r, self.w = reader, writer
        self._in, self._out = ssl.MemoryBIO(), ssl.MemoryBIO()
        self.tls = client_context(identity, key).wrap_bio(self._in, self._out, server_side=False)
        self.held = b""

    @classmethod
    async def until_held(cls, port, identity=IDENTITY, key=KEY):
        c = cls(*await asyncio.open_connection("127.0.0.1", port), identity, key)
        c._step()
        async with asyncio.timeout(3):
            while not c.held:
                data = await c.r.read(65536)
                assert data, "the relay closed before the client's last flight"
                c._in.write(data)
                c._step(hold=True)
        return c

    def _step(self, hold=False):
        with contextlib.suppress(ssl.SSLWantReadError):
            self.tls.do_handshake()
        data = self._out.read()
        if hold and data:
            data, self.held = hold_from_ccs(data)
            assert self.held, "no ChangeCipherSpec in the client's flight"
        self.w.write(data)

    async def release(self) -> bool:
        """Send the held records; True when the relay keeps the connection open for a session."""
        self.w.write(self.held)
        try:
            async with asyncio.timeout(2):
                while True:
                    data = await self.r.read(65536)
                    if not data:
                        return False
                    self._in.write(data)
                    with contextlib.suppress(ssl.SSLWantReadError):
                        self.tls.do_handshake()
                        break
            self.w.write(self._out.read())
            self.tls.write(b"\xc1\xad")
            self.w.write(self._out.read())
            return not await _eof_within(self.r, 0.5)
        except (ssl.SSLError, ConnectionError):
            return False

    def close(self):
        self.w.close()


class HeldWsBoard(WsBoard):
    """Tunnel board that holds back ChangeCipherSpec and Finished of its inner handshake until released."""

    held = b""

    def _send_records(self):
        if self.handshaken.is_set() or self.held or self.tls.version() is not None:
            return super()._send_records()
        data = self._out.read()
        first, held = hold_from_ccs(data)
        self.held = held
        self.ws.send_binary(first)
        self._flush_ws()

    def release(self):
        self.ws.send_binary(self.held)
        self._flush_ws()


async def _eof_within(reader, seconds):
    try:
        return await asyncio.wait_for(reader.read(1), seconds) == b""
    except TimeoutError:
        return False
    except ConnectionError:
        return True


class Recorder:
    def __init__(self):
        self.sessions = []

    async def __call__(self, t):
        self.sessions.append(t)
        with contextlib.suppress(TransportClosed):
            while True:
                await t.read(0.05)


class LookedUp(dict):
    """Key dict that records the identities the TLS server looked up."""

    def __init__(self, *a):
        super().__init__(*a)
        self.asked = []

    def get(self, identity, default=None):
        self.asked.append(identity)
        return super().get(identity, default)


def rcfg(**over):
    base = dict(wifi_host="127.0.0.1", wifi_port=0, tunnel_port=0, key_reload_interval_s=0.05, wifi_handshake_timeout_s=5.0)
    return dataclasses.replace(Config(), **{**base, **over})


@contextlib.asynccontextmanager
async def servers(store, keys, run_session, cfg=None):
    c = cfg or rcfg()
    ring = Keyring(LookedUp(keys))
    lan, tun = WifiServer(c, ring, run_session), TunnelServer(c, ring, run_session)
    await lan.start()
    await tun.start()
    try:
        yield lan, tun, KeyReloader(c, store, ring, [lan, tun]), ring
    finally:
        await lan.close()
        await tun.close()


def stored(*pairs):
    store = MemoryStore()
    for identity, key in pairs:
        store.set(identity, key)
    return store


@pytest.mark.parametrize("removed", [True, False], ids=["key-removed", "control"])
async def test_lan_handshake_in_flight_never_opens_after_a_removal(removed):
    store, rec = stored((IDENTITY, KEY)), Recorder()
    async with servers(store, {IDENTITY: KEY}, rec) as (lan, _tun, reloader, ring):
        c = await HeldLanClient.until_held(lan.port)
        await until(lambda: ring.keys.asked == [IDENTITY])  # the relay has processed ClientKeyExchange
        if removed:
            store.delete(IDENTITY)
        await reloader.reload()
        assert (IDENTITY in ring.keys) is not removed
        opened = await c.release()
        await asyncio.sleep(0.1)
        assert opened is not removed and bool(rec.sessions) is not removed
        c.close()


@pytest.mark.parametrize("removed", [True, False], ids=["key-removed", "control"])
async def test_tunnel_handshake_in_flight_never_opens_after_a_removal(removed):
    store, rec = stored((IDENTITY, KEY)), Recorder()
    async with servers(store, {IDENTITY: KEY}, rec) as (_lan, tun, reloader, ring):
        b = await HeldWsBoard.open(tun.port)
        await asyncio.wait_for(b.upgraded.wait(), 3)
        b._handshake()
        await until(lambda: b.held and ring.keys.asked == [IDENTITY])
        if removed:
            store.delete(IDENTITY)
        await reloader.reload()
        b.release()
        if removed:
            await asyncio.wait_for(b.eof.wait(), 2)
            await asyncio.sleep(0.1)
            assert rec.sessions == []
        else:
            await until(lambda: rec.sessions)
            assert not b.eof.is_set()
        await b.close()


async def test_removal_after_the_handshake_but_before_the_session_starts_is_refused():
    store, rec = stored((IDENTITY, KEY)), Recorder()
    async with servers(store, {IDENTITY: KEY}, rec) as (lan, tun, reloader, ring):
        spawn = tun.spawn
        tun.spawn = lambda conn: (setattr(ring, "removed", ring.removed + 1), spawn(conn))
        b = await WsBoard.open(tun.port)
        await asyncio.wait_for(b.upgraded.wait(), 3)
        b._handshake()
        await asyncio.wait_for(b.eof.wait(), 2)
        assert rec.sessions == []
        await b.close()


class Live:
    """Relay link behind LAN and tunnel servers sharing one keyring, with the reloader running."""

    def __init__(self, store, keys, **over):
        self.h = make(**{"wifi_host": "127.0.0.1", "wifi_port": 0, "tunnel_port": 0, "key_reload_interval_s": 0.05,
                         "wifi_silence_timeout_s": 3.0, **over})
        self.h.allow_open = False
        self.ring = Keyring(dict(keys))
        self.lan = WifiServer(self.h.cfg, self.ring, self.h.link.serve_wifi)
        self.tun = TunnelServer(self.h.cfg, self.ring, lambda t: self.h.link.serve_wifi(t, "tunnel"))
        self.reloader = KeyReloader(self.h.cfg, store, self.ring, [self.lan, self.tun])
        self.boards = []

    async def __aenter__(self):
        await self.lan.start()
        await self.tun.start()
        self.h.task = asyncio.create_task(self.h.link.run())
        self.reload_task = asyncio.create_task(self.reloader.run())
        return self

    async def board(self, kind, identity=IDENTITY, key=KEY):
        if kind == "lan":
            t = await tls_connect(self.lan.port, identity, key)
        else:
            t = await WsBoard.connect(self.tun.port, ctx=client_context(identity, key))
        d = FakeDevice(t, speed=self.h.speed, mic=self.h.mic, link="wifi" if kind == "lan" else "tunnel")
        d.start()
        self.boards.append(d)
        await until(lambda: T.SET_STATE in d.received)
        return d

    async def __aexit__(self, *exc):
        self.reload_task.cancel()
        self.h.task.cancel()
        for d in self.boards + self.h.devices:
            await d.stop()
            await d.t.close()
        await self.tun.close()
        await self.lan.close()


def closed(d):
    return d.t.closed if isinstance(d.t, WsBoard) else (d.t._closed or d.t._writer.is_closing())


async def _refused(w, kind, identity=IDENTITY, key=KEY):
    with pytest.raises(HANDSHAKE_ERRORS):
        d = await w.board(kind, identity, key)
        for _ in range(20):
            await d.t.read(0.05)


@pytest.mark.parametrize("kind", ["lan", "tunnel"])
async def test_deleted_key_closes_the_live_session_and_refuses_new_handshakes(kind, caplog):
    caplog.set_level(logging.WARNING, logger="claude_voice")
    store = stored((IDENTITY, KEY))
    async with Live(store, {IDENTITY: KEY}) as w:
        d = await w.board(kind)
        assert w.h.link.active_kind == "wifi"
        store.delete(IDENTITY)
        async with asyncio.timeout(0.05 + 1.0):
            await until(lambda: closed(d) and w.h.link.active_kind is None)
        for other in ("lan", "tunnel"):
            await _refused(w, other)
        assert w.h.link.active_kind is None and w.ring.keys == {}
    assert any("deleted or replaced in the Keychain" in r.getMessage() for r in caplog.records)


@pytest.mark.parametrize("error", [KeychainError("locked"), OSError("no security tool")], ids=["keychain", "os"])
async def test_unreadable_keychain_keeps_the_key_and_the_session(error, caplog):
    caplog.set_level(logging.WARNING, logger="claude_voice.revoke")

    class Unreadable(MemoryStore):
        def get(self, identity):
            raise error

    store = Unreadable()
    async with Live(store, {IDENTITY: KEY}) as w:
        d = await w.board("lan")
        pings = d.pings
        await asyncio.sleep(0.4)
        assert not closed(d) and d.pings > pings and w.ring.keys == {IDENTITY: KEY} and w.ring.removed == 0
        second = await w.board("tunnel")
        second.inject(TURN_INPUT)
        await settle(second, State.LISTENING)
    warnings = [r.getMessage() for r in caplog.records if "cannot re-read" in r.getMessage()]
    assert len(warnings) == 1, warnings


class NotHex(MemoryStore):
    def get(self, identity):
        raise KeychainValueError("not a hex key")


@pytest.mark.parametrize("kind", ["lan", "tunnel"])
@pytest.mark.parametrize("value", ["short", "not-hex"])
async def test_a_keychain_value_that_is_not_a_key_drops_the_identity(kind, value, caplog):
    caplog.set_level(logging.WARNING, logger="claude_voice.revoke")
    store = stored((IDENTITY, bytes(16))) if value == "short" else NotHex()
    async with Live(store, {IDENTITY: KEY}) as w:
        d = await w.board(kind)
        await until(lambda: closed(d) and w.h.link.active_kind is None, timeout=2)
        await _refused(w, "lan" if kind == "tunnel" else "tunnel")
        assert w.ring.keys == {} and w.ring.removed == 1
    assert any("is not a 32-byte hex key; dropping it" in r.getMessage() for r in caplog.records)


def test_keychain_value_that_is_not_hex_is_a_distinct_error(monkeypatch):
    out = {"value": "zz-not-hex\n"}
    monkeypatch.setattr(
        keychain_mod.subprocess, "run", lambda args, **kw: subprocess.CompletedProcess(args, 0, out["value"], "")
    )
    with pytest.raises(KeychainValueError):
        KeychainStore("claude-voice", 5.0).get(IDENTITY)
    out["value"] = KEY.hex()[:32] + "\n"
    assert KeychainStore("claude-voice", 5.0).get(IDENTITY) == KEY[:16]


async def test_removal_closes_every_network_session_and_a_still_valid_identity_reconnects():
    store = stored((IDENTITY, KEY), (OTHER, KEY2))
    async with Live(store, {IDENTITY: KEY, OTHER: KEY2}) as w:
        d = await w.board("tunnel", OTHER, KEY2)
        store.delete(IDENTITY)
        await until(lambda: closed(d), timeout=2)
        await _refused(w, "lan")
        again = await w.board("lan", OTHER, KEY2)
        again.inject(TURN_INPUT)
        await settle(again, State.LISTENING)
        assert w.ring.keys == {OTHER: KEY2}


async def test_replaced_key_closes_the_session_and_only_the_new_key_works():
    store = stored((IDENTITY, KEY))
    async with Live(store, {IDENTITY: KEY}) as w:
        d = await w.board("lan")
        store.set(IDENTITY, KEY2)
        await until(lambda: closed(d), timeout=2)
        await _refused(w, "tunnel", IDENTITY, KEY)
        again = await w.board("tunnel", IDENTITY, KEY2)
        assert T.SET_STATE in again.received and w.ring.keys == {IDENTITY: KEY2}


async def test_usb_session_survives_a_removal():
    store = stored((IDENTITY, KEY))
    async with Live(store, {IDENTITY: KEY}) as w:
        w.h.allow_open = True
        await until(lambda: w.h.devices and T.SET_STATE in w.h.dev.received)
        store.delete(IDENTITY)
        await until(lambda: w.ring.removed == 1)
        await asyncio.sleep(0.2)
        assert w.h.link.active_kind == "usb"
        w.h.dev.inject(TURN_INPUT)
        await settle(w.h.dev, State.LISTENING)


def revoke_out(c, store):
    out = []
    return revoke(c, store=store, out=out.append), out


def test_revoke_deletes_the_only_key_and_warns_about_wifi_passwords():
    store = stored((IDENTITY, KEY))
    ok, out = revoke_out(Config(), store)
    assert ok and store.items == {}
    assert out == [
        f"已删除本机钥匙串中设备 {IDENTITY} 的配对密钥。",
        "正在运行的中继会在 10 秒内断开这台设备，之后不再接受它连接。中继如果是在这次更新之前启动的，要重启后才会断开。",
        "拿到设备的人仍能读出其中保存的 Wi-Fi 密码，请修改这些 Wi-Fi 的密码。",
    ]


def test_revoke_with_device_serial_deletes_only_that_key_and_names_the_rest():
    store = stored((OTHER, KEY2), (IDENTITY, KEY))
    ok, out = revoke_out(dataclasses.replace(Config(), device_serial=FAKE_SERIAL), store)
    assert ok and store.items == {OTHER: KEY2}
    assert out[0] == f"已删除本机钥匙串中设备 {IDENTITY} 的配对密钥。"
    assert out[-1] == f"本机钥匙串中还有这些设备的配对密钥：{OTHER}。"


def test_revoke_with_two_keys_and_no_device_serial_deletes_nothing():
    store = stored((OTHER, KEY2), (IDENTITY, KEY))
    with pytest.raises(PairError, match="device_serial") as e:
        revoke_out(Config(), store)
    assert store.items == {OTHER: KEY2, IDENTITY: KEY}
    assert str(e.value) == (
        f"本机钥匙串中有 2 台设备的配对密钥：{OTHER}、{IDENTITY}。"
        "请在 config.toml 中用 device_serial 指定要撤销的设备，再运行 claude-voice revoke。"
    )


def test_revoke_without_keys_or_with_a_missing_or_bad_serial():
    with pytest.raises(PairError) as e:
        revoke_out(Config(), MemoryStore())
    assert str(e.value) == "本机钥匙串中没有配对密钥，没有可撤销的设备。"
    store = stored((OTHER, KEY2))
    ok, out = revoke_out(dataclasses.replace(Config(), device_serial=FAKE_SERIAL), store)
    assert not ok and store.items == {OTHER: KEY2}
    assert out == [f"本机钥匙串中没有设备 {IDENTITY} 的配对密钥。", f"本机钥匙串中还有这些设备的配对密钥：{OTHER}。"]
    with pytest.raises(PairError, match="device_serial 格式不对"):
        revoke_out(dataclasses.replace(Config(), device_serial="AABB"), store)
    assert store.items == {OTHER: KEY2}


def test_revoke_command_runs_without_a_relay_and_never_prints_the_psk(tmp_path, monkeypatch, capsys, caplog):
    caplog.set_level(logging.DEBUG)
    store = stored((IDENTITY, KEY))
    monkeypatch.setattr(cli, "keychain", lambda c: store)
    monkeypatch.setattr(cli, "setup_logging", lambda c, console: None)
    p = tmp_path / "c.toml"
    p.write_text("key_reload_interval_s = 3.0\n", encoding="utf-8")
    monkeypatch.setattr(sys, "argv", ["claude-voice", "--config", str(p), "revoke"])
    with pytest.raises(SystemExit) as e:
        cli.main()
    assert e.value.code == 0 and store.items == {}
    text = capsys.readouterr().out + "\n".join(r.getMessage() for r in caplog.records)
    assert "3 秒内断开" in text and KEY.hex() not in text and KEY.hex().upper() not in text
    monkeypatch.setattr(sys, "argv", ["claude-voice", "--config", str(p), "revoke"])
    with pytest.raises(SystemExit) as e:
        cli.main()
    assert e.value.code == 1 and "没有可撤销的设备" in capsys.readouterr().out


DUMP = """keychain: "/tmp/login.keychain-db"
version: 512
class: "genp"
attributes:
    0x00000007 <blob>="claude-voice"
    "acct"<blob>="clawd-AABBCCDDEEFF"
    "svce"<blob>="claude-voice"
keychain: "/tmp/login.keychain-db"
version: 512
class: "genp"
attributes:
    "acct"<blob>="clawd-AABBCCDDEE01"
    "svce"<blob>="claude-voice-other"
keychain: "/tmp/login.keychain-db"
version: 512
class: "inet"
attributes:
    "acct"<blob>="clawd-AABBCCDDEE02"
    "svce"<blob>="claude-voice"
keychain: "/tmp/login.keychain-db"
version: 512
class: "genp"
attributes:
    "acct"<blob>="not-an-identity"
    "svce"<blob>="claude-voice"
keychain: "/tmp/login.keychain-db"
version: 512
class: "genp"
attributes:
    "acct"<blob>="clawd-AABBCCDDEE03"
    "cdat"<timedate>=0x32303236313132303030303030305A00  "20261120000000Z\\000"
    "svce"<blob>="claude-voice"
"""


class Disagreeing(MemoryStore):
    """A store whose listing misses items or fails while the relay's lookup still finds one."""

    def __init__(self, listed, fail_after=None):
        super().__init__()
        self.listed, self.fail_after, self.listings = listed, fail_after, 0

    def identities(self):
        self.listings += 1
        if self.fail_after is not None and self.listings > self.fail_after:
            raise KeychainError("dump failed")
        return list(self.listed)


@pytest.mark.parametrize("listed", [[], [OTHER]], ids=["listing-empty", "listing-other"])
def test_revoke_deletes_nothing_when_listing_and_lookup_disagree(listed):
    store = Disagreeing(listed)
    store.set(IDENTITY, KEY)
    with pytest.raises(PairError, match="device_serial") as e:
        revoke_out(Config(), store)
    assert store.items == {IDENTITY: KEY} and IDENTITY in str(e.value)


def test_revoke_reports_a_lookup_only_key_and_a_failed_final_listing():
    store = Disagreeing([], fail_after=0)
    store.set(OTHER, KEY2)
    store.set(IDENTITY, KEY)
    ok, out = revoke_out(dataclasses.replace(Config(), device_serial=FAKE_SERIAL), store)
    assert ok and store.items == {OTHER: KEY2}
    assert out[-1] == "无法检查本机钥匙串中是否还有其他设备的配对密钥。"
    store = Disagreeing([])
    store.set(OTHER, KEY2)
    store.set(IDENTITY, KEY)
    ok, out = revoke_out(dataclasses.replace(Config(), device_serial=FAKE_SERIAL), store)
    assert ok and out[-1] == f"本机钥匙串中还有这些设备的配对密钥：{OTHER}。"


async def test_cli_listeners_share_one_keyring_with_the_reloader():
    from claude_voice.link import Link

    c = rcfg()
    store = stored((IDENTITY, KEY))
    link = Link(c, lambda: None, None)
    ring = Keyring(cli.wifi_keys(c, store))
    lan, tun = await cli.start_wifi(c, link, store, ring), await cli.start_tunnel(c, link, ring)
    try:
        reloader = cli.key_reloader(c, store, ring, [lan, tun])
        t = await tls_connect(lan.port, IDENTITY, KEY)
        await t.close()
        store.delete(IDENTITY)
        await reloader.reload()
        with pytest.raises(HANDSHAKE_ERRORS):
            await tls_connect(lan.port, IDENTITY, KEY)
        with pytest.raises(ConnectionError):
            await WsBoard.connect(tun.port)
    finally:
        await lan.close()
        await tun.close()


def test_keychain_identities_lists_this_service_from_attributes_only(monkeypatch):
    calls = []

    def fake_run(args, **kw):
        calls.append(args)
        return subprocess.CompletedProcess(args, 0, DUMP, "")

    monkeypatch.setattr(keychain_mod.subprocess, "run", fake_run)
    assert KeychainStore("claude-voice", 5.0).identities() == ["clawd-AABBCCDDEEFF", "clawd-AABBCCDDEE03"]
    assert calls == [[keychain_mod.SECURITY, "dump-keychain"]]


def test_keychain_identities_fails_loudly(monkeypatch):
    monkeypatch.setattr(keychain_mod.subprocess, "run", lambda args, **kw: subprocess.CompletedProcess(args, 51, "", ""))
    with pytest.raises(KeychainError):
        KeychainStore("claude-voice", 5.0).identities()


def test_config_rejects_a_non_positive_reload_interval(tmp_path):
    from claude_voice.config import load

    p = tmp_path / "c.toml"
    p.write_text("key_reload_interval_s = 0.0\n", encoding="utf-8")
    with pytest.raises(ValueError, match="key_reload_interval_s"):
        load(p)


def test_reloader_runs_only_with_a_listener():
    ring = Keyring({IDENTITY: KEY})
    assert cli.key_reloader(Config(), MemoryStore(), ring, [None, None]) is None
    assert cli.key_reloader(Config(), MemoryStore(), ring, [None, object()]) is not None


class SearchList:
    """Fake `security` whose search list holds `copies` keychains with the same item."""

    def __init__(self, copies, value=None):
        self.copies, self.value, self.deleted, self.calls = copies, value or KEY.hex(), 0, []

    def __call__(self, args, **kw):
        self.calls.append(args[1])
        if args[1] == "delete-generic-password":
            if not self.copies:
                return subprocess.CompletedProcess(args, 44, "", "")
            self.copies -= 1
            self.deleted += 1
            return subprocess.CompletedProcess(args, 0, "", "")
        if args[1] == "find-generic-password":
            if not self.copies:
                return subprocess.CompletedProcess(args, 44, "", "")
            return subprocess.CompletedProcess(args, 0, self.value + "\n", "")
        raise AssertionError(args)


def test_keychain_delete_removes_the_item_from_every_keychain(monkeypatch):
    fake = SearchList(2)
    monkeypatch.setattr(keychain_mod.subprocess, "run", fake)
    store = KeychainStore("claude-voice", 5.0)
    assert store.delete(IDENTITY) and store.get(IDENTITY) is None
    assert fake.deleted == 2 and fake.calls == ["delete-generic-password"] * 3 + ["find-generic-password"]
    assert not store.delete(IDENTITY)


def test_keychain_delete_that_never_reaches_not_found_fails(monkeypatch):
    fake = SearchList(10**6)
    monkeypatch.setattr(keychain_mod.subprocess, "run", fake)
    with pytest.raises(KeychainError, match="still found"):
        KeychainStore("claude-voice", 5.0).delete(IDENTITY)
    assert fake.deleted == keychain_mod.DELETE_MAX
    monkeypatch.setattr(keychain_mod.subprocess, "run", lambda args, **kw: subprocess.CompletedProcess(args, 51, "", ""))
    with pytest.raises(KeychainError, match="exited with 51"):
        KeychainStore("claude-voice", 5.0).delete(IDENTITY)


class StillThere(MemoryStore):
    """A store whose delete reports success while its lookup still reads a value."""

    def __init__(self, error=None):
        super().__init__()
        self.error = error

    def delete(self, identity):
        super().delete(identity)
        return True

    def get(self, identity):
        if self.error is not None:
            raise self.error
        return KEY


@pytest.mark.parametrize(
    "error", [None, KeychainValueError("not hex")], ids=["key-readable", "not-a-key"]
)
def test_revoke_fails_when_the_key_is_still_readable_after_the_delete(error):
    store = StillThere(error)
    store.set(IDENTITY, KEY)
    ok, out = revoke_out(dataclasses.replace(Config(), device_serial=FAKE_SERIAL), store)
    assert not ok
    assert out == [f"删除后本机钥匙串中仍能读到设备 {IDENTITY} 的配对密钥，撤销没有完成。"]


def test_revoke_lets_an_unreadable_keychain_after_the_delete_propagate():
    store = StillThere(KeychainError("locked"))
    store.set(IDENTITY, KEY)
    with pytest.raises(KeychainError, match="locked"):
        revoke_out(dataclasses.replace(Config(), device_serial=FAKE_SERIAL), store)


def test_psk_identity_is_uppercase_whatever_the_serial_case():
    assert psk_identity("aa:bb:cc:dd:ee:ff") == "clawd-AABBCCDDEEFF" == psk_identity("AA:bb:CC:dd:EE:ff")


@pytest.mark.parametrize(
    "paired, revoked", [("aa:bb:cc:dd:ee:ff", "AA:BB:CC:DD:EE:FF"), ("AA:BB:CC:DD:EE:FF", "aa:bb:cc:dd:ee:ff")]
)
async def test_revoke_finds_the_key_paired_under_the_other_serial_case(paired, revoked):
    from test_pairing import PASSWORD, Board
    from test_pairing import cfg as pair_cfg

    from claude_voice.pairing import pair

    store = MemoryStore()
    async with Board() as b:
        assert await pair(pair_cfg(), transport=b.host, store=store, serial=paired, ssid="家里的网",
                          password=PASSWORD, out=[].append)
    assert list(store.items) == [IDENTITY] and b.dev.paired["psk_identity"] == IDENTITY
    ok, out = revoke_out(dataclasses.replace(Config(), device_serial=revoked), store)
    assert ok and store.items == {}, out


def test_relay_lookup_finds_a_key_whatever_the_device_serial_case():
    from claude_voice.wifi import load_keys

    store = stored((psk_identity("AA:BB:CC:DD:EE:FF"), KEY))
    assert load_keys(dataclasses.replace(Config(), device_serial="aa:bb:cc:dd:ee:ff"), store) == {IDENTITY: KEY}


PHANTOMS = """keychain: "/tmp/login.keychain-db"
version: 512
class: "genp"
attributes:
    0x00000007 <blob>=""svce"<blob>="claude-voice""
    "acct"<blob>="clawd-AABBCCDDEE04"
    "svce"<blob>="claude-voice-other"
keychain: "/tmp/login.keychain-db"
version: 512
class: "genp"
attributes:
    "acct"<blob>=<NULL>
    "icmt"<blob>=""acct"<blob>="clawd-AABBCCDDEE05""
    "svce"<blob>="claude-voice"
keychain: "/tmp/login.keychain-db"
version: 512
class: "genp"
attributes:
    0x00000007 <blob>="claude-voice"
    "acct"<blob>="clawd-AABBCCDDEEFF"
    "svce"<blob>="claude-voice"
"""


def test_keychain_attributes_quoted_in_a_label_or_comment_are_not_read(monkeypatch):
    output = {"text": PHANTOMS}
    monkeypatch.setattr(
        keychain_mod.subprocess, "run", lambda args, **kw: subprocess.CompletedProcess(args, 0, output["text"], "")
    )
    store = KeychainStore("claude-voice", 5.0)
    assert store.identities() == ["clawd-AABBCCDDEEFF"]
    label, comment, genuine = PHANTOMS.split('keychain: "/tmp/login.keychain-db"\n')[1:]
    output["text"] = label.replace('"acct"<blob>="clawd-AABBCCDDEE04"', '"acct"<blob>="clawd-AABBCCDDEEFF"').replace(
        '""svce"<blob>="claude-voice""', '""acct"<blob>="clawd-AABBCCDDEE06""'
    )
    assert store.find_any() == "clawd-AABBCCDDEEFF"
    output["text"] = comment
    assert store.find_any() is None


def test_redact_hides_overlapping_secrets_whole():
    from claude_voice.pairing import redact

    secrets_ = ["password1", "password1234", ""]
    text = redact("a=password1234 b=password1 c", secrets_)
    assert text == "a=*** b=*** c" and secrets_ == ["password1", "password1234", ""]
