import asyncio
import dataclasses
import json
import logging
import os
import socket
import ssl
import subprocess
import uuid

import pytest
from fake_device import FAKE_SERIAL, FakeDevice, board_identity, tls_connect

from claude_voice import keychain as keychain_mod
from claude_voice.config import Config
from claude_voice.keychain import KeychainError, KeychainStore, MemoryStore, key_id, psk_identity, wire_identity
from claude_voice.link import memory_pair
from claude_voice.pairing import PairError, forget, pair, private_ipv4
from claude_voice.protocol import T

PASSWORD = "fake-Pa55word-Zq9!"
IDENTITY = psk_identity(FAKE_SERIAL)
KEY32 = bytes(range(32))


def cfg(**over):
    base = dict(
        wifi_host="127.0.0.1", wifi_port=0, pair_relay_names=["relay-test.local"], pair_relay_addrs=["10.0.0.2"],
        usb_hello_retry_s=0.02, usb_open_timeout_s=2.0, ping_interval_s=0.05, read_poll_s=0.01,
        pair_result_timeout_s=1.0, pair_relay_timeout_s=1.0, forget_timeout_s=1.0,
    )
    return dataclasses.replace(Config(), **{**base, **over})


class Board:
    """Fake board on USB whose Wi-Fi join performs a real TLS-PSK handshake with the relay."""

    def __init__(self, key_override=None, **kw):
        self.host, dev = memory_pair()
        self.sessions = []
        self.wifi_rx = None
        self.key_override = key_override
        self.dev = FakeDevice(dev, link="usb", relay_connector=self._connect, **kw)

    async def _connect(self, info):
        key = self.key_override or bytes.fromhex(info["psk"])
        try:
            t = await tls_connect(info["relay_port"], board_identity(info), key)
            self.sessions.append(t)
            self.wifi_rx = await t.read(0.2)
        except (ssl.SSLError, ConnectionError):
            return "auth_failed"
        return "connected"

    async def __aenter__(self):
        self.dev.start()
        return self

    async def __aexit__(self, *exc):
        await self.dev.stop()
        for t in self.sessions:
            await t.close()


async def run_pair(board, c=None, store=None, out=None, ssid="家里的网"):
    return await pair(
        c or cfg(), transport=board.host, store=store if store is not None else MemoryStore(),
        serial=FAKE_SERIAL, ssid=ssid, password=PASSWORD, out=(out if out is not None else []).append,
    )


async def test_pair_end_to_end_without_leaking_secrets(caplog, capsys):
    caplog.set_level(logging.DEBUG)
    store, out = MemoryStore(), []
    async with Board() as b:
        b.dev.leaky_log = True
        ok = await run_pair(b, store=store, out=out)
    assert ok, out
    sent = b.dev.paired
    key = store.get(IDENTITY)
    assert key is not None and len(key) == 32 and sent["psk"] == key.hex() and sent["key_id"] == key_id(key)
    assert sent["password"] == PASSWORD and sent["ssid"] == "家里的网" and sent["psk_identity"] == IDENTITY
    assert sent["relay_names"] == ["relay-test.local"] and sent["relay_addrs"] == ["10.0.0.2"]
    assert b.sessions and b.wifi_rx == b"", "TLS-PSK session with the new key, held without HOST_HELLO"
    assert out[-1].startswith("配对完成")
    captured = capsys.readouterr()
    everything = "\n".join([caplog.text, captured.out, captured.err, *out])
    assert "家里的网" in everything
    assert PASSWORD not in everything and key.hex() not in everything and key.hex().upper() not in everything


async def test_pair_log_shows_only_the_ssid(caplog):
    caplog.set_level(logging.DEBUG)
    async with Board() as b:
        assert await run_pair(b, ssid="家里的网")
    sent = b.dev.paired
    logged = "\n".join(r.getMessage() for r in caplog.records)
    assert "家里的网" in logged
    for value in (sent["psk_identity"], *sent["relay_names"], *sent["relay_addrs"], str(sent["relay_port"])):
        assert value not in logged, value


