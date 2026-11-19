import importlib.util
import re
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

SPEC = importlib.util.spec_from_file_location("devctl", Path(__file__).parents[2] / "firmware" / "tools" / "devctl.py")
devctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(devctl)

BRIGHT = b"I (5000) app: bright lvl=49 eff=49 panel=49 set=1 arms=3 rim=1 cancels=0 slides=4 stops=1 saves=1 in=2"


class StubLink:
    def __init__(self, frames=()):
        self.sent = []
        self.frames = list(frames)

    def send(self, ftype, payload=b""):
        self.sent.append((ftype, payload))

    def poll(self):
        out, self.frames = self.frames, []
        return out


class TimedLink(StubLink):
    """Delivers each (seconds after the first poll, payload) LOG frame once its time has come; the board's lines arrive over time."""

    def __init__(self, timed=()):
        super().__init__()
        self.timed = sorted(timed, key=lambda f: f[0])
        self.start = None

    def poll(self):
        now = time.monotonic()
        self.start = self.start or now
        due = [f for f in self.timed if f[0] <= now - self.start]
        self.timed = self.timed[len(due):]
        time.sleep(0.002)
        return [(devctl.T_LOG, p) for _, p in due]


def disp_args(**kw):
    base = dict(scale=None, force=None, orbit=None, fill=None, arc=None, bench=None, brt=None, brt_arc=None, seconds=0)
    base.update(kw)
    return SimpleNamespace(**base)


def test_brightness_sends_brt_set_and_prints_the_bright_line(capsys):
    link = StubLink([(devctl.T_LOG, b"I (4000) app: knob vol=70"), (devctl.T_LOG, BRIGHT)])
    devctl.cmd_brightness(link, SimpleNamespace(value=49))
    assert link.sent == [(devctl.T_DEBUG, b'{"brt_set":49}')]
    assert "bright lvl=49" in capsys.readouterr().out


def bright_line(lvl, arms=3, slides=4):
    return b"I (5000) app: bright lvl=%d eff=%d panel=%d set=1 arms=%d rim=1 cancels=0 slides=%d stops=1 saves=1 in=2" % (lvl, lvl, lvl, arms, slides)


def test_ladder_constants_match_the_firmware():
    text = (Path(__file__).parents[2] / "firmware" / "main" / "app_config.h").read_text()
    assert "#define BRT_MIN_PERCENT %d " % devctl.BRT_MIN_PERCENT in text and "#define BRT_STEPS %d " % devctl.BRT_STEPS in text
    assert [devctl.brt_snap(p) for p in (0, 20, 25, 50, 51, 99, 200)] == [20, 20, 24, 49, 52, 100, 100]


def test_brightness_skips_a_stale_line_and_prints_the_applied_one(capsys):
    link = StubLink([(devctl.T_LOG, bright_line(100)), (devctl.T_LOG, bright_line(49))])
    devctl.cmd_brightness(link, SimpleNamespace(value=50))
    out = capsys.readouterr().out
    assert link.sent == [(devctl.T_DEBUG, b'{"brt_set":50}')]
    assert "bright lvl=49" in out and "bright lvl=100" not in out


def test_brightness_fails_when_the_level_never_arrives(monkeypatch, capsys):
    monkeypatch.setattr(devctl, "BRIGHT_WAIT_S", 0.05)
    link = StubLink([(devctl.T_LOG, bright_line(100))])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_brightness(link, SimpleNamespace(value=49))
    assert e.value.code == 1 and "no 'bright lvl=49' line" in capsys.readouterr().out


@pytest.fixture
def fast(monkeypatch):
    monkeypatch.setattr(devctl, "SWIPE_FINAL_GUARD_S", 0.05)
    monkeypatch.setattr(devctl, "BRIGHT_WAIT_S", 0.3)


