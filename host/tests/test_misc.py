import contextlib
import dataclasses
from types import SimpleNamespace

import pytest

from claude_voice import link as link_mod
from claude_voice.agent import ClaudeAgent
from claude_voice.config import DEFAULT_SYSTEM_PROMPT, Config, load
from claude_voice.stt import SttError, WhisperStt, normalise


def test_config_defaults_and_toml_override(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text('voice = "Meijia"\nrate = 180\ntools = ["WebSearch"]\n', encoding="utf-8")
    cfg = load(p)
    assert (cfg.voice, cfg.rate, cfg.tools) == ("Meijia", 180, ["WebSearch"])
    assert cfg.system_prompt == DEFAULT_SYSTEM_PROMPT and cfg.model == "claude-opus-5-5" and cfg.effort == "low"


def test_config_rejects_unknown_key(tmp_path):
    p = tmp_path / "c.toml"
    p.write_text("volum = 3\n")
    with pytest.raises(ValueError, match="volum"):
        load(p)


async def test_unset_whisper_model_has_clear_error():
    with pytest.raises(SttError, match="whisper_model is not set"):
        await WhisperStt(Config()).start()


async def test_missing_whisper_model_has_clear_error(tmp_path):
    cfg = dataclasses.replace(Config(), whisper_model=str(tmp_path / "nope.bin"))
    with pytest.raises(SttError, match="whisper_model"):
        await WhisperStt(cfg).start()


def test_whisper_marker_filtering():
    assert normalise(" [BLANK_AUDIO] ") == "" and normalise("（音乐）") == "" and normalise("你好 ") == "你好"


def test_agent_options_isolate_session():
    o = ClaudeAgent(Config()).options()
    assert o.tools == ["WebSearch", "WebFetch"] and o.allowed_tools == ["WebSearch", "WebFetch"]
    assert o.strict_mcp_config and o.setting_sources == [] and o.model == "claude-opus-5-5" and o.effort == "low"
    assert {"safe-mode", "restricted", "no-session-persistence"} <= set(o.extra_args)


def test_find_port_matches_serial_not_name(monkeypatch):
    ports = [
        SimpleNamespace(device="/dev/cu.usbmodem1", vid=0x303A, pid=0x1001, serial_number="AA:BB"),
        SimpleNamespace(device="/dev/cu.usbmodem9", vid=0x303A, pid=0x1001, serial_number="aa:bb:cc:dd:ee:ff"),
        SimpleNamespace(device="/dev/cu.other", vid=0x1234, pid=0x1001, serial_number="AA:BB:CC:DD:EE:FF"),
    ]
    pinned = dataclasses.replace(Config(), device_serial="AA:BB:CC:DD:EE:FF")
    monkeypatch.setattr("serial.tools.list_ports.comports", lambda: ports)
    assert link_mod.find_port(pinned) == "/dev/cu.usbmodem9"
    monkeypatch.setattr("serial.tools.list_ports.comports", lambda: ports[:1])
    assert link_mod.find_port(pinned) is None


def test_find_port_without_pinned_serial_needs_exactly_one_board(monkeypatch):
    one = SimpleNamespace(device="/dev/cu.usbmodem1", vid=0x303A, pid=0x1001, serial_number="AA:BB:CC:DD:EE:01")
    two = SimpleNamespace(device="/dev/cu.usbmodem2", vid=0x303A, pid=0x1001, serial_number="AA:BB:CC:DD:EE:02")
    other = SimpleNamespace(device="/dev/cu.other", vid=0x1234, pid=0x5678, serial_number="X")
    monkeypatch.setattr("serial.tools.list_ports.comports", lambda: [one, other])
    assert link_mod.find_port(Config()) == "/dev/cu.usbmodem1"
    monkeypatch.setattr("serial.tools.list_ports.comports", lambda: [one, two])
    assert link_mod.find_port(Config()) is None
    monkeypatch.setattr("serial.tools.list_ports.comports", lambda: [other])
    assert link_mod.find_port(Config()) is None


def test_defaults_hold_no_machine_specific_values():
    cfg = Config()
    assert cfg.device_serial == "" and cfg.whisper_model == ""
    assert cfg.pair_relay_names == [] and cfg.pair_relay_addrs == []


async def test_serial_transport_deasserts_dtr_rts_around_open(monkeypatch):
    calls = []

    class FakeSerial:
        def __init__(self):
            self._dtr = self._rts = None
            self.exclusive = None

        @property
        def dtr(self):
            return self._dtr

        @dtr.setter
        def dtr(self, v):
            self._dtr = v
            calls.append(("dtr", v))

        @property
        def rts(self):
            return self._rts

        @rts.setter
        def rts(self, v):
            self._rts = v
            calls.append(("rts", v))

        def open(self):
            calls.append(("open", self.dtr, self.rts, self.exclusive))

        def read(self, n):
            import time

            time.sleep(0.01)
            return b""

        def close(self):
            pass

    monkeypatch.setattr("serial.Serial", FakeSerial)
    t = link_mod.SerialTransport("/dev/cu.fake")
    assert calls == [("dtr", False), ("rts", False), ("open", False, False, True), ("dtr", False), ("rts", False)]
    await t.close()


def test_relay_log_rotates_at_the_configured_size(tmp_path):
    import logging

    from claude_voice import cli

    root = logging.getLogger()
    before = list(root.handlers)
    c = dataclasses.replace(Config(), log_dir=str(tmp_path), log_max_bytes=4096, log_backup_count=2)
    try:
        cli.setup_logging(c, console=False)
        for i in range(400):
            logging.getLogger("claude_voice.test").info("line %04d %s", i, "x" * 60)
    finally:
        for h in root.handlers[len(before):]:
            root.removeHandler(h)
            h.close()
    names = sorted(p.name for p in tmp_path.iterdir())
    assert names == ["relay.log", "relay.log.1", "relay.log.2"]
    assert all(p.stat().st_size <= 4096 for p in tmp_path.iterdir())


@pytest.mark.parametrize(
    "soft,hard,want",
    [(256, "inf", 4096), (256, 1024, 1024), (8192, "inf", None), ("inf", "inf", None)],
)
def test_open_files_limit_is_raised_within_the_hard_limit(monkeypatch, soft, hard, want):
    import resource

    from claude_voice import cli

    inf = resource.RLIM_INFINITY
    soft, hard = (inf if v == "inf" else v for v in (soft, hard))
    calls = []
    monkeypatch.setattr(cli.resource, "getrlimit", lambda which: (soft, hard))
    monkeypatch.setattr(cli.resource, "setrlimit", lambda which, lim: calls.append((which, lim)))
    cli.raise_open_files_limit(4096)
    assert calls == ([] if want is None else [(resource.RLIMIT_NOFILE, (want, hard))])


def test_open_files_limit_failure_is_not_fatal(monkeypatch):
    from claude_voice import cli

    monkeypatch.setattr(cli.resource, "getrlimit", lambda which: (256, cli.resource.RLIM_INFINITY))

    def refuse(which, lim):
        raise ValueError("not allowed")

    monkeypatch.setattr(cli.resource, "setrlimit", refuse)
    cli.raise_open_files_limit(4096)


@pytest.mark.parametrize("command", ["run", "pair"])
def test_main_raises_open_files_limit_at_startup(monkeypatch, tmp_path, command):
    import logging
    import sys

    from claude_voice import cli

    conf = tmp_path / "c.toml"
    conf.write_text(f'log_dir = "{tmp_path}"\nopen_files_limit = 5000\n', encoding="utf-8")
    raised = []
    monkeypatch.setattr(sys, "argv", ["claude-voice", "--config", str(conf), command])
    monkeypatch.setattr(cli, "raise_open_files_limit", raised.append)

    async def stub(cfg):
        return True

    monkeypatch.setattr(cli, "run", stub)
    monkeypatch.setattr(cli, "pair_command", stub)
    root = logging.getLogger()
    before = list(root.handlers)
    try:
        with pytest.raises(SystemExit) if command == "pair" else contextlib.nullcontext():
            cli.main()
    finally:
        for h in root.handlers[len(before):]:
            root.removeHandler(h)
            h.close()
    assert raised == [5000]


def test_config_rejects_volume_out_of_range(tmp_path):
    for bad in (-1, 101, 300):
        p = tmp_path / "c.toml"
        p.write_text(f"volume = {bad}\n", encoding="utf-8")
        with pytest.raises(ValueError, match="volume"):
            load(p)
    p.write_text("volume = 100\n", encoding="utf-8")
    assert load(p).volume == 100