OLD_KEY = b"\x01" * 32
EARLIER_KEY = b"\x03" * 32
STALE_KEY = "钥匙串中的配对密钥可能与设备不一致。请重新运行 claude-voice pair。"


def kept(store, *keys):
    """The store holds exactly `keys`: the first as the current key, the rest as earlier keys."""
    want = {IDENTITY: keys[0], **{f"{IDENTITY}-earlier-{key_id(k)}": k for k in keys[1:]}}
    return store.items == want


async def test_pair_without_result_or_join_is_unknown_and_keeps_every_key():
    store, out = MemoryStore(), []
    store.set(IDENTITY, OLD_KEY)
    async with Board() as b:
        b.dev.ignore_pair = True
        assert not await run_pair(b, store=store, out=out)
    new = store.get(IDENTITY)
    assert T.PAIR in b.dev.received and new not in (None, OLD_KEY) and kept(store, new, OLD_KEY)
    assert not any("配对失败" in line for line in out)
    assert out[-1] == "无法确认配对结果：设备没有回复，也没有通过 Wi-Fi 连上本机。请重新运行 claude-voice pair。"


async def test_pair_board_joining_with_new_key_is_success_without_result():
    store, out = MemoryStore(), []
    store.set(IDENTITY, OLD_KEY)
    store.set_earlier(IDENTITY, key_id(EARLIER_KEY), EARLIER_KEY)
    async with Board() as b:
        b.dev.pair_reply = False
        assert await run_pair(b, store=store, out=out), out
    assert T.PAIR_RESULT not in b.dev.received and b.sessions
    assert kept(store, bytes.fromhex(b.dev.paired["psk"])) and out[-1].startswith("配对完成")


class FailingRestoreStore(MemoryStore):
    """Accepts the new key, then fails when pairing puts the old one back."""

    def set(self, identity, key):
        if identity in self.items:
            raise KeychainError("locked")
        super().set(identity, key)

    def delete(self, identity):
        raise KeychainError("locked")


async def test_pair_closes_usb_and_listener_when_restore_raises():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    out = []
    async with Board() as b:
        b.dev.refuse_pair = True
        with pytest.raises(KeychainError):
            await run_pair(b, c=cfg(wifi_port=port), store=FailingRestoreStore(), out=out)
        assert b.host.closed
    with pytest.raises(OSError):
        socket.create_connection(("127.0.0.1", port), timeout=1).close()
    assert "配对失败" in out[-2] and out[-1] == STALE_KEY


class FailingSecondSetStore(MemoryStore):
    """Holds a previous key, accepts the new one, then fails when pairing puts the old one back."""

    def __init__(self, old):
        super().__init__()
        self.items[IDENTITY] = old
        self.sets = 0

    def set(self, identity, key):
        self.sets += 1
        if self.sets > 1:
            raise KeychainError("locked")
        super().set(identity, key)


async def test_pair_tells_user_to_pair_again_when_previous_key_cannot_be_restored():
    store, out = FailingSecondSetStore(OLD_KEY), []
    async with Board() as b:
        b.dev.refuse_pair = True
        with pytest.raises(KeychainError):
            await run_pair(b, store=store, out=out)
    assert store.get(IDENTITY) not in (None, OLD_KEY)
    assert "配对失败" in out[-2] and out[-1] == STALE_KEY


async def test_pair_interrupted_after_sending_is_unknown_and_keeps_every_key():
    store, out = MemoryStore(), []
    store.set(IDENTITY, OLD_KEY)
    async with Board() as b:
        b.dev.ignore_pair = True
        task = asyncio.create_task(run_pair(b, store=store, out=out))
        async with asyncio.timeout(3):
            while T.PAIR not in b.dev.received:
                await asyncio.sleep(0.01)
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
    new = store.get(IDENTITY)
    assert new not in (None, OLD_KEY) and kept(store, new, OLD_KEY) and b.host.closed
    assert not any("配对失败" in line for line in out) and out[-1] == "配对已中断，无法确认结果。请重新运行 claude-voice pair。"