def test_swipecheck_exits_nonzero_on_fail_and_releases_the_pin(fast, capsys):
    link = TimedLink([(0.0, bright_line(49, arms=3)), (0.2, bright_line(49, arms=4))])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="grip"))
    assert e.value.code == 1 and "swipecheck grip FAIL" in capsys.readouterr().out
    assert link.sent[0] == (devctl.T_DEBUG, b'{"disp_force":0}') and link.sent[-1] == (devctl.T_DEBUG, b'{"disp_force":null}')


def test_swipecheck_pass_returns_normally(fast, capsys):
    link = TimedLink([(0.0, bright_line(49)), (0.2, bright_line(49))])
    devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="wipe"))
    assert "swipecheck wipe PASS" in capsys.readouterr().out


def test_brightness_fails_loudly_without_a_bright_line(monkeypatch, capsys):
    monkeypatch.setattr(devctl, "BRIGHT_WAIT_S", 0.05)
    link = StubLink([(devctl.T_LOG, b"I (4000) app: knob vol=70")])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_brightness(link, SimpleNamespace(value=49))
    assert e.value.code == 1 and "no 'bright lvl=49' line" in capsys.readouterr().out


def test_disp_brt_pins_and_releases():
    link = StubLink()
    devctl.cmd_disp(link, disp_args(brt="20"))
    devctl.cmd_disp(link, disp_args(brt="null", force="0"))
    assert link.sent == [(devctl.T_DEBUG, b'{"brt":20}'), (devctl.T_DEBUG, b'{"disp_force":0,"brt":null}')]
    assert devctl.PROTO == 3


def test_parse_lines():
    assert devctl.parse_bright(BRIGHT) == dict(lvl=49, eff=49, panel=49, set=1, arms=3, rim=1, cancels=0, slides=4, stops=1, saves=1, **{"in": 2})
    s = devctl.parse_swipe(b"I (1) ui: swipe: end cls=centre q=1 why=lateral arm=0 b0=-1 b=-1 dx=30 dy=-2 up=30 pk=30 lat=25 arm_ms=-1 r1=31 rev=0 "
                           b"gap=17 stp=12 ku=30 kl=-25 dur=200")
    assert s["cls"] == "centre" and s["why"] == "lateral" and s["arm"] == 0 and s["lat"] == 25 and s["b0"] == -1
    assert s["gap"] == 17 and s["stp"] == 12 and s["ku"] == 30 and s["kl"] == -25 and s["dur"] == 200


def test_swipe_report_verdict():
    first = devctl.parse_bright(BRIGHT)
    same = dict(first, slides=first["slides"] + 2)
    slides = [dict(cls="centre", why="lateral", arm=0, pk=30, lat=25), dict(cls="centre", why="none", arm=0, pk=40, lat=3)]
    lines, ok = devctl.swipe_report("grip", first, same, slides)
    assert ok and lines[-1] == "swipecheck grip PASS" and "largest unarmed pk 40 lat 3" in "\n".join(lines)
    armed = dict(first, arms=first["arms"] + 1)
    assert devctl.swipe_report("wipe", first, armed, slides)[1] is False
    moved = dict(first, lvl=52)
    assert devctl.swipe_report("wipe", first, moved, [])[1] is False
    assert devctl.swipe_report("free", None, None, [])[1] is False


FIRMWARE = Path(__file__).parents[2] / "firmware" / "main"
ARMED_GRIP = dict(cls="edge", why="none", arm=1, pk=60, lat=4, ku=48, kl=3)


def dbcf027_verdict(first, last):
    """The verdict before the bracket: counters and level only."""
    return last["arms"] - first["arms"] == 0 and first["lvl"] == last["lvl"]


def test_bright_keys_follow_the_firmware_line():
    fmt = re.search(r'"bright (lvl=[^"]*)"', (FIRMWARE / "main.c").read_text()).group(1)
    assert tuple(kv.split("=")[0] for kv in fmt.split()) == devctl.BRIGHT_KEYS
    fmt = re.search(r'#define VK_SWIPE_LINE_FMT "swipe: end ([^"]*)"', (FIRMWARE / "volknob.h").read_text()).group(1)
    assert [kv.split("=")[0] for kv in fmt.split()][-5:] == ["gap", "stp", "ku", "kl", "dur"]


