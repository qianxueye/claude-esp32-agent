import importlib.util
import json
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


def anim_lines(ms, frames=100, late=0, cuts=2, pack=1, skin=1, clip=0, verify=640, free=101000, large=40000, work=(150000, 9000), dec=(30000, 2100), boot=82, patch=(4200, 6100), defer=3):
    a = b"I (%d) app: anim: pack=%d skin=%d clip=%d frames=%d late=%d cuts=%d seek=0 verify_ms=%d iheap=%d/%d ilarge=%d id=%d" % (
        ms, pack, skin, clip, frames, late, cuts, verify, free, free - 900, large, ms // 2000)
    t = b"I (%d) app: animt: dec_us=%d/%d work_us=%d/%d cut_ms=40/71 boot_ms=%d patch_us=%d/%d ring_defer=%d id=%d" % (
        ms, dec[0], dec[1], work[0], work[1], boot, patch[0], patch[1], defer, ms // 2000)
    return [a, t]


def stats_line(ms, under=1):
    return b"I (%d) app: stats rx=10/100B crc=0 skip=0 tx=5/50B drop=0 spkq=0 spkovf=0 pa=0 under=%d slow=0 micgap=0 micdrop=0" % (ms, under)


def knob(ms, vol, eff):
    return b"I (%d) app: knob vol=%d eff=%d set=1 arms=0 cancels=0 edge=0 saves=4 ov=0/0/0" % (ms, vol, eff)


def sample(ms, **kw):
    s = devctl.AnimSampler()
    lines = [stats_line(ms - 3)] + anim_lines(ms, **kw)
    assert s.feed(lines[0]) is None and s.feed(lines[1]) is None
    return s.feed(lines[2])


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
    assert s.feed(stats_line(3997)) is None
    assert s.feed(a2) is None
    got = s.feed(t2)
    assert got["frames"] == 40 and got["ms"] == 4000 and got["iheap"] == (101000, 100100) and got["work_us"] == (150000, 9000)
    assert got["stats"] == devctl.parse_stats(stats_line(3997))
    assert s.feed(t2) is None
    # Red proof: merging every anim line into one dict mixes the frames of one period with the times of another.
    merged = {}
    for p in (a1, t1, a2):
        merged.update(devctl.parse_kv(p.split(b": ", 2)[2].decode()))
    assert merged["frames"] == 40 and merged["dec_us"] == (30000, 2100) and "ms" not in merged


def test_sampler_drops_a_group_missing_a_line_of_its_period():
    s = devctl.AnimSampler()
    a1, _ = anim_lines(2000)
    _, t2 = anim_lines(4000)
    assert s.feed(stats_line(1997)) is None and s.feed(a1) is None
    assert s.feed(t2) is None
    # A period whose stats line was lost does not borrow the previous period's counters.
    a3, t3 = anim_lines(6000)
    a4, t4 = anim_lines(8000)
    assert s.feed(stats_line(5997)) is None and s.feed(a3) is None and s.feed(t3)["stats"] is not None
    assert s.feed(a4) is None and s.feed(t4) is None


def colour(line, code=b"0;32"):
    return b"\x1b[" + code + b"m" + line + b"\x1b[0m"


def test_coloured_lines_parse_like_plain_ones():
    plain_stats = stats_line(2000)
    assert devctl.parse_stats(colour(plain_stats)) == devctl.parse_stats(plain_stats) is not None
    s = devctl.AnimSampler()
    a, t = anim_lines(2000)
    assert s.feed(colour(stats_line(1997))) is None and s.feed(colour(a)) is None
    assert s.feed(colour(t)) == sample(2000)
    # A field that is not a number drops the sample instead of reaching the report.
    s = devctl.AnimSampler()
    s.feed(stats_line(1997))
    s.feed(a.replace(b"frames=100", b"frames=1x"))
    assert s.feed(t) is None


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
    ({"frames": 100}, 1, "G0 FAIL"),
    ({"clip": -1}, 1, "G0 FAIL"),
    ({"skin": 0, "clip": -1}, 1, "G0 FAIL"),
])
def test_report_fails_each_judged_rule(kw, stats_b, rule):
    a, b = sample(2000), sample(62000, **{"frames": 200, **kw})
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
    """A board behind one link: one 2 s log period per poll; a transient set changes eff until HOST_HELLO, a persist set the user volume."""

    opens = 0

    def __init__(self, port=None, serial_number=None):
        FakeBoard.opens += 1
        self.sent, self.ms, self.vol, self.eff = [], 1000, 55, 55
        self.hellos = [{"vol": self.vol, "vol_set": True}]
        self.pushback = []
        self.ser = SimpleNamespace(close=lambda: None)

    def connect(self, timeout=8.0):
        return True

    def send(self, ftype, payload=b""):
        self.sent.append((ftype, payload))
        if ftype == devctl.T_SET_VOLUME:
            self.eff = payload[0]
            if payload[1:] == b"\x01":
                self.vol = payload[0]
        elif ftype == devctl.T_HOST_HELLO:
            self.eff = self.vol
            self.pushback.append((devctl.T_HELLO, b'{"vol":%d,"vol_set":true,"reason":"reply"}' % self.vol))
        elif ftype == devctl.T_SPK_FLUSH:
            self.pushback.append((devctl.T_SPK_STATUS, bytes(9)))

    def poll(self):
        self.ms += 2000
        out, self.pushback = self.pushback, []
        self.hellos += [json.loads(p) for t, p in out if t == devctl.T_HELLO]
        lines = [stats_line(self.ms), knob(self.ms, self.vol, self.eff)] + anim_lines(self.ms, frames=self.ms // 66)
        return out + [(devctl.T_LOG, p) for p in lines]

    def wait_for(self, ftype, timeout, show=True, pred=None):
        for t, p in self.poll():
            if t == ftype and (pred is None or pred(p)):
                return p
        return None

    def volume_sets(self):
        return [p for t, p in self.sent if t == devctl.T_SET_VOLUME]

    def types(self):
        return [t for t, _ in self.sent]


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
    monkeypatch.setattr(devctl, "EFF_WAIT_S", 0.5)
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


def ends_override(link, after):
    """True when SPK_FLUSH, then HOST_HELLO, follow the frame type 'after' and nothing persisted a volume."""
    t = link.types()
    k = len(t) - 1 - t[::-1].index(after)
    tail = t[k:]
    return devctl.T_SPK_FLUSH in tail and devctl.T_HOST_HELLO in tail[tail.index(devctl.T_SPK_FLUSH):] and link.vol == 55


def test_gate_sets_a_transient_volume_and_ends_it_with_host_hello(monkeypatch, board, wav, capsys):
    streams = []
    monkeypatch.setattr(devctl, "stream_pcm", lambda link, pcm, **kw: (streams.append((link, pcm)), link.sent.append(("stream", b"")))[0] or True)
    links = []
    real = devctl.cmd_gate_anim
    monkeypatch.setattr(devctl, "cmd_gate_anim", lambda link, args: (links.append(link), real(link, args)))
    run_main(monkeypatch, "gate-anim", "--wav", str(wav), "--volume", "30")
    out = capsys.readouterr().out
    assert board.opens == 1 and len(streams) == 1 and streams[0] == (links[0], bytes(3200))
    assert links[0].volume_sets() == [bytes([30, 0])]
    assert ends_override(links[0], "stream") and links[0].eff == 55
    assert "G1 PASS" in out and "G3 PASS" in out and "effective volume 55" in out
    assert (devctl.T_SET_STATE, bytes([devctl.STATES["speaking"]])) in links[0].sent


def test_override_ends_when_streaming_raises(monkeypatch, board, wav):
    def broken(link, pcm, **kw):
        link.sent.append(("stream", b""))
        raise OSError("port gone")

    monkeypatch.setattr(devctl, "stream_pcm", broken)
    link = FakeBoard()
    with pytest.raises(OSError):
        devctl.cmd_gate_anim(link, SimpleNamespace(wav=bytes(3200), volume=25))
    assert link.volume_sets() == [bytes([25, 0])] and ends_override(link, "stream")


def test_gate_does_not_stream_when_the_board_never_shows_the_volume(monkeypatch, board, capsys):
    link = FakeBoard()
    streams = []
    monkeypatch.setattr(devctl, "stream_pcm", lambda *a, **kw: streams.append(a))
    real_send = link.send
    monkeypatch.setattr(link, "send", lambda ftype, payload=b"": link.sent.append((ftype, payload)) if ftype == devctl.T_SET_VOLUME else real_send(ftype, payload))
    with pytest.raises(SystemExit) as e:
        devctl.cmd_gate_anim(link, SimpleNamespace(wav=bytes(3200), volume=25))
    assert e.value.code == 1 and not streams and ends_override(link, devctl.T_SET_VOLUME)
    assert "did not confirm volume 25" in capsys.readouterr().out


class SpeakerLink:
    """A link for stream_pcm whose speaker consumes `rate` bytes per poll and reports drained when `drains`."""

    def __init__(self, rate, drains):
        self.rate, self.drains, self.sent_bytes, self.spk_consumed, self.spk_flags, self.polls = rate, drains, 0, 0, 0, 0

    def poll(self):
        self.polls += 1
        if self.polls > 2_000_000:
            raise RuntimeError("stream_pcm has no bound")
        self.spk_consumed = min(self.sent_bytes, self.spk_consumed + self.rate)
        return []

    def outstanding(self):
        return self.sent_bytes - self.spk_consumed

    def send_audio(self, chunk):
        self.sent_bytes += len(chunk)

    def send(self, ftype, payload=b""):
        if ftype == devctl.T_SPK_END and self.drains:
            self.spk_flags = 2


def test_stream_returns_false_on_a_stall_or_a_drain_timeout(monkeypatch, board, capsys):
    monkeypatch.setattr(devctl, "STREAM_STALL_S", 0.05)
    monkeypatch.setattr(devctl, "STREAM_DRAIN_S", 0.05)
    pcm = bytes(devctl.OUTSTANDING_LIMIT * 2)
    assert devctl.stream_pcm(SpeakerLink(4096, True), pcm, tail=0) is True
    assert devctl.stream_pcm(SpeakerLink(0, True), pcm, tail=0) is False
    assert devctl.stream_pcm(SpeakerLink(4096, False), pcm, tail=0) is False
    out = capsys.readouterr().out
    assert "stalled" in out and "drain timeout" in out
    # The gate fails on such a stream and still ends the override.
    monkeypatch.setattr(devctl, "stream_pcm", lambda link, pcm, **kw: link.sent.append(("stream", b"")) or False)
    link = FakeBoard()
    with pytest.raises(SystemExit) as e:
        devctl.cmd_gate_anim(link, SimpleNamespace(wav=bytes(3200), volume=25))
    assert e.value.code == 1 and ends_override(link, "stream")
