import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest

SPEC = importlib.util.spec_from_file_location("devctl", Path(__file__).parents[2] / "firmware" / "tools" / "devctl.py")
devctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(devctl)

BRIGHT = b"I (5000) app: bright lvl=49 eff=49 panel=49 set=1 arms=3 cancels=0 slides=4 stops=1 saves=1 in=2"


class StubLink:
    def __init__(self, frames=()):
        self.sent = []
        self.frames = list(frames)

    def send(self, ftype, payload=b""):
        self.sent.append((ftype, payload))

    def poll(self):
        out, self.frames = self.frames, []
        return out


def disp_args(**kw):
    base = dict(scale=None, force=None, orbit=None, fill=None, arc=None, bench=None, brt=None, seconds=0)
    base.update(kw)
    return SimpleNamespace(**base)


def test_brightness_sends_brt_set_and_prints_the_bright_line(capsys):
    link = StubLink([(devctl.T_LOG, b"I (4000) app: knob vol=70"), (devctl.T_LOG, BRIGHT)])
    devctl.cmd_brightness(link, SimpleNamespace(value=49))
    assert link.sent == [(devctl.T_DEBUG, b'{"brt_set":49}')]
    assert "bright lvl=49" in capsys.readouterr().out


def test_brightness_fails_loudly_without_a_bright_line(monkeypatch, capsys):
    monkeypatch.setattr(devctl, "BRIGHT_WAIT_S", 0.05)
    link = StubLink([(devctl.T_LOG, b"I (4000) app: knob vol=70")])
    with pytest.raises(SystemExit) as e:
        devctl.cmd_brightness(link, SimpleNamespace(value=49))
    assert e.value.code == 1 and "no 'bright lvl=' line" in capsys.readouterr().out


def test_disp_brt_pins_and_releases():
    link = StubLink()
    devctl.cmd_disp(link, disp_args(brt="20"))
    devctl.cmd_disp(link, disp_args(brt="null", force="0"))
    assert link.sent == [(devctl.T_DEBUG, b'{"brt":20}'), (devctl.T_DEBUG, b'{"disp_force":0,"brt":null}')]
    assert devctl.PROTO == 3


def test_parse_lines():
    assert devctl.parse_bright(BRIGHT) == dict(lvl=49, eff=49, panel=49, set=1, arms=3, cancels=0, slides=4, stops=1, saves=1, **{"in": 2})
    s = devctl.parse_swipe(b"I (1) ui: swipe: end cls=centre q=1 why=lateral arm=0 b0=-1 b=-1 dx=30 dy=-2 up=30 pk=30 lat=25 arm_ms=-1 r1=31 rev=0 dur=200")
    assert s["cls"] == "centre" and s["why"] == "lateral" and s["arm"] == 0 and s["lat"] == 25 and s["b0"] == -1


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
