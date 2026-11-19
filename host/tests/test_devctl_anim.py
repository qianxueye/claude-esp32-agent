import importlib.util
import re
import sys
import wave
from pathlib import Path
from types import SimpleNamespace

import pytest

SPEC = importlib.util.spec_from_file_location("devctl", Path(__file__).parents[2] / "firmware" / "tools" / "devctl.py")
devctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(devctl)

FIRMWARE = Path(__file__).parents[2] / "firmware" / "main"


def anim_lines(ms, frames=100, late=0, cuts=2, pack=1, verify=640, free=101000, large=40000, work=(150000, 9000), dec=(30000, 2100), boot=82, patch=(4200, 6100), defer=3):
    a = b"I (%d) app: anim: pack=%d skin=1 clip=0 frames=%d late=%d cuts=%d seek=0 verify_ms=%d iheap=%d/%d ilarge=%d" % (
        ms, pack, frames, late, cuts, verify, free, free - 900, large)
    t = b"I (%d) app: animt: dec_us=%d/%d work_us=%d/%d cut_ms=40/71 boot_ms=%d patch_us=%d/%d ring_defer=%d" % (
        ms, dec[0], dec[1], work[0], work[1], boot, patch[0], patch[1], defer)
    return [a, t]


def stats_line(ms, under=1):
    return b"I (%d) app: stats rx=10/100B crc=0 skip=0 tx=5/50B drop=0 spkq=0 spkovf=0 pa=0 under=%d slow=0 micgap=0 micdrop=0" % (ms, under)


def knob(ms, vol, saves):
    return b"I (%d) app: knob vol=%d eff=%d set=1 arms=0 cancels=0 edge=0 saves=%d ov=0/0/0" % (ms, vol, vol, saves)


def sample(ms, **kw):
    s = devctl.AnimSampler()
    lines = anim_lines(ms, **kw)
    assert s.feed(lines[0]) is None
    return s.feed(lines[1])


def test_anim_keys_follow_the_firmware_lines():
    text = (FIRMWARE / "aglue.h").read_text()
    fmt = re.search(r'#define AG_LINE_FMT "anim: ([^"]*)"', text).group(1)
    assert tuple(kv.split("=")[0] for kv in fmt.split()) == devctl.ANIM_KEYS
    fmt = re.search(r'#define AG_TIME_LINE_FMT "animt: ([^"]*)"', text).group(1)
    assert tuple(kv.split("=")[0] for kv in fmt.split()) == devctl.ANIMT_KEYS


def test_sampler_pairs_the_two_lines_of_one_period():
    s = devctl.AnimSampler()
    a1, t1 = anim_lines(2000, frames=10)
    a2, t2 = anim_lines(4000, frames=40)
    assert s.feed(t1) is None
    assert s.feed(a1) is None
    assert s.feed(a2) is None
    got = s.feed(t2)
    assert got["frames"] == 40 and got["ms"] == 4000 and got["iheap"] == (101000, 100100) and got["work_us"] == (150000, 9000)
    assert s.feed(t2) is None
    # Red proof: merging every anim line into one dict mixes the frames of one period with the times of another.
    merged = {}
    for p in (a1, t1, a2):
        merged.update(devctl.parse_kv(p.split(b": ", 2)[2].decode()))
    assert merged["frames"] == 40 and merged["dec_us"] == (30000, 2100) and "ms" not in merged


def test_report_computes_deltas_across_a_wrap():
    a = sample(2000, frames=0xFFFFFFF0, late=3, work=(0xFFFFFF00, 9000))
    b = sample(62000, frames=0x384 - 0x10, late=3, work=(0x384 * 10000 - 0x100, 9500))
    lines, failed = devctl.anim_report(a, b, devctl.parse_stats(stats_line(2000)), devctl.parse_stats(stats_line(62000)))
    text = "\n".join(lines)
    assert not failed
    assert "G1 PASS late +0; frames +900 in 60.0 s (15.00/s)" in text
    assert "G2 work mean 10000 us, worst so far 9500 us" in text
    assert "G3 PASS spkovf +0 under +0" in text
    assert "G2 boot frame 82 ms; patch_us over the last window: mean 4200, worst so far 6100; ring updates deferred in it: 3" in text
    assert "G4 cut_ms last 40 worst 71" in text and "G7 PASS pack=1 verify_ms=640" in text
    assert re.search(r"^G4 (?!PASS|FAIL)", text, re.M) and re.search(r"^G6 (?!PASS|FAIL)", text, re.M)


