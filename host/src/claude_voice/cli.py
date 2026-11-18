"""Entry point: `uv run claude-voice [--config PATH] [run|pair|forget]`."""

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
from .keychain import KeychainError, KeychainStore, PskStore, psk_identity
from .link import Link, Opener, SerialTransport, list_boards, serial_opener
from .pairing import PairError, current_ssid, forget, pair
from .relay import Relay
from .stt import WhisperStt
from .tts import SayTts
from .wifi import WifiServer, load_keys


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


async def start_wifi(cfg: config_mod.Config, link: Link, store: PskStore) -> WifiServer | None:
    if not cfg.wifi_enabled:
        return None
    try:
        keys = load_keys(cfg, store)
    except (KeychainError, ValueError) as e:
        logging.error("Wi-Fi link off: %s", e)
        return None
    if not keys:
        logging.info("Wi-Fi link off: no paired board (run `claude-voice pair`)")
        return None
    server = WifiServer(cfg, keys, link.serve_wifi)
    try:
        await server.start()
    except OSError as e:
        logging.error("Wi-Fi link off: cannot listen on port %d: %s", cfg.wifi_port, e.strerror)
        return None
    logging.info("Wi-Fi link listening on %s:%d", cfg.wifi_host, server.port)
    return server


async def run(cfg: config_mod.Config, opener: Opener | None = None) -> None:
    stt = WhisperStt(cfg)
    agent = ClaudeAgent(cfg)
    relay = Relay(cfg, stt, agent, SayTts(cfg))
    link = Link(cfg, opener or serial_opener(cfg), relay)
    relay.attach(link)
    tasks: list[asyncio.Task] = []
    server: WifiServer | None = None
    try:
        logging.info("starting whisper-server (first start may take a while)")
        await stt.start()
        agent.start()
        server = await start_wifi(cfg, link, keychain(cfg))
        tasks = [asyncio.create_task(link.run()), asyncio.create_task(relay.idle_reset_loop())]
        await asyncio.gather(*tasks)
    finally:
        for t in tasks:
            t.cancel()
        if server is not None:
            await server.close()
        with contextlib.suppress(Exception):
            await agent.close()
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


def _ask_wifi(cfg: config_mod.Config) -> tuple[str, str]:
    default = current_ssid(cfg)
    prompt = f"Wi-Fi 名称 [{default}]：" if default else "Wi-Fi 名称："
    ssid = input(prompt).strip() or (default or "")
    if not ssid or len(ssid.encode()) > 32:
        raise PairError("Wi-Fi 名称不能为空，且不能超过 32 字节。")
    password = getpass.getpass("Wi-Fi 密码（输入时不显示）：")
    if len(password.encode()) > 63 or 0 < len(password) < 8:
        raise PairError("Wi-Fi 密码应为 8 到 63 个字符；没有密码的网络直接按回车。")
    return ssid, password


async def _usb(cfg: config_mod.Config, port: str) -> SerialTransport:
    try:
        return SerialTransport(port)
    except Exception:
        raise PairError("无法打开设备的串口。如果 claude-voice 正在运行，请先停止它。") from None


async def pair_command(cfg: config_mod.Config) -> bool:
    port, serial = _board(cfg)
    ssid, password = _ask_wifi(cfg)
    print("正在连接设备（设备会重启，约 3 秒）……")
    transport = await _usb(cfg, port)
    return await pair(
        cfg, transport=transport, store=keychain(cfg),
        serial=serial, ssid=ssid, password=password, out=print,
    )


async def forget_command(cfg: config_mod.Config) -> bool:
    port, serial = _board(cfg)
    transport = await _usb(cfg, port)
    return await forget(cfg, transport=transport, store=keychain(cfg), serial=serial, out=print)


def main() -> None:
    ap = argparse.ArgumentParser(prog="claude-voice")
    ap.add_argument("--config", help="TOML file (default: host/config.toml if present)")
    ap.add_argument("command", nargs="?", default="run", choices=["run", "pair", "forget"])
    args = ap.parse_args()
    cfg = config_mod.load(args.config)
    setup_logging(cfg, console=args.command == "run")
    raise_open_files_limit(cfg.open_files_limit)
    if args.command == "run":
        with contextlib.suppress(KeyboardInterrupt):
            asyncio.run(run(cfg))
        return
    command = pair_command if args.command == "pair" else forget_command
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
