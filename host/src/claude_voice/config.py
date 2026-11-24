"""Relay configuration: defaults in code, overridden by an optional TOML file."""

from __future__ import annotations

import dataclasses
import math
import re
import shutil
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

HOST_DIR = Path(__file__).resolve().parents[2]

DEFAULT_SYSTEM_PROMPT = (
    "你是 Claude，住在用户桌上一块圆形小屏里的语音助手。你说的每个字都会被朗读出来，所以："
    "用简短、自然的口语回答，默认一到三句话；用户用什么语言提问就用什么语言回答；"
    "不要使用 Markdown、列表、代码块、表情符号或网址；数字和单位写成适合朗读的形式。"
    "需要最新信息时可以联网搜索，搜索前不必解释。不确定时直说。"
)


@dataclass
class Config:
    # device; an empty serial means the single connected board with this VID/PID
    device_serial: str = ""
    usb_vid: int = 0x303A
    usb_pid: int = 0x1001
    volume: int = 70  # initial value for a board with no stored volume
    ping_interval_s: float = 1.0
    reconnect_backoff_min_s: float = 0.5
    reconnect_backoff_max_s: float = 5.0
    read_poll_s: float = 0.05
    usb_hello_retry_s: float = 1.0  # HOST_HELLO repeat while the board resets after the port opens
    usb_open_timeout_s: float = 8.0  # no HELLO by then: close the port and reopen later

    # Wi-Fi link
    wifi_enabled: bool = True
    wifi_host: str = "0.0.0.0"
    wifi_port: int = 47801
    wifi_handshake_timeout_s: float = 10.0
    wifi_preauth_max: int = 8  # connections still in the TLS handshake; more are closed on accept
    # per peer IP: one stale socket from a rebooted board plus its fresh retry
    wifi_preauth_per_addr_max: int = 2
    wifi_warn_interval_s: float = 60.0  # at most one failed-handshake warning per interval
    wifi_silence_timeout_s: float = 5.0  # no valid frame from the board: the session is dead
    wifi_write_buffer_max: int = 262144  # frames are dropped while this much is unsent
    wifi_close_timeout_s: float = 2.0
    # tunnel link: WebSocket endpoint on 127.0.0.1 for a tunnel connector such as cloudflared
    tunnel_enabled: bool = True
    tunnel_port: int = 47802  # 0: an ephemeral port
    tunnel_preauth_max: int = 4  # tunnel connections from WebSocket accept to the end of the TLS handshake
    tunnel_preauth_per_addr_max: int = 2  # per CF-Connecting-IP IPv4 address or IPv6 /64
    tunnel_upgrade_timeout_s: float = 10.0
    tunnel_ws_max_message: int = 32768
    tunnel_preauth_bytes_max: int = 32768  # inbound bytes before the TLS session is authenticated
    tunnel_write_buffer_max: int = 262144  # frames are dropped while this much is unsent
    # cloudflared Quick Tunnel to the tunnel endpoint, started by the relay; "" leaves it off
    cloudflared_bin: str = ""
    cloudflared_url_timeout_s: float = 30.0  # no public hostname by then: restart cloudflared
    cloudflared_restart_min_s: float = 1.0
    cloudflared_restart_max_s: float = 60.0  # a run longer than this resets the delay to the minimum
    key_reload_interval_s: float = 10.0  # the running relay re-reads its PSKs; a deleted one stops working
    keychain_service: str = "claude-voice"
    keychain_timeout_s: float = 30.0  # `security` may wait for the user to unlock the Keychain

    # pairing
    pair_relay_names: list[str] = field(default_factory=list)  # empty: `<LocalHostName>.local`
    pair_relay_addrs: list[str] = field(default_factory=list)  # empty: current private IPv4 addresses
    pair_result_timeout_s: float = 10.0
    pair_relay_timeout_s: float = 60.0
    forget_timeout_s: float = 5.0
    system_query_timeout_s: float = 5.0  # scutil, ifconfig, networksetup

    # flow control (protocol constants)
    spk_max_outstanding: int = 64000
    spk_chunk: int = 4096
    spk_status_gap_s: float = 1.0  # no SPK_STATUS while blocked: treat the board queue as empty
    drained_slack_s: float = 1.5

    # listening
    vad_frame_ms: int = 20
    vad_min_speech_ms: int = 300
    vad_end_silence_ms: int = 800
    vad_noise_factor: float = 3.0
    vad_min_rms: float = 120.0
    vad_noise_init_frames: int = 10
    vad_noise_rise: float = 0.02
    no_speech_timeout_s: float = 8.0
    max_listen_s: float = 30.0
    min_utterance_ms: int = 300

    # timeouts
    error_hold_s: float = 2.0
    whisper_timeout_s: float = 60.0
    claude_turn_timeout_s: float = 120.0
    interrupt_drain_timeout_s: float = 5.0
    conversation_idle_reset_s: float = 600.0

    # speech to text
    whisper_server: str = "/opt/homebrew/bin/whisper-server"
    whisper_model: str = ""  # required: path to a ggml model file, set in host/config.toml
    whisper_language: str = "zh"
    whisper_prompt: str = "以下是普通话的句子。"
    whisper_threads: int = 4
    whisper_ready_timeout_s: float = 180.0

    # agent
    claude_cli: str = field(default_factory=lambda: shutil.which("claude") or "claude")
    claude_workdir: str = "~/.claude-voice-agent-ws"
    model: str = "claude-opus-5-5"
    effort: str = "low"
    tools: list[str] = field(default_factory=lambda: ["WebSearch", "WebFetch"])
    system_prompt: str = DEFAULT_SYSTEM_PROMPT

    # text to speech
    say_bin: str = "/usr/bin/say"
    voice: str = "Tingting"
    rate: int = 0  # words per minute for `say -r`; 0 keeps the voice default
    first_chunk_min_chars: int = 12  # a comma may end the first chunk once it is this long
    tts_timeout_s: float = 30.0
    tts_engine: str = "say"  # "say" or "qwen3"; qwen3 falls back to say per sentence
    qwen_url: str = ""  # speech server base URL; ignored when qwen_server_cmd is set
    # argv; --host/--port are appended; started on a free local port, or on qwen_ssh_host
    qwen_server_cmd: list[str] = field(default_factory=list)
    qwen_model: str = "mlx-community/Qwen3-TTS-12Hz-0.6B-CustomVoice-bf16"
    qwen_voice: str = "ono_anna"
    qwen_lang_code: str = "chinese"  # "chinese" or "japanese"
    qwen_voices: dict[str, str] = field(default_factory=dict)  # per lang_code voice override
    qwen_connect_timeout_s: float = 2.0
    qwen_skip_s: float = 30.0  # after a failure, use say for this long before trying the server again
    qwen_ready_timeout_s: float = 180.0
    qwen_ssh_host: str = ""  # ssh config alias of the Mac that runs qwen_server_cmd; "" runs it here
    qwen_ssh_bin: str = "/usr/bin/ssh"
    qwen_remote_python: str = "/usr/bin/python3"  # runs the stdlib guard on that Mac
    qwen_remote_port: int = 47803  # the server's port on that Mac, forwarded to a free local port
    qwen_ssh_connect_timeout_s: float = 10.0  # ssh ConnectTimeout, which also covers the key exchange
    qwen_link_timeout_s: float = 10.0  # no heartbeat for this long: the remote server is stopped
    qwen_restart_min_s: float = 5.0
    qwen_restart_max_s: float = 300.0  # a run longer than this resets the restart delay to the minimum
    afconvert_bin: str = "/usr/bin/afconvert"

    # earcons
    earcon_gain: float = 0.35

    # logging and process limits
    log_dir: str = str(HOST_DIR / "logs")
    log_max_bytes: int = 5 * 1024 * 1024  # relay.log rotates at this size
    log_backup_count: int = 3
    open_files_limit: int = 4096  # soft RLIMIT_NOFILE raised to this at startup, never above the hard limit