@pytest.mark.parametrize("kw, stats_b, rule", [
    ({"late": 1}, 1, "G1 FAIL"),
    ({}, 2, "G3 FAIL"),
    ({"free": 101000 - 2049}, 1, "G5 FAIL"),
    ({"large": 31743}, 1, "G5 FAIL"),
    ({"pack": 4}, 1, "G7 FAIL"),
    ({"verify": 1001}, 1, "G7 FAIL"),
])
def test_report_fails_each_judged_rule(kw, stats_b, rule):
    a, b = sample(2000), sample(62000, **kw)
    lines, failed = devctl.anim_report(a, b, devctl.parse_stats(stats_line(2000)), devctl.parse_stats(stats_line(62000, under=stats_b)))
    assert failed and any(line.startswith(rule) for line in lines)


class StubLink:
    def __init__(self):
        self.sent = []

    def send(self, ftype, payload=b""):
        self.sent.append((ftype, payload))

    def poll(self):
        return []


def disp_args(**kw):
    base = dict(scale=None, force=None, orbit=None, fill=None, arc=None, bench=None, brt=None, brt_arc=None, arc_over=None, anim_clip=None, seconds=0)
    base.update(kw)
    return SimpleNamespace(**base)


def test_disp_anim_clip_pins_and_releases():
    link = StubLink()
    devctl.cmd_disp(link, disp_args(anim_clip="3"))
    devctl.cmd_disp(link, disp_args(anim_clip="null"))
    assert link.sent == [(devctl.T_DEBUG, b'{"anim_clip":3}'), (devctl.T_DEBUG, b'{"anim_clip":null}')]