class CurrentSetFails(MemoryStore):
    """Accepts the earlier-key copy of the old key, then fails to write the new current key."""

    def set(self, identity, key):
        if identity in self.items:
            raise KeychainError("locked")
        super().set(identity, key)


async def test_pair_failing_before_sending_restores_every_item():
    store = CurrentSetFails()
    store.items[IDENTITY] = OLD_KEY
    store.set_earlier(IDENTITY, key_id(EARLIER_KEY), EARLIER_KEY)
    before = dict(store.items)
    async with Board() as b:
        with pytest.raises(KeychainError):
            await run_pair(b, store=store)
    assert T.PAIR not in b.dev.received and store.items == before


UNKNOWN_KEPT = (
    "设备保存配对时出错，无法确认是否已经保存。本机钥匙串保留了这次的配对密钥，之前的配对密钥也没有删除，"
    "设备用其中哪一个，本机都能接受。要重新配对，请先运行 claude-voice forget，再运行 claude-voice pair。"
)
UNKNOWN_FIRST = (
    "设备保存配对时出错，无法确认是否已经保存。本机钥匙串保留了这次的配对密钥。"
    "要重新配对，请先运行 claude-voice forget，再运行 claude-voice pair。"
)


async def pair_storage_unknown(store, out):
    async with Board() as b:
        b.dev.refuse_pair, b.dev.refuse_reason = True, "storage unknown"
        assert not await run_pair(b, store=store, out=out)
    return store.get(IDENTITY)


async def test_pair_storage_unknown_first_pairing_keeps_the_new_key():
    store, out = MemoryStore(), []
    new = await pair_storage_unknown(store, out)
    assert new is not None and kept(store, new) and out[-1] == UNKNOWN_FIRST


async def test_pair_storage_unknown_keeps_the_new_key_and_every_earlier_key():
    store, out = MemoryStore(), []
    store.set(IDENTITY, OLD_KEY)
    store.set_earlier(IDENTITY, key_id(EARLIER_KEY), EARLIER_KEY)
    new = await pair_storage_unknown(store, out)
    assert new not in (None, OLD_KEY, EARLIER_KEY) and kept(store, new, OLD_KEY, EARLIER_KEY)
    assert out[-1] == UNKNOWN_KEPT and not any("配对失败" in line for line in out)


async def test_two_unknown_saves_in_a_row_keep_three_keys():
    store, out = MemoryStore(), []
    store.set(IDENTITY, OLD_KEY)
    first = await pair_storage_unknown(store, out)
    second = await pair_storage_unknown(store, out)
    assert len({OLD_KEY, first, second}) == 3 and kept(store, second, OLD_KEY, first) and out[-1] == UNKNOWN_KEPT


async def test_pair_ok_after_unknown_saves_keeps_only_the_new_key():
    store = MemoryStore()
    store.set(IDENTITY, OLD_KEY)
    await pair_storage_unknown(store, [])
    async with Board() as b:
        assert await run_pair(b, store=store)
    assert kept(store, bytes.fromhex(b.dev.paired["psk"]))


@pytest.mark.parametrize("reason", ["storage error", "nvs full"])
async def test_pair_refused_restores_exactly_the_items_before_the_run(reason):
    store, out = MemoryStore(), []
    store.set(IDENTITY, OLD_KEY)
    store.set_earlier(IDENTITY, key_id(EARLIER_KEY), EARLIER_KEY)
    before = dict(store.items)
    async with Board() as b:
        b.dev.refuse_pair, b.dev.refuse_reason = True, reason
        assert not await run_pair(b, store=store, out=out)
    assert store.items == before and out[-1] == f"配对失败：设备拒绝了配对（{reason}）。"


