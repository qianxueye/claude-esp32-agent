"""Entry point: `uv run claude-voice [--config PATH] [run|pair|forget|revoke]`."""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import getpass
import logging
import logging.handlers
import resource
import sys
from pathlib import Path

from . import config as config_mod
from .agent import ClaudeAgent
from .cloudflared import Cloudflared
from .keychain import KeychainError, KeychainStore, PskStore, psk_identity
from .link import Link, Opener, SerialTransport, list_boards, serial_opener
from .pairing import PairError, check_pair_size, current_ssid, forget, pair
from .protocol import PAIR_NETWORKS_MAX
from .relay import Relay
from .revoke import KeyReloader, revoke
from .stt import WhisperStt
from .qwen_tts import QwenTts
from .tts import SayTts
from .tunnel import TunnelServer
from .wifi import Keyring, WifiServer, load_keys


def setup_logging(cfg: config_mod.Config, *, console: bool = True) -> None:
    Path(cfg.log_dir).mkdir(parents=True, exist_ok=True)
    fmt = logging.Formatter("%(asctime)s %(levelname)s %(name)s: %(message)s")
    root = logging.getLogger()
    root.setLevel(logging.INFO)
    handlers: list[logging.Handler] = [
        logging.handlers.RotatingFileHandler(
            Path(cfg.log_dir) / "relay.log", maxBytes=cfg.log_max_bytes, backupCount=cfg.log_backup_count
        )
    ]
    if console:
        handlers.append(logging.StreamHandler(sys.stdout))
    for handler in handlers:
        handler.setFormatter(fmt)
        root.addHandler(handler)


def raise_open_files_limit(target: int) -> None:
    """Raise the soft open-file limit to `target`, capped by the hard limit; never lower it."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    want = target if hard == resource.RLIM_INFINITY else min(target, hard)
    if soft != resource.RLIM_INFINITY and soft < want:
        try:
            resource.setrlimit(resource.RLIMIT_NOFILE, (want, hard))
        except (ValueError, OSError) as e:
            logging.warning("cannot raise the open-file limit from %d to %d: %s", soft, want, e)


def keychain(cfg: config_mod.Config) -> KeychainStore:
    return KeychainStore(cfg.keychain_service, cfg.keychain_timeout_s)


def wifi_keys(cfg: config_mod.Config, store: PskStore) -> dict[str, bytes]:
    """The paired board's PSK for the network links, or {} after logging why they stay off."""
    if not cfg.wifi_enabled:
        return {}
    try:
        keys = load_keys(cfg, store)
    except (KeychainError, ValueError) as e:
        logging.error("Wi-Fi link off: %s", e)
        return {}
    if not keys:
        logging.info("Wi-Fi link off: no paired board (run `claude-voice pair`)")
    return keys


async def start_wifi(
    cfg: config_mod.Config, link: Link, store: PskStore, keys: Keyring | dict[str, bytes] | None = None
) -> WifiServer | None:
    keys = wifi_keys(cfg, store) if keys is None else keys
    if not keys:
        return None
    server = WifiServer(cfg, keys, link.serve_wifi)
    try:
        await server.start()
    except OSError as e:
        logging.error("Wi-Fi link off: cannot listen on port %d: %s", cfg.wifi_port, e.strerror)
        return None
    logging.info("Wi-Fi link listening on %s:%d", cfg.wifi_host, server.port)
    return server


async def start_tunnel(
    cfg: config_mod.Config, link: Link, keys: Keyring | dict[str, bytes]
) -> TunnelServer | None:
    if not (cfg.tunnel_enabled and cfg.wifi_enabled and keys):
        return None
    server = TunnelServer(cfg, keys, lambda t: link.serve_wifi(t, "tunnel"))
    try:
        await server.start()
    except OSError as e:
        logging.error("tunnel link off: cannot listen on port %d: %s", cfg.tunnel_port, e.strerror)
        return None
    logging.info("tunnel link listening on %s:%d", server.address, server.port)
    return server


def key_reloader(
    cfg: config_mod.Config, store: PskStore, ring: Keyring, servers: list[WifiServer | TunnelServer | None]
) -> KeyReloader | None:
    """A reloader for the listeners that run; None when neither does."""
    running = [s for s in servers if s is not None]
    return KeyReloader(cfg, store, ring, running) if running else None


def start_cloudflared(cfg: config_mod.Config, link: Link, tunnel: TunnelServer | None) -> Cloudflared | None:
    """Start the Quick Tunnel supervisor when `cloudflared_bin` is set and the tunnel endpoint listens."""
    if not cfg.cloudflared_bin:
        return None
    if tunnel is None:
        logging.info("cloudflared not started: the tunnel endpoint is off")
        return None
    supervisor = Cloudflared(cfg, tunnel.port, link.set_remote)
    supervisor.start()
    logging.info("starting cloudflared for the tunnel endpoint")
    return supervisor