class FakeBoard:
    """A board behind one link: one 2 s log period per poll; a persist set is confirmed and saved once 5 s passed since the last save."""

    opens = 0

    def __init__(self, port=None, serial_number=None):
        FakeBoard.opens += 1
        self.sent, self.ms, self.vol, self.stored, self.saves, self.last_save = [], 1000, 55, 55, 4, 0
        self.hellos = [{"vol": self.vol, "vol_set": True}]
        self.pushback = []
        self.ser = SimpleNamespace(close=lambda: None)

    def connect(self, timeout=8.0):
        return True

    def send(self, ftype, payload=b""):
        self.sent.append((ftype, payload))
        if ftype == devctl.T_SET_VOLUME and len(payload) == 2 and payload[1] == 1:
            self.vol = payload[0]
            self.pushback.append((devctl.T_VOLUME, bytes([self.vol])))

    def poll(self):
        self.ms += 2000
        if self.vol != self.stored and self.ms - self.last_save >= 5000:
            self.stored, self.saves, self.last_save = self.vol, self.saves + 1, self.ms
        out, self.pushback = self.pushback, []
        lines = [stats_line(self.ms), knob(self.ms, self.vol, self.saves)] + anim_lines(self.ms, frames=self.ms // 66)
        return out + [(devctl.T_LOG, p) for p in lines]

    def wait_for(self, ftype, timeout, show=True, pred=None):
        for t, p in self.poll():
            if t == ftype and (pred is None or pred(p)):
                return p
        return None

    def volume_sets(self):
        return [p for t, p in self.sent if t == devctl.T_SET_VOLUME]


@pytest.fixture
def wav(tmp_path):
    path = tmp_path / "speech.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(bytes(3200))
    return path


@pytest.fixture
def board(monkeypatch):
    FakeBoard.opens = 0
    monkeypatch.setattr(devctl, "Link", FakeBoard)
    monkeypatch.setattr(devctl, "KEEPALIVE_S", 0.0)
    monkeypatch.setattr(devctl, "VOLUME_SAVE_WAIT_S", 1.0)
    return FakeBoard


def run_main(monkeypatch, *argv):
    monkeypatch.setattr(sys, "argv", ["devctl.py", *argv])
    devctl.main()


def test_gate_rejects_a_loud_volume_before_opening_the_port(monkeypatch, board, wav, capsys):
    with pytest.raises(SystemExit) as e:
        run_main(monkeypatch, "gate-anim", "--wav", str(wav), "--volume", "31")
    assert e.value.code != 0 and board.opens == 0
    assert "volume must be 0-30" in capsys.readouterr().err


def test_gate_rejects_a_wav_of_the_wrong_rate_before_opening_the_port(monkeypatch, board, tmp_path):
    path = tmp_path / "fast.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(48000)
        w.writeframes(bytes(96))
    with pytest.raises(SystemExit) as e:
        run_main(monkeypatch, "gate-anim", "--wav", str(path), "--volume", "20")
    assert e.value.code != 0 and board.opens == 0


def test_gate_run_opens_one_link_streams_and_restores_the_volume(monkeypatch, board, wav, capsys):
    streams = []
    monkeypatch.setattr(devctl, "stream_pcm", lambda link, pcm, **kw: streams.append((link, pcm)))
    links = []
    real = devctl.cmd_gate_anim
    monkeypatch.setattr(devctl, "cmd_gate_anim", lambda link, args: (links.append(link), real(link, args)))
    run_main(monkeypatch, "gate-anim", "--wav", str(wav), "--volume", "30")
    out = capsys.readouterr().out
    assert board.opens == 1 and len(streams) == 1 and streams[0] == (links[0], bytes(3200))
    assert links[0].volume_sets() == [bytes([30, 1]), bytes([55, 1])]
    assert "G1 PASS" in out and "G3 PASS" in out and "volume 55 restored and stored" in out
    assert (devctl.T_SET_STATE, bytes([devctl.STATES["speaking"]])) in links[0].sent


def test_volume_is_restored_when_streaming_raises(monkeypatch, board, wav):
    def broken(link, pcm, **kw):
        raise OSError("port gone")

    monkeypatch.setattr(devctl, "stream_pcm", broken)
    link = FakeBoard()
    with pytest.raises(OSError):
        devctl.cmd_gate_anim(link, SimpleNamespace(wav=bytes(3200), volume=25))
    assert link.volume_sets() == [bytes([25, 1]), bytes([55, 1])]


def test_volume_is_restored_when_the_board_never_confirms(monkeypatch, board, capsys):
    link = FakeBoard()
    monkeypatch.setattr(link, "wait_for", lambda *a, **kw: None)
    with pytest.raises(SystemExit) as e:
        devctl.cmd_gate_anim(link, SimpleNamespace(wav=bytes(3200), volume=25))
    assert e.value.code == 1 and link.volume_sets() == [bytes([25, 1]), bytes([55, 1])]
    assert "did not confirm volume 25" in capsys.readouterr().out


def first_line_confirms(seq):
    """A wrong rule: the first knob line at the restored volume counts as stored, though a save may still be due."""
    return any(vol == 55 for _, vol, _ in seq)


def restore_with(capsys, seq, extra=()):
    link = StubLink()
    polls = iter([[(devctl.T_LOG, knob(ms, vol, saves))] for ms, vol, saves in seq] + [[(devctl.T_LOG, p)] for p in extra])
    link.poll = lambda: next(polls, [])
    return devctl.restore_volume(link, 55, True), capsys.readouterr().out


@pytest.mark.parametrize("seq, stored", [
    ([(9000, 30, 8), (11000, 55, 8), (13000, 55, 8), (15000, 55, 8), (17000, 55, 8), (19000, 55, 8)], True),
    ([(9000, 55, 8), (11000, 55, 9), (13000, 55, 9), (15000, 55, 9), (17000, 55, 9)], False),
    ([(9000, 55, 8), (11000, 55, 9), (13000, 55, 9), (15000, 55, 9), (17000, 55, 9), (19000, 55, 9)], True),
    ([(9000, 55, 8), (11000, 30, 8), (13000, 55, 8), (15000, 55, 8), (17000, 55, 8)], False),
])
def test_restore_waits_for_a_quiet_settle_span(board, capsys, seq, stored):
    """Stored only after 7 s of board time at the restored volume without a save; a save or another volume restarts the span."""
    got, out = restore_with(capsys, seq)
    assert got == stored and ("restored and stored" in out) == stored
    # Red proof: taking the first line at the restored volume would call every case stored.
    assert first_line_confirms(seq)


def test_restore_fails_on_a_write_failure(board, capsys):
    got, out = restore_with(capsys, [(9000, 55, 8)], extra=[b"W (10000) volume: nvs write failed (ESP_FAIL): retrying every 5000 ms"])
    assert not got and "storage failed" in out