POSITIVE_KEYS = (
    "tunnel_preauth_max", "tunnel_preauth_per_addr_max", "tunnel_upgrade_timeout_s",
    "tunnel_ws_max_message", "tunnel_preauth_bytes_max", "tunnel_write_buffer_max",
    "cloudflared_url_timeout_s", "cloudflared_restart_min_s", "cloudflared_restart_max_s",
    "key_reload_interval_s", "qwen_ssh_connect_timeout_s", "qwen_link_timeout_s", "qwen_restart_min_s",
    "qwen_restart_max_s",
)
SSH_TIMING_KEYS = ("qwen_ssh_connect_timeout_s", "qwen_link_timeout_s", "qwen_restart_min_s", "qwen_restart_max_s")
# the remote python is pasted into the remote shell's command line, so it is limited to inert characters
REMOTE_PYTHON = re.compile(r"/[A-Za-z0-9._/+-]+")


def load(path: Path | str | None = None) -> Config:
    cfg = Config()
    p = Path(path) if path else HOST_DIR / "config.toml"
    if not p.exists():
        if path:
            raise FileNotFoundError(f"config file not found: {p}")
        return cfg
    data = tomllib.loads(p.read_text(encoding="utf-8"))
    known = {f.name for f in dataclasses.fields(Config)}
    unknown = sorted(set(data) - known)
    if unknown:
        raise ValueError(f"unknown config keys in {p}: {', '.join(unknown)}")
    cfg = dataclasses.replace(cfg, **data)
    if not 0 <= cfg.tunnel_port <= 65535:
        raise ValueError(f"tunnel_port must be 0-65535, got {cfg.tunnel_port} in {p}")
    for key in POSITIVE_KEYS:
        if not getattr(cfg, key) > 0:
            raise ValueError(f"{key} must be positive, got {getattr(cfg, key)} in {p}")
    for key in SSH_TIMING_KEYS:
        if not math.isfinite(getattr(cfg, key)):
            raise ValueError(f"{key} must be finite, got {getattr(cfg, key)} in {p}")
    if cfg.cloudflared_restart_max_s < cfg.cloudflared_restart_min_s:
        raise ValueError(f"cloudflared_restart_max_s must not be below cloudflared_restart_min_s in {p}")
    if cfg.qwen_restart_max_s < cfg.qwen_restart_min_s:
        raise ValueError(f"qwen_restart_max_s must not be below qwen_restart_min_s in {p}")
    if type(cfg.qwen_remote_port) is not int or not 1 <= cfg.qwen_remote_port <= 65535:
        raise ValueError(f"qwen_remote_port must be an integer 1-65535, got {cfg.qwen_remote_port!r} in {p}")
    if cfg.qwen_ssh_host:
        if cfg.qwen_ssh_host.startswith("-") or any(c.isspace() or not c.isprintable() for c in cfg.qwen_ssh_host):
            raise ValueError(f"qwen_ssh_host must be an ssh alias without spaces or a leading '-' in {p}")
        if not cfg.qwen_server_cmd:
            raise ValueError(f"qwen_ssh_host needs qwen_server_cmd in {p}")
    if not REMOTE_PYTHON.fullmatch(cfg.qwen_remote_python):
        raise ValueError(f"qwen_remote_python must be an absolute path of letters, digits and ._+-/ in {p}")
    if not 0 <= cfg.volume <= 100:
        raise ValueError(f"volume must be 0-100, got {cfg.volume} in {p}")
    if cfg.tts_engine not in ("say", "qwen3"):
        raise ValueError(f'tts_engine must be "say" or "qwen3", got {cfg.tts_engine!r} in {p}')
    return cfg
