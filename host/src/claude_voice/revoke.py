"""PSK revocation: the running relay drops deleted keys, and `claude-voice revoke` deletes one without USB."""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Sequence
from typing import Protocol

from .config import Config
from .keychain import KeychainError, KeychainValueError, PskStore, delete_board, psk_identity
from .pairing import Out, PairError
from .wifi import PSK_BYTES, BoardKeys, Keyring, WarnLimiter, board_keys, parse_wire_identity

log = logging.getLogger("claude_voice.revoke")


class Sweepable(Protocol):
    def close_sessions(self) -> Awaitable[None]: ...


class KeyReloader:
    """Re-reads every key of each board in `ring`; a definite "not found" or a value that is not a key drops one."""

    def __init__(self, cfg: Config, store: PskStore, ring: Keyring, servers: Sequence[Sweepable]) -> None:
        self._cfg = cfg
        self._store = store
        self._ring = ring
        self._servers = list(servers)
        self._warn, self._warn_bad = (WarnLimiter(log, cfg.wifi_warn_interval_s) for _ in range(2))

    async def run(self) -> None:
        while True:
            await asyncio.sleep(self._cfg.key_reload_interval_s)
            try:
                await self.reload()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                self._warn.warn("PSK reload failed: %r", e)

    def _read(self, boards: list[str], bad: list[str]) -> dict[str, BoardKeys | Exception]:
        found: dict[str, BoardKeys | Exception] = {}
        for board in boards:
            try:
                found[board] = board_keys(self._store, board, bad.append)
            except Exception as e:  # unknown, never treated as deleted
                found[board] = e
        return found

    async def reload(self) -> None:
        keys = self._ring.keys
        owner = {wire: parsed[0] for wire in keys if (parsed := parse_wire_identity(wire))}
        bad: list[str] = []
        found = await asyncio.to_thread(self._read, list(dict.fromkeys(owner.values())), bad)
        for name in bad:
            self._warn_bad.warn("Keychain PSK of %s is not a %d-byte hex key; dropping it", name, PSK_BYTES)
        dropped = []
        for board, fresh in found.items():
            if isinstance(fresh, Exception):
                self._warn.warn("cannot re-read the PSK of %s, keeping it: %s", board, type(fresh).__name__)
                continue
            if not fresh.listed or fresh.unread:
                self._warn.warn("cannot list or read every earlier key of %s, keeping those in use", board)
            for wire in [w for w, b in owner.items() if b == board]:
                new = fresh.keys.get(wire)
                # no valid current key drops every wire; a wire whose key was not read stays
                unknown = not fresh.listed and new is None or wire in fresh.unread
                if fresh.current is None or (new != keys.get(wire) and not unknown):
                    keys.pop(wire, None)
                    dropped.append(wire)
            for wire, key in fresh.keys.items():
                keys.setdefault(wire, key)
        if not dropped:
            return
        self._ring.removed += 1
        log.warning("PSK of %s deleted or replaced in the Keychain; closing the network sessions", ", ".join(dropped))
        await asyncio.gather(*(s.close_sessions() for s in self._servers))


ASK_SERIAL = "请在 config.toml 中用 device_serial 指定要撤销的设备，再运行 claude-voice revoke。"


def _stored(store: PskStore) -> list[str]:
    """Identities the listing shows, plus the one the relay's own lookup finds if the listing missed it."""
    found = store.identities()
    first = store.find_any()
    return found + [first] if first and first not in found else found


def revoke_identity(cfg: Config, store: PskStore) -> str:
    """The identity to delete: `device_serial`'s, else the only one both the listing and the relay's lookup find."""
    if cfg.device_serial:
        try:
            return psk_identity(cfg.device_serial)
        except ValueError:
            raise PairError(f"config.toml 中的 device_serial 格式不对：{cfg.device_serial!r}") from None
    listed, first = store.identities(), store.find_any()
    if not listed and first is None:
        raise PairError("本机钥匙串中没有配对密钥，没有可撤销的设备。")
    if len(listed) > 1:
        raise PairError(f"本机钥匙串中有 {len(listed)} 台设备的配对密钥：{'、'.join(listed)}。" + ASK_SERIAL)
    if listed != [first]:
        found = "、".join(dict.fromkeys(i for i in [*listed, first] if i))
        raise PairError(f"无法确认本机钥匙串中只有设备 {found} 的配对密钥。" + ASK_SERIAL)
    return listed[0]


def revoke(cfg: Config, *, store: PskStore, out: Out) -> bool:
    """Delete every PSK of one board from the Keychain, check by listing that none is left, then name other boards.
    Fails when the earlier keys could not be listed, after saying which part was deleted."""
    identity = revoke_identity(cfg, store)
    done = delete_board(store, identity)
    try:
        still = store.get(identity) is not None
    except KeychainValueError:
        still = True
    listed = done.listed
    if listed:
        try:
            still = still or bool(store.earlier(identity))
        except KeychainError:
            listed = False
    if still:
        out(
            f"删除后本机钥匙串中仍能读到设备 {identity} 的配对密钥，撤销没有完成。"
            f"请在「钥匙串访问」中搜索 {identity}，删除找到的条目，再运行一次 claude-voice revoke。"
        )
        return False
    deleted = done.current or (listed and done.earlier > 0)  # an unlisted run names only the current key
    if deleted:
        out(f"已删除本机钥匙串中设备 {identity} 的配对密钥。" if listed else f"已删除本机钥匙串中设备 {identity} 当前的配对密钥。")
        out(
            f"正在运行的中继会在 {cfg.key_reload_interval_s:g} 秒内断开这台设备，之后不再接受它连接。"
            "中继如果是在这次更新之前启动的，要重启后才会断开。"
        )
        out("拿到设备的人仍能读出其中保存的 Wi-Fi 密码，请修改这些 Wi-Fi 的密码。")
    else:
        out(f"本机钥匙串中没有设备 {identity} 的配对密钥。" if listed else f"本机钥匙串中没有设备 {identity} 当前的配对密钥。")
    try:
        left = [b for b in _stored(store) if b != identity]
    except KeychainError:
        out("无法检查本机钥匙串中是否还有其他设备的配对密钥。")
        left = []
    if left:
        out(f"本机钥匙串中还有这些设备的配对密钥：{'、'.join(left)}。")
    if not listed:
        out(
            f"无法列出设备 {identity} 的旧配对密钥，其中可能还有没删除的。请再运行一次 claude-voice revoke；"
            f"仍然失败时，在「钥匙串访问」中搜索 {identity}，删除找到的条目。"
        )
        return False
    return deleted
