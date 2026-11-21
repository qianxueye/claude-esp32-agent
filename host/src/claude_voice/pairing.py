"""USB pairing: Wi-Fi credentials, PSK and relay addresses to the board (docs/protocol.md, Pairing)."""

from __future__ import annotations

import asyncio
import contextlib
import ipaddress
import json
import logging
import re
import secrets
import struct
import subprocess
import time
from collections.abc import Callable, Sequence

from .config import Config
from .keychain import KeychainError, PskStore, delete_board, key_id, psk_identity, wire_identity
from .link import Transport, TransportClosed, parse_hello
from .protocol import PAIR_NETWORKS_MAX, PAIR_PAYLOAD_MAX, PROTO_VERSION, Decoder, T, encode
from .wifi import PSK_BYTES, WifiServer

log = logging.getLogger("claude_voice.pairing")
Out = Callable[[str], None]

_PRIVATE = [ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")]
WIFI_ZH = {"off": "关闭", "connecting": "连接中", "connected": "已连接", "failed": "连接失败"}
RELAY_ZH = {
    "none": "未连接", "connecting": "连接中", "connected": "已连接",
    "auth_failed": "密钥不符", "unreachable": "连不上本机",
}


class PairError(RuntimeError):
    """A failure with a message for the user."""


def _run(args: list[str], timeout_s: float) -> str:
    try:
        return subprocess.run(args, capture_output=True, text=True, timeout=timeout_s, check=False).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def relay_names(cfg: Config) -> list[str]:
    if cfg.pair_relay_names:
        return list(cfg.pair_relay_names)
    name = _run(["/usr/sbin/scutil", "--get", "LocalHostName"], cfg.system_query_timeout_s).strip()
    return [f"{name}.local"] if name else []


def private_ipv4(cfg: Config, ifconfig: str | None = None) -> list[str]:
    """RFC 1918 addresses of the Mac's active `en*` interfaces, in interface order."""
    if cfg.pair_relay_addrs:
        return list(cfg.pair_relay_addrs)
    text = _run(["/sbin/ifconfig"], cfg.system_query_timeout_s) if ifconfig is None else ifconfig
    found: list[str] = []
    iface = ""
    for line in text.splitlines():
        head = re.match(r"^([A-Za-z0-9]+):", line)
        if head:
            iface = head.group(1)
            continue
        m = re.match(r"^\s+inet (\d+\.\d+\.\d+\.\d+)\s", line)
        if not m or not iface.startswith("en"):
            continue
        addr = ipaddress.ip_address(m.group(1))
        if any(addr in n for n in _PRIVATE) and str(addr) not in found:
            found.append(str(addr))
    return found


def current_ssid(cfg: Config) -> str | None:
    """The Mac's Wi-Fi network name if macOS reveals it without a location permission."""
    ports = _run(["/usr/sbin/networksetup", "-listallhardwareports"], cfg.system_query_timeout_s)
    m = re.search(r"Hardware Port: Wi-Fi\nDevice: (\S+)", ports)
    if not m:
        return None
    out = _run(["/usr/sbin/networksetup", "-getairportnetwork", m.group(1)], cfg.system_query_timeout_s)
    found = re.match(r"Current Wi-Fi Network: (.+)", out.strip())
    if found and found.group(1) != "<redacted>":
        return found.group(1)
    return None


def redact(text: str, secrets_: list[str]) -> str:
    """Replace every secret with `***`, longest first so a secret inside another leaves no remainder."""
    for s in sorted((s for s in secrets_ if s), key=len, reverse=True):
        text = text.replace(s, "***")
    return text


class UsbConsole:
    """One USB session: the port-open handshake, then frames in order, with pings."""

    def __init__(self, cfg: Config, t: Transport, secrets_: list[str]) -> None:
        self._cfg = cfg
        self._t = t
        self._dec = Decoder()
        self._pending: list[tuple[int, bytes]] = []
        self._secrets = secrets_
        self._pinger: asyncio.Task | None = None

    def send(self, ftype: int, payload: bytes = b"") -> None:
        self._t.write(encode(ftype, payload))

    async def _read(self, timeout: float) -> tuple[int, bytes] | None:
        deadline = time.monotonic() + timeout
        while not self._pending:
            left = deadline - time.monotonic()
            if left <= 0:
                return None
            data = await self._t.read(min(left, self._cfg.read_poll_s))
            for f in self._dec.feed(data, time.monotonic()):
                if f[0] == T.LOG:
                    log.info("[device] %s", redact(f[1].decode(errors="replace").rstrip(), self._secrets))
                elif f[0] != T.PONG:
                    self._pending.append(f)
        return self._pending.pop(0)

    async def open(self) -> dict:
        """Send HOST_HELLO until the board answers; returns its HELLO."""
        cfg = self._cfg
        hello = json.dumps({"proto": PROTO_VERSION}).encode()
        deadline = time.monotonic() + cfg.usb_open_timeout_s
        info: dict | None = None
        while info is None:
            left = deadline - time.monotonic()
            if left <= 0:
                raise PairError("设备没有响应。请重新插拔 USB 线后再试。")
            self.send(T.HOST_HELLO, hello)
            f = await self._read(min(left, cfg.usb_hello_retry_s))
            if f is not None and f[0] == T.HELLO:
                info = parse_hello(f[1])
        if info.get("proto") != PROTO_VERSION:
            raise PairError(f"设备固件的协议版本是 {info.get('proto')!r}，需要 {PROTO_VERSION}。请先更新固件。")
        if info.get("reason") == "boot":
            # the board reset when the port opened and missed the earlier HOST_HELLO
            self.send(T.HOST_HELLO, hello)
        self._pinger = asyncio.create_task(self._ping_loop())
        return info

    async def wait_for(self, ftype: int, timeout: float) -> bytes | None:
        deadline = time.monotonic() + timeout
        while (left := deadline - time.monotonic()) > 0:
            f = await self._read(left)
            if f is not None and f[0] == ftype:
                return f[1]
        return None

    async def _ping_loop(self) -> None:
        nonce = 0
        with contextlib.suppress(TransportClosed):
            while True:
                await asyncio.sleep(self._cfg.ping_interval_s)
                nonce = (nonce + 1) & 0xFFFFFFFF
                self.send(T.PING, struct.pack("<I", nonce))

    async def close(self) -> None:
        if self._pinger is not None:
            self._pinger.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._pinger
        await self._t.close()


async def _hold(t: Transport) -> None:
    """Keep a paired board's TLS session open without making it the active link."""
    with contextlib.suppress(TransportClosed):
        while True:
            await t.read(1.0)


def _status_line(st: dict) -> str:
    wifi = WIFI_ZH.get(st.get("wifi"), str(st.get("wifi")))
    relay = RELAY_ZH.get(st.get("relay"), str(st.get("relay")))
    line = f"设备 Wi-Fi：{wifi}"
    if st.get("wifi") == "connected" and st.get("ip"):
        line += f"（{st['ip']}，信号 {st.get('rssi')} dBm）"
    return line + f"；中继：{relay}"


def pair_payload(
    *, identity: str, psk_hex: str, kid: str, ssid: str, password: str, extra_networks: Sequence[tuple[str, str]],
    names: list[str], addrs: list[str], port: int,
) -> bytes:
    """PAIR as compact JSON, main network first in `networks`; PairError when the device cannot hold it."""
    networks = [{"ssid": ssid, "password": password, "lan": True}]
    networks += [{"ssid": s, "password": p, "lan": False} for s, p in extra_networks]
    body = {
        "ssid": ssid, "password": password, "psk_identity": identity, "psk": psk_hex, "key_id": kid,
        "relay_names": names, "relay_addrs": addrs, "relay_port": port, "networks": networks,
    }
    payload = json.dumps(body, ensure_ascii=False, separators=(",", ":")).encode()
    if len(payload) > PAIR_PAYLOAD_MAX:
        raise PairError(
            f"配对信息有 {len(payload)} 字节，超过了设备能接收的 {PAIR_PAYLOAD_MAX} 字节。"
            "请少加几个 Wi-Fi，或换用较短的名称和密码。"
        )
    return payload


def check_pair_size(
    cfg: Config, *, serial: str, ssid: str, password: str, extra_networks: Sequence[tuple[str, str]]
) -> None:
    """Refuse an oversize PAIR before the USB port opens (opening it resets the board)."""
    pair_payload(
        identity=psk_identity(serial), psk_hex="0" * 2 * PSK_BYTES, kid="0" * 8, ssid=ssid, password=password,
        extra_networks=extra_networks, names=relay_names(cfg), addrs=private_ipv4(cfg), port=65535,
    )


RESTORE, NEW_ONLY, KEEP_ALL = "restore", "new only", "keep all"  # what pair() leaves in the Keychain
UNKNOWN_SAVE_KEPT = (
    "设备保存配对时出错，无法确认是否已经保存。本机钥匙串保留了这次的配对密钥，之前的配对密钥也没有删除，"
    "设备用其中哪一个，本机都能接受。要重新配对，请先运行 claude-voice forget，再运行 claude-voice pair。"
)
UNKNOWN_SAVE_FIRST = (
    "设备保存配对时出错，无法确认是否已经保存。本机钥匙串保留了这次的配对密钥。"
    "要重新配对，请先运行 claude-voice forget，再运行 claude-voice pair。"
)
STORE_UNREADABLE = "设备这次开机时读不出它保存的配对，暂时不能保存新的配对。请先运行 claude-voice forget，再运行 claude-voice pair。"
REFUSALS = {
    "storage error": (
        "配对失败：设备没能保存配对，设备上原来的配对没有改变。请再运行一次 claude-voice pair；"
        "仍然失败时，先运行 claude-voice forget，再运行 claude-voice pair。"
    ),
    "identity mismatch": "配对失败：设备自身的硬件地址与本机从 USB 读到的序列号不一致（identity mismatch），这台设备现在无法配对。",
}


def _new_psk(kept: set[str]) -> bytes:
    """A fresh PSK whose key id differs from every key id still kept for the board."""
    while key_id(psk := secrets.token_bytes(PSK_BYTES)) in kept:
        pass
    return psk


async def pair(
    cfg: Config, *, transport: Transport, store: PskStore, serial: str, ssid: str, password: str, out: Out,
    extra_networks: Sequence[tuple[str, str]] = (),
) -> bool:
    """Send PAIR over USB and wait until the board reaches this Mac over Wi-Fi.
    Never deletes a key the board may still hold (docs/network.md, Keys)."""
    if len(extra_networks) > PAIR_NETWORKS_MAX - 1:
        raise ValueError(f"at most {PAIR_NETWORKS_MAX - 1} extra networks")
    identity = psk_identity(serial)
    hidden = [password, *(p for _, p in extra_networks)]
    names, addrs = relay_names(cfg), private_ipv4(cfg)
    if not addrs and not names:
        raise PairError("找不到本机的局域网地址。请先连上局域网。")
    joined = asyncio.Event()

    async def hold(t: Transport) -> None:
        joined.set()
        await _hold(t)

    ring: dict[str, bytes] = {}  # only the new key, so a join proves the board took it
    server = WifiServer(cfg, ring, hold)
    try:
        await server.start()
    except OSError:
        await transport.close()
        raise PairError(f"端口 {cfg.wifi_port} 已被占用。请先停止正在运行的 claude-voice，再配对。") from None
    console = UsbConsole(cfg, transport, hidden)
    old: bytes | None = None
    earlier: list[str] = []
    demoted = stored = False  # the old current key was added as an earlier item; the new key is current
    outcome = RESTORE
    try:
        try:
            old = store.get(identity)
            earlier = store.earlier(identity)
            psk = _new_psk(set(earlier) | ({key_id(old)} if old is not None else set()))
            kid = key_id(psk)
            hidden.append(psk.hex())
            ring[wire_identity(identity, kid)] = psk
            payload = pair_payload(
                identity=identity, psk_hex=psk.hex(), kid=kid, ssid=ssid, password=password,
                extra_networks=extra_networks, names=names, addrs=addrs, port=server.port,
            )
            hello = await console.open()
            if hello.get("store") == "error":
                out(STORE_UNREADABLE)
                return False
            if old is not None and key_id(old) not in earlier:
                store.set_earlier(identity, key_id(old), old)
                demoted = True
            stored = True  # before the write: a set that fails may still have replaced the item
            store.set(identity, psk)
            log.info("pairing with Wi-Fi %r, other networks %r", ssid, [s for s, _ in extra_networks])
            outcome = KEEP_ALL  # before the send: a send that fails may still have reached the board
            console.send(T.PAIR, payload)
            result = await console.wait_for(T.PAIR_RESULT, cfg.pair_result_timeout_s)
            if result is None:
                out("设备没有回复配对结果。正在等待设备通过 Wi-Fi 连上本机……")
        except TransportClosed:
            if outcome == RESTORE:
                out("USB 连接断开了。请检查线缆后再试。")
                return False
            result = None
            out("USB 连接断开了。正在等待设备通过 Wi-Fi 连上本机……")
        if result is None:
            with contextlib.suppress(TimeoutError):
                await asyncio.wait_for(joined.wait(), cfg.pair_relay_timeout_s)
            if joined.is_set():
                outcome = NEW_ONLY
                out("配对完成：设备已通过 Wi-Fi 连上本机。")
                return True
            log.warning("PAIR outcome unknown: no PAIR_RESULT and no Wi-Fi session; keeping every key")
            out("无法确认配对结果：设备没有回复，也没有通过 Wi-Fi 连上本机。请重新运行 claude-voice pair。")
            return False
        verdict = parse_hello(result)
        if verdict.get("ok") is not True and verdict.get("error") == "storage unknown":
            log.warning("PAIR saved with an unknown outcome; keeping every key")
            out(UNKNOWN_SAVE_KEPT if old is not None or earlier else UNKNOWN_SAVE_FIRST)
            return False
        if verdict.get("ok") is not True:
            outcome = RESTORE
            reason = redact(str(verdict.get("error", "")), hidden)
            log.warning("PAIR refused: %s", reason)
            out(REFUSALS.get(reason) or (f"配对失败：设备拒绝了配对（{reason}）。" if reason else "配对失败：设备拒绝了配对。"))
            return False
        outcome = NEW_ONLY
        out(f"设备已保存 Wi-Fi「{ssid}」。正在等待设备连上本机……")
        return await _await_relay(cfg, console, out)
    except TransportClosed:
        out("USB 连接断开了。请检查线缆后再试。")
        return False
    except asyncio.CancelledError:
        if outcome == KEEP_ALL and joined.is_set():
            outcome = NEW_ONLY
            out("配对完成：设备已通过 Wi-Fi 连上本机。")
        elif outcome == KEEP_ALL:
            out("配对已中断，无法确认结果。请重新运行 claude-voice pair。")
        raise
    finally:
        try:
            try:
                _settle_keys(store, identity, outcome, old, stored, demoted)
            except KeychainError:
                out("钥匙串中的配对密钥可能与设备不一致。请重新运行 claude-voice pair。")
                raise
        finally:
            await console.close()
            await server.close()


def _settle_keys(store: PskStore, identity: str, outcome: str, old: bytes | None, stored: bool, demoted: bool) -> None:
    """Apply a pair() outcome to the Keychain: put back what was there, keep only the new key, or change nothing."""
    if outcome == NEW_ONLY:
        try:
            for kid in store.earlier(identity):
                store.delete_earlier(identity, kid)
        except KeychainError as e:
            log.warning("paired, but earlier keys of %s were not all deleted: %s", identity, e)
    elif outcome == RESTORE:
        if stored and old is None:
            store.delete(identity)
        elif stored:
            store.set(identity, old)
        if demoted:
            store.delete_earlier(identity, key_id(old))


async def _await_relay(cfg: Config, console: UsbConsole, out: Out) -> bool:
    deadline = time.monotonic() + cfg.pair_relay_timeout_s
    last: dict = {}
    while (left := deadline - time.monotonic()) > 0:
        payload = await console.wait_for(T.WIFI_STATUS, left)
        if payload is None:
            break
        st = parse_hello(payload)
        log.info("board Wi-Fi status: %s", {k: st.get(k) for k in ("wifi", "ip", "rssi", "relay")})
        if st != last:
            out(_status_line(st))
            last = st
        if st.get("relay") == "connected":
            out("配对完成：设备已通过 Wi-Fi 连上本机。")
            return True
    hint = {
        "auth_failed": "设备与本机的密钥不一致，请重新配对。",
        "unreachable": "设备连不上本机：请确认两者在同一局域网，并在 macOS 防火墙中允许 Python 接受传入连接。",
    }.get(last.get("relay"), "")
    if last.get("wifi") == "failed":
        hint = "设备连不上这个 Wi-Fi：请检查名称和密码。"
    out(f"{int(cfg.pair_relay_timeout_s)} 秒内设备没有通过 Wi-Fi 连上本机。{hint}")
    return False


async def forget(cfg: Config, *, transport: Transport, store: PskStore, serial: str, out: Out) -> bool:
    """Erase the board's Wi-Fi settings and delete every key this Mac keeps for it, whatever the board replies."""
    identity = psk_identity(serial)
    console = UsbConsole(cfg, transport, [])
    try:
        await console.open()
        console.send(T.FORGET)
        payload = await console.wait_for(T.FORGET_RESULT, cfg.forget_timeout_s)
        verdict = parse_hello(payload) if payload is not None else {}
    except TransportClosed:
        verdict = {}
    finally:
        await console.close()
    confirmed = verdict.get("ok") is True
    done = delete_board(store, identity)
    removed = done.current or done.earlier > 0
    if confirmed:
        out("设备已清除 Wi-Fi 设置。")
    elif "error" in verdict:
        out(f"设备没能清除 Wi-Fi 设置（{verdict['error']}），设备上的 Wi-Fi 设置可能还在。")
    else:
        out("没有收到设备的确认，设备上的 Wi-Fi 设置可能还在。")
    if done.listed:
        out("已删除本机钥匙串中的配对密钥。" if removed else "本机钥匙串中没有这台设备的配对密钥。")
    else:
        out("已删除本机钥匙串中这台设备当前的配对密钥。" if done.current else "本机钥匙串中没有这台设备当前的配对密钥。")
        out("无法列出这台设备的旧配对密钥，其中可能还有没删除的；中继不会再接受它们。请再运行一次 claude-voice forget（设备要用 USB 连着）。")
    return confirmed