async def test_new_key_id_never_equals_a_kept_key_id(monkeypatch):
    from claude_voice import pairing as pairing_mod

    store = MemoryStore()
    store.set(IDENTITY, OLD_KEY)
    store.set_earlier(IDENTITY, key_id(EARLIER_KEY), EARLIER_KEY)
    fresh = bytes(range(100, 132))
    draws = iter([OLD_KEY, EARLIER_KEY, fresh])
    monkeypatch.setattr(pairing_mod.secrets, "token_bytes", lambda n: next(draws))
    async with Board() as b:
        assert await run_pair(b, store=store)
    assert b.dev.paired["key_id"] == key_id(fresh) and kept(store, fresh)


async def test_pair_refused_restores_previous_key():
    store = MemoryStore()
    store.set(IDENTITY, b"\x01" * 32)
    out = []
    async with Board() as b:
        b.dev.refuse_pair = True
        assert not await run_pair(b, store=store, out=out)
    assert store.get(IDENTITY) == b"\x01" * 32
    assert "nvs full" in out[-1]


async def test_pair_refused_without_previous_key_leaves_no_item():
    store = MemoryStore()
    async with Board() as b:
        b.dev.refuse_pair = True
        assert not await run_pair(b, store=store)
    assert store.items == {}


async def test_pair_reports_auth_failure():
    out = []
    async with Board(key_override=bytes(32)) as b:
        assert not await run_pair(b, out=out)
    assert any("密钥不符" in line for line in out) and "重新配对" in out[-1]


async def test_pair_refuses_v1_board_before_sending_anything():
    store = MemoryStore()
    async with Board(proto=1) as b:
        with pytest.raises(PairError, match="协议版本"):
            await run_pair(b, store=store)
    assert T.PAIR not in b.dev.received and store.items == {}


async def test_pair_stops_when_the_relay_port_is_taken():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        s.listen()
        port = s.getsockname()[1]
        async with Board() as b:
            with pytest.raises(PairError, match=str(port)):
                await run_pair(b, c=cfg(wifi_port=port))
    assert T.PAIR not in b.dev.received


async def test_forget_erases_board_and_keychain_item():
    store, out = MemoryStore(), []
    store.set(IDENTITY, b"\x02" * 32)
    async with Board() as b:
        assert await forget(cfg(), transport=b.host, store=store, serial=FAKE_SERIAL, out=out.append)
    assert b.dev.forgets == 1 and store.items == {}


@pytest.mark.parametrize("before", ["connected", "off"])
async def test_forget_confirms_only_on_forget_result(before):
    out = []
    async with Board() as b:
        b.dev.wifi, b.dev.relay = before, ("connected" if before == "connected" else "none")
        assert await forget(cfg(), transport=b.host, store=MemoryStore(), serial=FAKE_SERIAL, out=out.append)
    assert b.dev.forgets == 1 and out[0] == "设备已清除 Wi-Fi 设置。"


@pytest.mark.parametrize("before", ["connected", "off"])
@pytest.mark.parametrize("reply", [None, b'{"ok":false,"error":"nvs busy"}'], ids=["no-result", "refused"])
async def test_forget_without_ok_result_is_unconfirmed(before, reply):
    out = []
    async with Board() as b:
        b.dev.wifi, b.dev.relay = before, ("connected" if before == "connected" else "none")
        b.dev.forget_erases, b.dev.forget_reply = False, reply
        assert not await forget(cfg(), transport=b.host, store=MemoryStore(), serial=FAKE_SERIAL, out=out.append)
    assert b.dev.forgets == 1 and "设备已清除" not in out[0]
    if reply is not None:
        assert "nvs busy" in out[0]


