"""PSK revocation: the running relay drops deleted keys, and `claude-voice revoke` deletes one without USB."""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Sequence
from typing import Protocol

from .config import Config
from .keychain import KeychainError, KeychainValueError, PskStore, psk_identity
from .pairing import Out, PairError
from .wifi import PSK_BYTES, Keyring, WarnLimiter

log = logging.getLogger("claude_voice.revoke")
_MISSING = object()


class Sweepable(Protocol):
    def close_sessions(self) -> Awaitable[None]: ...


class KeyReloader:
    """Re-reads the PSK of every identity in `ring`; a definite "not found" or any other value drops one."""

    def __init__(self, cfg: Config, store: PskStore, ring: Keyring, servers: Sequence[Sweepable]) -> None:
        self._cfg = cfg
        self._store = store
        self._ring = ring
        self._servers = list(servers)
        self._warn = WarnLimiter(log, cfg.wifi_warn_interval_s)

    async def run(self) -> None:
        while True:
            await asyncio.sleep(self._cfg.key_reload_interval_s)
            try:
                await self.reload()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                self._warn.warn("PSK reload failed: %r", e)

    def _read(self, identities: list[str]) -> dict[str, object]:
        found: dict[str, object] = {}
        for identity in identities:
            try:
                key = self._store.get(identity)
            except Exception as e:  # unknown, never treated as deleted
                found[identity] = e
                continue
            found[identity] = _MISSING if key is None else key
        return found

    async def reload(self) -> None:
        keys = self._ring.keys
        found = await asyncio.to_thread(self._read, list(keys))
        dropped = []
        for identity, value in found.items():
            if isinstance(value, KeychainValueError) or (isinstance(value, bytes) and len(value) != PSK_BYTES):
                log.warning("Keychain PSK of %s is not a %d-byte hex key; dropping it", identity, PSK_BYTES)
                keys.pop(identity, None)
                dropped.append(identity)
            elif isinstance(value, Exception):
                self._warn.warn("cannot re-read the PSK of %s, keeping it: %s", identity, type(value).__name__)
            elif value is _MISSING:
                keys.pop(identity, None)
                dropped.append(identity)
            elif value != keys.get(identity):
                keys[identity] = value
                dropped.append(identity)
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
    """Delete one board's PSK from the Keychain, then name any PSK that remains."""
    identity = revoke_identity(cfg, store)
    deleted = store.delete(identity)
    try:
        still = store.get(identity) is not None
    except KeychainValueError:
        still = True
    if still:
        out(f"删除后本机钥匙串中仍能读到设备 {identity} 的配对密钥，撤销没有完成。")
        return False
    if deleted:
        out(f"已删除本机钥匙串中设备 {identity} 的配对密钥。")
        out(
            f"正在运行的中继会在 {cfg.key_reload_interval_s:g} 秒内断开这台设备，之后不再接受它连接。"
            "中继如果是在这次更新之前启动的，要重启后才会断开。"
        )
        out("拿到设备的人仍能读出其中保存的 Wi-Fi 密码，请修改这些 Wi-Fi 的密码。")
    else:
        out(f"本机钥匙串中没有设备 {identity} 的配对密钥。")
    try:
        left = _stored(store)
    except KeychainError:
        out("无法检查本机钥匙串中是否还有其他设备的配对密钥。")
        return deleted
    if left:
        out(f"本机钥匙串中还有这些设备的配对密钥：{'、'.join(left)}。")
    return deleted

