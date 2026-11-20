import importlib.util
import re
import struct
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

SPEC = importlib.util.spec_from_file_location("devctl", Path(__file__).parents[2] / "firmware" / "tools" / "devctl.py")
devctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(devctl)

BRIGHT = b"I (5000) app: bright lvl=49 eff=49 panel=49 set=1 arms=3 rim=1 cancels=0 slides=4 stops=1 saves=1"


class StubLink:
    def __init__(self, frames=()):
        self.sent = []
        self.frames = list(frames)

    def send(self, ftype, payload=b""):
        self.sent.append((ftype, payload))

    def poll(self):
        out, self.frames = self.frames, []
        return out


class MarkerLink(StubLink):
    """Answers the first PING with its PONG; frames before that ping are pre-command input."""

    def __init__(self):
        super().__init__()
        self.marker, self.ponged = None, False

    def send(self, ftype, payload=b""):
        super().send(ftype, payload)
        if ftype == devctl.T_PING and self.marker is None:
            self.marker = payload

    def pong(self):
        out = [(devctl.T_PONG, self.marker)] if self.marker is not None and not self.ponged else []
        self.ponged = self.ponged or bool(out)
        return out


class TimedLink(MarkerLink):
    """After the marker ping, delivers the PONG and then each (seconds after that ping, payload) LOG frame once its time has come."""

    def __init__(self, timed=()):
        super().__init__()
        self.timed = sorted(timed, key=lambda f: f[0])
        self.start = None

    def send(self, ftype, payload=b""):
        super().send(ftype, payload)
        if ftype == devctl.T_PING and self.start is None:
            self.start = time.monotonic()

    def poll(self):
        time.sleep(0.002)
        if self.start is None:
            return []
        now = time.monotonic()
        due = [f for f in self.timed if f[0] <= now - self.start]
        self.timed = self.timed[len(due):]
        return self.pong() + [(devctl.T_LOG, p) for _, p in due]


def disp_args(**kw):
    base = dict(scale=None, force=None, orbit=None, fill=None, arc=None, bench=None, brt=None, brt_arc=None, arc_over=None, seconds=0)
    base.update(kw)
    return SimpleNamespace(**base)


def test_brightness_sends_brt_set_and_prints_the_bright_line(capsys):
    link = StubLink([(devctl.T_LOG, b"I (4000) app: knob vol=70"), (devctl.T_LOG, BRIGHT)])
    devctl.cmd_brightness(link, SimpleNamespace(value=49))
    assert link.sent == [(devctl.T_DEBUG, b'{"brt_set":49}')]
    assert "bright lvl=49" in capsys.readouterr().out


def bright_line(lvl, arms=3, slides=4, ms=5000):
    return b"I (%d) app: bright lvl=%d eff=%d panel=%d set=1 arms=%d rim=1 cancels=0 slides=%d stops=1 saves=1" % (ms, lvl, lvl, lvl, arms, slides)


def bright_line_ov(*args, **kw):
    return bright_line(*args, **kw) + b" ov=1/2/3"


def test_ov_counters_change_neither_the_parse_nor_the_verdict(fast, capsys):
    assert devctl.parse_bright(BRIGHT + b" ov=1/2/3") == devctl.parse_bright(BRIGHT)
    assert "ov" not in devctl.BRIGHT_KEYS and "ov" not in devctl.BRIGHT_COUNTERS
    for lines, verdict in ((bright_line, "PASS"), (bright_line_ov, "PASS")):
        link = TimedLink([(0.0, lines(49)), (0.2, lines(49, ms=5200))])
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="wipe"))
        assert "swipecheck wipe %s" % verdict in capsys.readouterr().out
    for lines in (bright_line, bright_line_ov):
        link = TimedLink([(0.0, lines(49, arms=3)), (0.2, lines(49, arms=4, ms=5200))])
        with pytest.raises(SystemExit):
            devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="grip"))
        assert "swipecheck grip FAIL" in capsys.readouterr().out