def test_private_ipv4_lists_only_lan_addresses_of_en_interfaces():
    text = (
        "lo0: flags=8049<UP,LOOPBACK>\n\tinet 127.0.0.1 netmask 0xff000000\n"
        "en0: flags=8863<UP>\n\tinet 192.168.7.20 netmask 0xffffff00 broadcast 192.168.7.255\n"
        "en1: flags=8863<UP>\n\tinet 10.1.2.3 netmask 0xffffff00 broadcast 10.1.2.255\n"
        "\tinet 169.254.1.1 netmask 0xffff0000\n"
        "utun3: flags=8051<UP>\n\tinet 100.64.0.5 --> 100.64.0.5 netmask 0xffffffff\n"
        "bridge100: flags=8863<UP>\n\tinet 192.168.64.1 netmask 0xffffff00\n"
        "en5: flags=8863<UP>\n\tinet 8.8.8.8 netmask 0xffffff00\n"
    )
    assert private_ipv4(Config(), text) == ["192.168.7.20", "10.1.2.3"]


def test_keychain_key_goes_through_stdin_not_argv(monkeypatch):
    calls = []

    def fake_run(args, **kw):
        calls.append((args, kw.get("input")))
        return subprocess.CompletedProcess(args, 0, "", "")

    monkeypatch.setattr(keychain_mod.subprocess, "run", fake_run)
    key = bytes(range(32))
    KeychainStore("claude-voice-test", 5.0).set(IDENTITY, key)
    (args, stdin), = calls
    assert args == [keychain_mod.SECURITY, "-i"]
    assert key.hex() not in " ".join(args) and key.hex() in stdin and "-s claude-voice-test " in stdin


@pytest.mark.parametrize("bad", ["clawd-AABBCCDDEEFF\ndelete-keychain x", "clawd-AABB", "other-AABBCCDDEEFF"])
def test_keychain_rejects_malformed_identity(monkeypatch, bad):
    monkeypatch.setattr(keychain_mod.subprocess, "run", lambda *a, **k: pytest.fail("security ran"))
    with pytest.raises(ValueError):
        KeychainStore("claude-voice-test", 5.0).set(bad, bytes(32))


def test_keychain_rejects_odd_service_name():
    with pytest.raises(ValueError):
        KeychainStore("claude voice -a x", 5.0)


@pytest.mark.skipif(os.environ.get("CLAUDE_VOICE_KEYCHAIN_TEST") != "1", reason="set CLAUDE_VOICE_KEYCHAIN_TEST=1")
def test_real_keychain_with_test_service():
    store = KeychainStore(f"claude-voice-test-{uuid.uuid4().hex[:8]}", 30.0)
    key = os.urandom(32)
    try:
        assert store.get(IDENTITY) is None
        store.set(IDENTITY, key)
        store.set(IDENTITY, key)
        assert store.get(IDENTITY) == key and store.find_any() == IDENTITY
    finally:
        assert store.delete(IDENTITY)
    assert store.get(IDENTITY) is None and not store.delete(IDENTITY)



async def test_relay_starts_wifi_only_when_paired():
    from claude_voice import cli
    from claude_voice.link import Link

    c = cfg()
    link = Link(c, lambda: None, None)
    store = MemoryStore()
    assert await cli.start_wifi(c, link, store) is None
    store.set(IDENTITY, KEY32)
    server = await cli.start_wifi(c, link, store)
    try:
        assert server is not None and server.port > 0
        t = await tls_connect(server.port, wire_identity(IDENTITY, key_id(KEY32)), KEY32)
        await t.close()
    finally:
        await server.close()
    assert await cli.start_wifi(dataclasses.replace(c, wifi_enabled=False), link, store) is None


def test_ask_wifi_validates_input(monkeypatch):
    from claude_voice import cli

    monkeypatch.setattr(cli, "current_ssid", lambda c: "办公室")
    monkeypatch.setattr("builtins.input", lambda prompt: "")
    monkeypatch.setattr(cli.getpass, "getpass", lambda prompt: PASSWORD)
    assert cli._ask_wifi(cfg()) == ("办公室", PASSWORD)
    monkeypatch.setattr(cli.getpass, "getpass", lambda prompt: "short")
    with pytest.raises(PairError):
        cli._ask_wifi(cfg())
    monkeypatch.setattr(cli, "current_ssid", lambda c: None)
    with pytest.raises(PairError):
        cli._ask_wifi(cfg())


