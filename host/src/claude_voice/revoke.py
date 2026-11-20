"""PSK revocation: the running relay drops deleted keys, and `claude-voice revoke` deletes one without USB."""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Awaitable, Sequence
from typing import Protocol

from .config import Config
from .keychain import PskStore, psk_identity
from .pairing import Out, PairError
from .wifi import PSK_BYTES, Keyring, WarnLimiter

log = logging.getLogger("claude_voice.revoke")
_MISSING = object()


class Sweepable(Protocol):
    def close_sessions(self) -> Awaitable[None]: ...


class KeyReloader:
    """Re-reads the PSK of every identity in `ring`; only a definite "not found" or a new key drops one."""

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
            if isinstance(value, Exception):
                self._warn.warn("cannot re-read the PSK of %s, keeping it: %s", identity, type(value).__name__)
            elif value is _MISSING:
                keys.pop(identity, None)
                dropped.append(identity)
            elif not isinstance(value, bytes) or len(value) != PSK_BYTES:
                self._warn.warn("Keychain PSK of %s is not %d bytes, keeping the current one", identity, PSK_BYTES)
            elif value != keys.get(identity):
                keys[identity] = value
                dropped.append(identity)
        if not dropped:
            return
        self._ring.removed += 1
        log.warning("PSK of %s deleted or replaced in the Keychain; closing the network sessions", ", ".join(dropped))
        await asyncio.gather(*(s.close_sessions() for s in self._servers))


def revoke_identity(cfg: Config, store: PskStore) -> str:
    """The identity to delete: the one for `device_serial` when set, else the only one in the store."""
    if cfg.device_serial:
        try:
            return psk_identity(cfg.device_serial)
        except ValueError:
            raise PairError(f"config.toml 中的 device_serial 格式不对：{cfg.device_serial!r}") from None
    found = store.identities()
    if not found:
        raise PairError("本机钥匙串中没有配对密钥，没有可撤销的设备。")
    if len(found) > 1:
        raise PairError(
            f"本机钥匙串中有 {len(found)} 台设备的配对密钥：{'、'.join(found)}。"
            "请在 config.toml 中用 device_serial 指定要撤销的设备，再运行 claude-voice revoke。"
        )
    return found[0]


def revoke(cfg: Config, *, store: PskStore, out: Out) -> bool:
    """Delete one board's PSK from the Keychain, then name any PSK that remains."""
    identity = revoke_identity(cfg, store)
    deleted = store.delete(identity)
    if deleted:
        out(f"已删除本机钥匙串中设备 {identity} 的配对密钥。")
        out(
            f"正在运行的 claude-voice 会在 {cfg.key_reload_interval_s:g} 秒内断开这台设备，之后不再接受它连接；"
            "如果它是用更新前的版本启动的，要重新启动后才会断开。"
        )
        out("设备里保存的 Wi-Fi 密码仍能被读出，请修改这些 Wi-Fi 的密码。")
    else:
        out(f"本机钥匙串中没有设备 {identity} 的配对密钥。")
    left = store.identities()
    if left:
        out(f"本机钥匙串中还有这些设备的配对密钥：{'、'.join(left)}。")
    return deleted