def test_disp_arc_over_sends_the_aid():
    link = StubLink()
    devctl.cmd_disp(link, disp_args(arc="100", arc_over="-3"))
    devctl.cmd_disp(link, disp_args(arc_over="null"))
    assert link.sent == [(devctl.T_DEBUG, b'{"arc":100,"arc_over":-3}'), (devctl.T_DEBUG, b'{"arc_over":null}')]


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
    link = TimedLink([(0.0, bright_line(49, arms=3)), (0.2, bright_line(49, arms=4, ms=5200))])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="grip"))
    assert e.value.code == 1 and "swipecheck grip FAIL" in capsys.readouterr().out
    assert link.sent[0] == (devctl.T_DEBUG, b'{"disp_force":0}') and link.sent[-1] == (devctl.T_DEBUG, b'{"disp_force":null}')


def test_swipecheck_pass_returns_normally(fast, capsys):
    link = TimedLink([(0.0, bright_line(49)), (0.2, bright_line(49, ms=5200))])
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
    assert devctl.parse_bright(BRIGHT) == dict(lvl=49, eff=49, panel=49, set=1, arms=3, rim=1, cancels=0, slides=4, stops=1, saves=1)
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


def counters_only_verdict(first, last):
    """A verdict on the arms counter and the level alone, without the armed-line rule."""
    return last["arms"] - first["arms"] == 0 and first["lvl"] == last["lvl"]


def test_bright_keys_follow_the_firmware_line():
    fmt = re.search(r'#define BRIGHT_LINE_FMT "bright (lvl=[^"]*)"', (FIRMWARE / "bright.h").read_text()).group(1)
    keys = tuple(kv.split("=")[0] for kv in fmt.split())
    assert keys == devctl.BRIGHT_KEYS + ("ov",)
    fmt = re.search(r'#define VK_SWIPE_LINE_FMT "swipe: end ([^"]*)"', (FIRMWARE / "volknob.h").read_text()).group(1)
    assert [kv.split("=")[0] for kv in fmt.split()][-5:] == ["gap", "stp", "ku", "kl", "dur"]


def test_armed_line_with_equal_counters_fails_grip_and_wipe():
    first = devctl.parse_bright(BRIGHT)
    for label in ("grip", "wipe"):
        lines, ok = devctl.swipe_report(label, first, dict(first), [ARMED_GRIP])
        assert not ok and lines[-1] == "swipecheck %s FAIL" % label and "armed cls edge" in "\n".join(lines)
    # Counters and level alone pass it: the red proof.
    assert counters_only_verdict(first, dict(first))


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


def test_swipecheck_final_sample_catches_a_late_arm(monkeypatch, capsys):
    """An arm after the last bright line inside the window shows in the final line; the pin is released after it."""
    monkeypatch.setattr(devctl, "SWIPE_FINAL_GUARD_S", 0.2)
    monkeypatch.setattr(devctl, "BRIGHT_WAIT_S", 1.0)
    link = TimedLink([(0.0, bright_line(49)), (0.1, bright_line(49, ms=5100)), (0.25, bright_line(49, arms=3, slides=5, ms=5250)),
                      (0.6, bright_line(49, arms=4, slides=5, ms=5600))])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.2, label="grip"))
    out = capsys.readouterr().out
    assert e.value.code == 1 and "arms +1" in out and "swipecheck grip FAIL" in out
    assert link.sent[0] == (devctl.T_DEBUG, b'{"disp_force":0}') and link.sent[-1] == (devctl.T_DEBUG, b'{"disp_force":null}')
    assert not link.timed


def test_swipecheck_collects_swipe_lines_until_the_final_sample(fast, capsys):
    swipe = b"I (2) ui: swipe: end cls=edge q=0 why=none arm=1 b0=49 b=52 dx=0 dy=-60 up=60 pk=60 lat=0 arm_ms=40 r1=150 rev=0 gap=16 stp=8 ku=48 kl=0 dur=300"
    link = TimedLink([(0.0, bright_line(49)), (0.08, swipe), (0.25, bright_line(49, ms=5250))])
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


def host_clock_final(ms, first_ms, window_s):
    """A wrong rule: any bright line read after the host's window counts as final, whatever its board time."""
    return time.monotonic() >= host_clock_final.due