HOTSPOT_PASSWORD = "hot-Sp0t-pass-Qx7"
CAFE_PASSWORD = "cafe-Pa55-Wq2!xyz"


async def test_pair_sends_every_network_with_lan_only_on_the_main_one(caplog, capsys):
    caplog.set_level(logging.DEBUG)
    store, out = MemoryStore(), []
    extra = [("我的手机", HOTSPOT_PASSWORD), ("cafe", CAFE_PASSWORD), ("open-net", "")]
    async with Board() as b:
        b.dev.leaky_log = True
        ok = await pair(
            cfg(), transport=b.host, store=store, serial=FAKE_SERIAL, ssid="家里的网", password=PASSWORD,
            out=out.append, extra_networks=extra,
        )
    assert ok, out
    sent = b.dev.paired
    assert sent["ssid"] == "家里的网" and sent["password"] == PASSWORD
    assert sent["networks"] == [
        {"ssid": "家里的网", "password": PASSWORD, "lan": True},
        {"ssid": "我的手机", "password": HOTSPOT_PASSWORD, "lan": False},
        {"ssid": "cafe", "password": CAFE_PASSWORD, "lan": False},
        {"ssid": "open-net", "password": "", "lan": False},
    ]
    captured = capsys.readouterr()
    everything = "\n".join([caplog.text, captured.out, captured.err, *out])
    assert "careless" in caplog.text and "我的手机" in everything
    for secret in (PASSWORD, HOTSPOT_PASSWORD, CAFE_PASSWORD, sent["psk"]):
        assert secret not in everything


async def test_pair_with_only_the_main_network_still_sends_networks():
    async with Board() as b:
        assert await run_pair(b)
    assert b.dev.paired["networks"] == [{"ssid": "家里的网", "password": PASSWORD, "lan": True}]


async def test_pair_refuses_more_than_three_extra_networks():
    async with Board() as b:
        with pytest.raises(ValueError):
            await pair(
                cfg(), transport=b.host, store=MemoryStore(), serial=FAKE_SERIAL, ssid="a", password=PASSWORD,
                out=[].append, extra_networks=[(f"n{i}", PASSWORD) for i in range(4)],
            )
    assert b.dev.paired is None


def _worst(char_ssid, char_password):
    return [(char_ssid * 32, char_password * 63) for _ in range(3)]


async def test_worst_case_plain_pair_fits_the_device_buffer():
    from claude_voice.protocol import PAIR_PAYLOAD_MAX

    c = cfg(pair_relay_names=["n" * 63 + ".local"], pair_relay_addrs=["192.168.100.200", "10.100.200.250"])
    async with Board() as b:
        assert await pair(
            c, transport=b.host, store=MemoryStore(), serial=FAKE_SERIAL, ssid="s" * 32, password="p" * 63,
            out=[].append, extra_networks=_worst("t", "q"),
        )
    payload = json.dumps(b.dev.paired, ensure_ascii=False, separators=(",", ":")).encode()
    assert len(payload) <= PAIR_PAYLOAD_MAX and len(b.dev.paired["networks"]) == 4


async def test_oversize_pair_is_refused_before_the_board_or_keychain_is_touched():
    store = MemoryStore()
    store.set(IDENTITY, OLD_KEY)
    async with Board() as b:
        with pytest.raises(PairError) as e:
            await pair(
                cfg(), transport=b.host, store=store, serial=FAKE_SERIAL, ssid='"' * 32, password='"' * 63,
                out=[].append, extra_networks=_worst('"', '"'),
            )
    assert "超过了设备能接收的 1023 字节" in str(e.value) and '"' * 8 not in str(e.value)
    assert b.dev.paired is None and b.dev.host_hellos == 0 and store.get(IDENTITY) == OLD_KEY