def test_armed_line_with_equal_counters_fails_grip_and_wipe():
    first = devctl.parse_bright(BRIGHT)
    for label in ("grip", "wipe"):
        lines, ok = devctl.swipe_report(label, first, dict(first), [ARMED_GRIP])
        assert not ok and lines[-1] == "swipecheck %s FAIL" % label and "armed cls edge" in "\n".join(lines)
    # The rule before the bracket passes it: the red proof.
    assert dbcf027_verdict(first, dict(first))


def test_arm_without_a_swipe_line_fails():
    first = devctl.parse_bright(BRIGHT)
    assert devctl.swipe_report("grip", first, dict(first, arms=first["arms"] + 1, rim=first["rim"] + 1), [])[1] is False


def test_missing_baseline_or_final_fails():
    first = devctl.parse_bright(BRIGHT)
    for label in ("grip", "wipe", "free"):
        lines, ok = devctl.swipe_report(label, None, first, [])
        assert not ok and "no baseline" in lines[0]
        lines, ok = devctl.swipe_report(label, first, None, [])
        assert not ok and "no final" in lines[0]


def test_counters_backwards_fail_every_label():
    first = devctl.parse_bright(BRIGHT)
    reset = dict(first, slides=0, saves=0)
    for label in ("grip", "wipe", "free"):
        lines, ok = devctl.swipe_report(label, first, reset, [])
        assert not ok and "backwards (slides saves)" in "\n".join(lines)


def test_free_passes_with_armed_swipes():
    first = devctl.parse_bright(BRIGHT)
    last = dict(first, arms=first["arms"] + 2, lvl=64)
    lines, ok = devctl.swipe_report("free", first, last, [ARMED_GRIP, dict(ARMED_GRIP, cls="centre")])
    assert ok and lines[-1] == "swipecheck free PASS"


def test_swipecheck_final_sample_catches_a_late_arm(fast, capsys):
    """An arm after the last bright line inside the window shows in the final line; the pin is released after it."""
    link = TimedLink([(0.0, bright_line(49)), (0.03, bright_line(49)), (0.06, bright_line(49, arms=3, slides=5)), (0.25, bright_line(49, arms=4, slides=5))])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="grip"))
    out = capsys.readouterr().out
    assert e.value.code == 1 and "arms +1" in out and "swipecheck grip FAIL" in out
    assert link.sent[0] == (devctl.T_DEBUG, b'{"disp_force":0}') and link.sent[-1] == (devctl.T_DEBUG, b'{"disp_force":null}')
    assert not link.timed


def test_swipecheck_collects_swipe_lines_until_the_final_sample(fast, capsys):
    swipe = b"I (2) ui: swipe: end cls=edge q=0 why=none arm=1 b0=49 b=52 dx=0 dy=-60 up=60 pk=60 lat=0 arm_ms=40 r1=150 rev=0 gap=16 stp=8 ku=48 kl=0 dur=300"
    link = TimedLink([(0.0, bright_line(49)), (0.08, swipe), (0.25, bright_line(49))])
    with pytest.raises(SystemExit):
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="wipe"))
    assert "armed cls edge" in capsys.readouterr().out


def test_swipecheck_fails_without_a_final_line(fast, capsys):
    link = TimedLink([(0.0, bright_line(49))])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="grip"))
    assert e.value.code == 1 and "no final 'bright lvl=' line" in capsys.readouterr().out
    assert link.sent[-1] == (devctl.T_DEBUG, b'{"disp_force":null}')


def test_disp_brt_arc_forces_and_releases():
    link = StubLink()
    devctl.cmd_disp(link, disp_args(brt_arc="11"))
    devctl.cmd_disp(link, disp_args(brt_arc="null"))
    assert link.sent == [(devctl.T_DEBUG, b'{"brt_arc":11}'), (devctl.T_DEBUG, b'{"brt_arc":null}')]