class PausedHostLink(MarkerLink):
    """The marker answer and the baseline arrive alone; the host then stalls past the window and reads one buffered batch."""

    def __init__(self, batch, stall_s):
        super().__init__()
        self.base = [(devctl.T_LOG, bright_line(49))]
        self.batch, self.stall_s = batch, stall_s

    def poll(self):
        if self.marker is None:
            return []
        if self.base:
            out, self.base = self.base, []
            return self.pong() + out
        if self.batch:
            time.sleep(self.stall_s)
            out, self.batch = self.batch, []
            return [(devctl.T_LOG, p) for p in out]
        return []


ARMED_LINE = b"I (5060) ui: swipe: end cls=edge q=0 why=none arm=1 b0=49 b=52 dx=0 dy=-60 up=60 pk=60 lat=0 arm_ms=40 r1=150 rev=0 gap=16 stp=8 ku=48 kl=0 dur=300"


def buffered_batch():
    return [bright_line(49, ms=5050), ARMED_LINE, bright_line(49, ms=5300)]


@pytest.mark.parametrize("label", ["grip", "wipe"])
def test_buffered_batch_with_an_accidental_arm_fails(fast, capsys, label):
    """An unchanged bright line from before the window ends, then an arm, then one from after: the arm is kept and the run fails."""
    link = PausedHostLink(buffered_batch(), stall_s=0.2)
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label=label))
    assert e.value.code == 1 and "armed cls edge" in capsys.readouterr().out


@pytest.mark.parametrize("label", ["grip", "wipe"])
def test_host_clock_rule_passes_the_buffered_batch_with_an_arm(fast, capsys, label):
    """The red proof: judging the final line by the host's clock takes the pre-window line and passes the run."""
    host_clock_final.due = time.monotonic() + 0.1
    link = PausedHostLink(buffered_batch(), stall_s=0.2)
    devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label=label), is_final=host_clock_final)
    out = capsys.readouterr().out
    assert "swipecheck %s PASS" % label in out and "armed cls" not in out


def test_a_bright_line_without_a_board_time_is_never_final(fast, capsys):
    link = TimedLink([(0.0, bright_line(49)), (0.2, bright_line(49).replace(b"I (5000) ", b""))])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="grip"))
    assert e.value.code == 1 and "no final" in capsys.readouterr().out


class StaleLink(MarkerLink):
    """Holds stale input from before the command, then after the marker ping answers it and delivers fresh lines in one batch."""

    def __init__(self, stale, fresh, answer=True):
        super().__init__()
        self.stale, self.fresh, self.answer = stale, fresh, answer

    def poll(self):
        out = [(devctl.T_LOG, p) for p in self.stale]
        self.stale = []
        if self.marker is not None and self.fresh is not None:
            out += (self.pong() if self.answer else []) + [(devctl.T_LOG, p) for p in self.fresh]
            self.fresh = None
        time.sleep(0.002)
        return out


def pre_marker_swipecheck(link, args, is_final=devctl.final_by_board):
    """A wrong rule: the earlier swipecheck, which takes the first bright line it reads as the baseline whatever came before the command."""
    first = last = first_ms = None
    swipes = []
    next_ping, seq = 0.0, 0

    def logs():
        nonlocal next_ping, seq
        if time.monotonic() >= next_ping:
            seq += 1
            link.send(devctl.T_PING, struct.pack("<I", seq))
            next_ping = time.monotonic() + 1.0
        return [p for t, p in link.poll() if t == devctl.T_LOG]

    link.send(devctl.T_DEBUG, b'{"disp_force":0}')
    try:
        end = time.monotonic() + devctl.BRIGHT_WAIT_S
        while last is None and time.monotonic() < end:
            for p in logs():
                b, s = devctl.parse_bright(p), devctl.parse_swipe(p)
                if first is None:
                    if b:
                        first, first_ms = b, devctl.board_ms(p)
                        end = time.monotonic() + args.seconds + devctl.SWIPE_FINAL_GUARD_S + devctl.BRIGHT_WAIT_S
                elif last is None:
                    if s:
                        swipes.append(s)
                    if b and is_final(devctl.board_ms(p), first_ms, args.seconds):
                        last = b
    finally:
        link.send(devctl.T_DEBUG, b'{"disp_force":null}')
    return devctl.swipe_report(args.label, first, last, swipes)[1]