def _prompts(monkeypatch, names, passwords):
    from claude_voice import cli

    asked = {"input": [], "getpass": []}
    names, passwords = iter(names), iter(passwords)
    monkeypatch.setattr("builtins.input", lambda prompt: (asked["input"].append(prompt), next(names))[1])
    monkeypatch.setattr(cli.getpass, "getpass", lambda prompt: (asked["getpass"].append(prompt), next(passwords))[1])
    return cli, asked


def test_more_wifi_stops_at_an_empty_name(monkeypatch):
    cli, asked = _prompts(monkeypatch, ["我的手机", ""], [HOTSPOT_PASSWORD])
    assert cli._ask_more_wifi("家里的网") == [("我的手机", HOTSPOT_PASSWORD)]
    assert asked["input"] == [
        "其他 Wi-Fi 名称（还可添加 3 个，例如手机热点；直接按回车结束）：",
        "其他 Wi-Fi 名称（还可添加 2 个，例如手机热点；直接按回车结束）：",
    ]
    assert asked["getpass"] == ["「我的手机」的密码（输入时不显示）："]


def test_more_wifi_asks_for_at_most_three(monkeypatch):
    cli, asked = _prompts(monkeypatch, ["a", "b", "c", "d"], [PASSWORD, "", PASSWORD])
    assert cli._ask_more_wifi("home") == [("a", PASSWORD), ("b", ""), ("c", PASSWORD)]
    assert len(asked["input"]) == 3 and len(asked["getpass"]) == 3


@pytest.mark.parametrize(
    "names,passwords,message",
    [
        (["home"], [], "已经输入过了"),
        (["a", "a"], [PASSWORD], "已经输入过了"),
        (["x" * 33], [], "不能超过 32 字节"),
        (["a"], ["short"], "8 到 63 个字符"),
        (["a"], ["p" * 64], "8 到 63 个字符"),
    ],
    ids=["same-as-main", "repeated", "long-name", "short-password", "long-password"],
)
def test_more_wifi_validates_like_the_main_network(monkeypatch, names, passwords, message):
    cli, _ = _prompts(monkeypatch, names, passwords)
    with pytest.raises(PairError, match=message):
        cli._ask_more_wifi("home")


async def test_pair_command_passes_the_extra_networks(monkeypatch):
    from claude_voice import cli

    seen = {}

    async def fake_pair(c, **kw):
        seen.update(kw)
        return True

    async def fake_usb(c, port):
        return "transport"

    monkeypatch.setattr(cli, "_board", lambda c: ("port", FAKE_SERIAL))
    monkeypatch.setattr(cli, "_ask_wifi", lambda c: ("家里的网", PASSWORD))
    monkeypatch.setattr(cli, "_ask_more_wifi", lambda main: [("我的手机", HOTSPOT_PASSWORD)] if main == "家里的网" else [])
    monkeypatch.setattr(cli, "_usb", fake_usb)
    monkeypatch.setattr(cli, "keychain", lambda c: MemoryStore())
    monkeypatch.setattr(cli, "pair", fake_pair)
    assert await cli.pair_command(cfg())
    assert seen["extra_networks"] == [("我的手机", HOTSPOT_PASSWORD)] and seen["ssid"] == "家里的网"


async def test_pair_command_refuses_an_oversize_pair_before_opening_the_port(monkeypatch):
    from claude_voice import cli

    opened = []

    async def fake_usb(c, port):
        opened.append(port)
        return "transport"

    monkeypatch.setattr(cli, "_board", lambda c: ("port", FAKE_SERIAL))
    monkeypatch.setattr(cli, "_ask_wifi", lambda c: ('"' * 32, '"' * 63))
    monkeypatch.setattr(cli, "_ask_more_wifi", lambda main: _worst('"', '"'))
    monkeypatch.setattr(cli, "_usb", fake_usb)
    monkeypatch.setattr(cli, "pair", lambda *a, **kw: pytest.fail("pair ran"))
    with pytest.raises(PairError, match="1023 字节"):
        await cli.pair_command(cfg())
    assert opened == []