async def run(cfg: config_mod.Config, opener: Opener | None = None) -> None:
    stt = WhisperStt(cfg)
    agent = ClaudeAgent(cfg)
    tts = QwenTts(cfg, SayTts(cfg)) if cfg.tts_engine == "qwen3" else SayTts(cfg)
    relay = Relay(cfg, stt, agent, tts)
    link = Link(cfg, opener or serial_opener(cfg), relay)
    relay.attach(link)
    tasks: list[asyncio.Task] = []
    server: WifiServer | None = None
    tunnel: TunnelServer | None = None
    quick: Cloudflared | None = None
    try:
        logging.info("starting whisper-server (first start may take a while)")
        await stt.start()
        if isinstance(tts, QwenTts):
            await tts.start()
        agent.start()
        store = keychain(cfg)
        ring = Keyring(wifi_keys(cfg, store))
        server = await start_wifi(cfg, link, store, ring)
        tunnel = await start_tunnel(cfg, link, ring)
        quick = start_cloudflared(cfg, link, tunnel)
        tasks = [asyncio.create_task(link.run()), asyncio.create_task(relay.idle_reset_loop())]
        reloader = key_reloader(cfg, store, ring, [server, tunnel])
        if reloader is not None:
            tasks.append(asyncio.create_task(reloader.run()))
        await asyncio.gather(*tasks)
    finally:
        for t in tasks:
            t.cancel()
        if quick is not None:
            await quick.close()
        if server is not None:
            await server.close()
        if tunnel is not None:
            await tunnel.close()
        with contextlib.suppress(Exception):
            await agent.close()
        if isinstance(tts, QwenTts):
            await tts.close()
        await stt.close()


def _board(cfg: config_mod.Config) -> tuple[str, str]:
    boards = list_boards(cfg)
    if not boards:
        raise PairError("没有找到用 USB 连接到本机的设备。请用数据线连接设备后再试。")
    if len(boards) > 1:
        raise PairError("连接了多台设备。请只保留一台，或在 config.toml 中设置 device_serial。")
    port, serial = boards[0]
    serial = cfg.device_serial or serial
    try:
        psk_identity(serial)
    except ValueError:
        raise PairError(f"设备的 USB 序列号格式不对：{serial!r}") from None
    return port, serial


def _check_ssid(ssid: str) -> str:
    if not ssid or len(ssid.encode()) > 32:
        raise PairError("Wi-Fi 名称不能为空，且不能超过 32 字节。")
    return ssid


def _ask_password(prompt: str) -> str:
    password = getpass.getpass(prompt)
    if len(password.encode()) > 63 or 0 < len(password) < 8:
        raise PairError("Wi-Fi 密码应为 8 到 63 个字符；没有密码的网络直接按回车。")
    return password


def _ask_wifi(cfg: config_mod.Config) -> tuple[str, str]:
    default = current_ssid(cfg)
    prompt = f"Wi-Fi 名称 [{default}]：" if default else "Wi-Fi 名称："
    ssid = _check_ssid(input(prompt).strip() or (default or ""))
    return ssid, _ask_password("Wi-Fi 密码（输入时不显示）：")


def _ask_more_wifi(main_ssid: str) -> list[tuple[str, str]]:
    """Up to the protocol's limit of further networks, such as a phone hotspot; an empty name ends the list."""
    extra: list[tuple[str, str]] = []
    while (left := PAIR_NETWORKS_MAX - 1 - len(extra)) > 0:
        ssid = input(f"其他 Wi-Fi 名称（还可添加 {left} 个，例如手机热点；直接按回车结束）：").strip()
        if not ssid:
            break
        _check_ssid(ssid)
        if ssid == main_ssid or any(ssid == s for s, _ in extra):
            raise PairError(f"Wi-Fi「{ssid}」已经输入过了。")
        extra.append((ssid, _ask_password(f"「{ssid}」的密码（输入时不显示）：")))
    return extra


async def _usb(cfg: config_mod.Config, port: str) -> SerialTransport:
    try:
        return SerialTransport(port)
    except Exception:
        raise PairError("无法打开设备的串口。如果 claude-voice 正在运行，请先停止它。") from None


async def pair_command(cfg: config_mod.Config) -> bool:
    port, serial = _board(cfg)
    ssid, password = _ask_wifi(cfg)
    extra = _ask_more_wifi(ssid)
    check_pair_size(cfg, serial=serial, ssid=ssid, password=password, extra_networks=extra)
    print("正在连接设备（设备会重启，约 3 秒）……")
    transport = await _usb(cfg, port)
    return await pair(
        cfg, transport=transport, store=keychain(cfg),
        serial=serial, ssid=ssid, password=password, out=print, extra_networks=extra,
    )


async def forget_command(cfg: config_mod.Config) -> bool:
    port, serial = _board(cfg)
    transport = await _usb(cfg, port)
    return await forget(cfg, transport=transport, store=keychain(cfg), serial=serial, out=print)


async def revoke_command(cfg: config_mod.Config) -> bool:
    return revoke(cfg, store=keychain(cfg), out=print)


def main() -> None:
    ap = argparse.ArgumentParser(prog="claude-voice")
    ap.add_argument("--config", help="TOML file (default: host/config.toml if present)")
    ap.add_argument("command", nargs="?", default="run", choices=["run", "pair", "forget", "revoke"])
    args = ap.parse_args()
    cfg = config_mod.load(args.config)
    setup_logging(cfg, console=args.command == "run")
    raise_open_files_limit(cfg.open_files_limit)
    if args.command == "run":
        with contextlib.suppress(KeyboardInterrupt):
            asyncio.run(run(cfg))
        return
    command = {"pair": pair_command, "forget": forget_command, "revoke": revoke_command}[args.command]
    try:
        ok = asyncio.run(command(cfg))
    except PairError as e:
        print(e)
        ok = False
    except KeychainError:
        print("无法读写 macOS 钥匙串。请确认已登录并解锁钥匙串后再试。")
        ok = False
    except KeyboardInterrupt:
        print()
        ok = False
    sys.exit(0 if ok else 1)