def stale_scenario():
    """Stale unchanged lines spanning the window sit in the buffer before the command; after the marker come a baseline, an armed swipe inside the window and a final line."""
    stale = [bright_line(49, ms=4000), bright_line(49, ms=4300)]
    fresh = [bright_line(49, ms=5000), ARMED_LINE, bright_line(49, arms=4, ms=5300)]
    return stale, fresh


@pytest.mark.parametrize("label", ["grip", "wipe"])
def test_stale_lines_before_the_marker_are_not_the_baseline(fast, capsys, label):
    """Pre-command lines are dropped and the arm after the marker is caught."""
    link = StaleLink(*stale_scenario())
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label=label))
    out = capsys.readouterr().out
    assert e.value.code == 1 and "armed cls edge" in out and "swipecheck %s FAIL" % label in out
    assert "(4000)" not in out and "(4300)" not in out
    assert link.sent[0] == (devctl.T_DEBUG, b'{"disp_force":0}') and link.sent[1][0] == devctl.T_PING and link.sent[-1] == (devctl.T_DEBUG, b'{"disp_force":null}')


@pytest.mark.parametrize("label", ["grip", "wipe"])
def test_pre_marker_rule_passes_the_stale_scenario(fast, label):
    """The red proof: taking the first bright line read as the baseline reports PASS on the reviewer's scenario."""
    stale, fresh = stale_scenario()
    link = StaleLink([], stale + fresh)
    assert pre_marker_swipecheck(link, SimpleNamespace(seconds=0.05, label=label)) is True


def test_lines_between_the_pin_and_the_marker_answer_are_ignored(fast, capsys):
    """A line that arrives after the pin but before the marker answer is dropped too."""
    link = StaleLink([], [bright_line(49, ms=4900), None, bright_line(49, ms=5000), bright_line(49, arms=4, ms=5300)])
    link.poll = lambda: [] if link.marker is None else [(devctl.T_LOG, link.fresh[0]), *link.pong(), (devctl.T_LOG, link.fresh[2]), (devctl.T_LOG, link.fresh[3])] if link.fresh else []
    with pytest.raises(SystemExit):
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="grip"))
    out = capsys.readouterr().out
    assert "(4900)" not in out and "arms +1" in out


class SilentLink(MarkerLink):
    """Never answers the marker ping; optionally delivers a PONG of another sequence number and bright lines."""

    def __init__(self, other_pong):
        super().__init__()
        self.other_pong = other_pong

    def poll(self):
        time.sleep(0.002)
        if self.marker is None or not self.other_pong:
            return []
        return [(devctl.T_PONG, struct.pack("<I", 7)), (devctl.T_LOG, bright_line(49, ms=5000)), (devctl.T_LOG, bright_line(49, ms=5300))]


@pytest.mark.parametrize("other_pong", [False, True])
def test_no_marker_answer_fails_without_a_pass(fast, capsys, other_pong):
    """No PONG, or a PONG of another sequence number, ends non-zero and releases the pin."""
    link = SilentLink(other_pong)
    with pytest.raises(SystemExit) as e:
        devctl.cmd_swipecheck(link, SimpleNamespace(seconds=0.05, label="wipe"))
    out = capsys.readouterr().out
    assert e.value.code == 1 and "did not answer the marker ping" in out and "PASS" not in out
    assert link.sent[-1] == (devctl.T_DEBUG, b'{"disp_force":null}')


def test_keep_alive_pings_use_other_sequence_numbers(monkeypatch, capsys):
    monkeypatch.setattr(devctl, "SWIPE_FINAL_GUARD_S", 0.05)
    link = TimedLink([(0.0, bright_line(49)), (1.3, bright_line(49, ms=6300))])
    devctl.cmd_swipecheck(link, SimpleNamespace(seconds=1.2, label="wipe"))
    seqs = [struct.unpack("<I", p)[0] for t, p in link.sent if t == devctl.T_PING]
    assert len(seqs) >= 2 and len(set(seqs)) == len(seqs)
