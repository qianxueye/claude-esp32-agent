#!/usr/bin/env python3
"""Test client for the Clawd terminal firmware (docs/protocol.md). Run with .venv/bin/python."""
import argparse
import json
import os
import re
import struct
import sys
import time
import wave
import zlib

import serial
from serial.tools import list_ports

USB_VID, USB_PID = 0x303A, 0x1001
PROTO = 3
SYNC = b"\xC1\xAD"
MAX_PAYLOAD = 4096
PARTIAL_TIMEOUT = 0.2

T_HELLO, T_LOG, T_MIC, T_INPUT, T_SPK_STATUS, T_VOLUME = 0x01, 0x02, 0x10, 0x20, 0x21, 0x22
T_PONG, T_SELFTEST_RESULT, T_SNAP_BEGIN, T_SNAP_DATA, T_SNAP_END = 0x30, 0x31, 0x32, 0x33, 0x34
T_HOST_HELLO, T_SET_STATE, T_SET_VOLUME = 0x81, 0x82, 0x83
T_SPK_AUDIO, T_SPK_FLUSH, T_SPK_END = 0x90, 0x91, 0x92
T_PING, T_SELFTEST, T_SNAPSHOT = 0xA0, 0xA2, 0xA3
T_PAIR_RESULT, T_WIFI_STATUS, T_FORGET_RESULT, T_PAIR, T_FORGET, T_DEBUG = 0x35, 0x36, 0x37, 0x84, 0x85, 0xF0
STATES = {"idle": 0, "listening": 1, "thinking": 2, "speaking": 3, "error": 4, "offline": 5}
OUTSTANDING_LIMIT = 64000
CHUNK = 4096
BRIGHT_WAIT_S = 5.0
BRT_MIN_PERCENT, BRT_STEPS = 20, 32  # firmware/main/app_config.h; a host test compares them
SWIPE_FINAL_GUARD_S = 0.5  # plain configuration: the final bright line must start this long after the window
BRIGHT_KEYS = ("lvl", "eff", "panel", "set", "arms", "rim", "cancels", "slides", "stops", "saves")
BRIGHT_COUNTERS = ("arms", "rim", "cancels", "slides", "stops", "saves")
BRIGHT_LINE = re.compile(rb"bright " + rb" ".join(k.encode() + rb"=(-?\d+)" for k in BRIGHT_KEYS))
BATT_KEYS = ("pct", "present", "vbus", "dir", "chg", "mv", "bars")
BATT_LINE = re.compile(rb"batt " + rb" ".join(k.encode() + rb"=(-?\d+)" for k in BATT_KEYS) + rb" look=(\w+)")
BATT_STALE = re.compile(rb"\(\d+\) \w+: batt: stale(?:\x1b\[0m)?$")
SWIPE_LINE = re.compile(rb"swipe: end (.*)")
BOARD_MS = re.compile(rb"\((\d+)\) ")
ANSI_CODE = re.compile(r"\x1b\[[0-9;]*m")
ANIM_KEYS = ("pack", "skin", "clip", "frames", "late", "cuts", "seek", "verify_ms", "iheap", "ilarge", "id")  # AG_LINE_FMT in firmware/main/aglue.h
ANIMT_KEYS = ("dec_us", "work_us", "cut_ms", "boot_ms", "patch_us", "ring_defer", "id")  # AG_TIME_LINE_FMT
AUDIO_KEYS = ("spkovf", "under", "slow", "micgap", "micdrop")  # stats line counters that gate G3 needs unchanged
STATS_PAIR_MS = 1000  # mathematical model: half the 2 s log period; one period's stats and anim lines share a tick
STREAM_STALL_S = 5.0  # plain configuration: no speaker progress for this long ends a stream as stalled
STREAM_DRAIN_S = 30.0  # plain configuration: longest wait for the drained flag after SPK_END
KEEPALIVE_S = 1.0  # plain configuration: below the board's 5 s host-silence timeout
GATE_VOLUME_MAX = 30  # Owner rule: speech volume during a gate run
GATE_VERIFY_MS_MAX = 1000  # gate G7, plain configuration, placeholder
GATE_HEAP_DROP_MAX = 2048  # gate G5, plain configuration, placeholder until the TLS budget is known
GATE_HEAP_LARGEST_MIN = 31744  # gate G5, plain configuration, placeholder
EFF_WAIT_S = 5.0  # plain configuration: two 2 s knob log periods plus 1 s
GATE_SAMPLE_WAIT_S = 6.0  # plain configuration: three 2 s log periods for one anim sample


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype, payload=b""):
    body = bytes([ftype]) + struct.pack("<H", len(payload)) + payload
    return SYNC + body + struct.pack("<H", crc16(body))


def find_port(serial_number=None):
    """Return the port of the single USB-Serial-JTAG board, or the one with the given serial number."""
    serial_number = serial_number or os.environ.get("CLAWD_SERIAL")
    found = [p for p in list_ports.comports() if p.vid == USB_VID and p.pid == USB_PID
             and (not serial_number or p.serial_number == serial_number)]
    if not found:
        sys.exit("no board found (VID 0x%04X PID 0x%04X%s)" % (USB_VID, USB_PID, ", serial " + serial_number if serial_number else ""))
    if len(found) > 1:
        sys.exit("several boards found, pin one with --serial or CLAWD_SERIAL: " + ", ".join(str(p.serial_number) for p in found))
    return found[0].device


class Link:
    def __init__(self, port=None, serial_number=None):
        self.port = port or find_port(serial_number)
        self.ser = serial.Serial()
        self.ser.port = self.port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.002
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.ser.dtr = False
        self.ser.rts = False
        self.buf = bytearray()
        self.wait_since = None
        self.crc_errors = 0
        self.rx_bytes = 0
        self.tx_bytes = 0
        self.spk_queued = 0
        self.spk_flags = 0
        self.spk_consumed = 0
        self.spk_base = None
        self.spk_sent = 0
        self.hellos = []
        self.spk_status_time = 0.0
        self.pushback = []

    def connect(self, timeout=8.0):
        """Send HOST_HELLO until HELLO arrives; opening the port may reset the board."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.send(T_HOST_HELLO, b'{"proto":%d}' % PROTO)
            if self.wait_for(T_HELLO, 1.0, show=False) is not None:
                self.send(T_SPK_FLUSH)
                self.wait_for(T_SPK_STATUS, 1.0, show=False)
                self.spk_base, self.spk_sent = self.spk_consumed, 0
                return True
        print("no HELLO from device")
        return False

    def outstanding(self):
        """Bytes sent since connect that the device has not consumed yet."""
        return (self.spk_sent - (self.spk_consumed - self.spk_base)) & 0xFFFFFFFF

    def send_audio(self, chunk):
        self.send(T_SPK_AUDIO, chunk)
        self.spk_sent += len(chunk)

    def send(self, ftype, payload=b""):
        frame = encode(ftype, payload)
        self.ser.write(frame)
        self.tx_bytes += len(frame)

    def _parse(self):
        while True:
            i = self.buf.find(SYNC)
            if i < 0:
                keep = 1 if self.buf[-1:] == SYNC[:1] else 0
                del self.buf[: len(self.buf) - keep]
                self.wait_since = None
                return None
            if i:
                del self.buf[:i]
            if len(self.buf) < 5:
                return self._incomplete()
            n = struct.unpack_from("<H", self.buf, 3)[0]
            if n > MAX_PAYLOAD:
                del self.buf[:1]
                continue
            if len(self.buf) < 7 + n:
                r = self._incomplete()
                if r is False:
                    continue
                return None
            if crc16(bytes(self.buf[2 : 5 + n])) != struct.unpack_from("<H", self.buf, 5 + n)[0]:
                self.crc_errors += 1
                del self.buf[:1]
                continue
            frame = (self.buf[2], bytes(self.buf[5 : 5 + n]))
            del self.buf[: 7 + n]
            self.wait_since = None
            return frame

    def _incomplete(self):
        now = time.monotonic()
        if self.wait_since is None:
            self.wait_since = now
            return None
        if now - self.wait_since > PARTIAL_TIMEOUT:
            del self.buf[:1]
            self.wait_since = None
            return False
        return None

    def poll(self):
        """Read available bytes and return decoded frames as (type, payload)."""
        data = self.ser.read(8192)
        if data:
            self.rx_bytes += len(data)
            self.buf += data
        out, self.pushback = self.pushback, []
        while True:
            f = self._parse()
            if f is None:
                break
            if f[0] == T_SPK_STATUS and len(f[1]) >= 9:
                self.spk_queued, self.spk_flags, self.spk_consumed = struct.unpack("<IBI", f[1][:9])
                self.spk_status_time = time.monotonic()
            elif f[0] == T_HELLO:
                self.hellos.append(json.loads(f[1]))
            out.append(f)
        return out

    def wait_for(self, ftype, timeout, show=True, pred=None):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            frames = self.poll()
            for i, (t, p) in enumerate(frames):
                if show:
                    show_frame(t, p)
                if t == ftype and (pred is None or pred(p)):
                    self.pushback = frames[i + 1 :]
                    return p
        return None


def show_frame(t, p):
    if t == T_LOG:
        print("LOG   ", p.decode("utf-8", "replace"))
    elif t == T_HELLO:
        print("HELLO ", p.decode("utf-8", "replace"))
    elif t == T_INPUT:
        print("INPUT ", {1: "TAP", 2: "LONG_PRESS", 3: "BOOT"}.get(p[0], p[0]))
    elif t == T_PONG:
        print("PONG  ", struct.unpack("<I", p)[0])
    elif t == T_SELFTEST_RESULT:
        print("SELFTEST", p.decode())
    elif t == T_SPK_STATUS:
        print("SPK   ", struct.unpack("<IBI", p[:9]))
    elif t == T_VOLUME and len(p) == 1:
        print("VOLUME", p[0])
    elif t in (T_PAIR_RESULT, T_WIFI_STATUS, T_FORGET_RESULT):
        print({T_PAIR_RESULT: "PAIR_RESULT", T_FORGET_RESULT: "FORGET_RESULT"}.get(t, "WIFI  "), p.decode())
    elif t not in (T_MIC, T_SNAP_DATA):
        print("FRAME %02x" % t, p[:40].hex())


def write_png(path, w, h, rgb):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def rgb565_to_rgb(data):
    out = bytearray(len(data) // 2 * 3)
    for i in range(0, len(data) - 1, 2):
        v = data[i] | (data[i + 1] << 8)
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        j = i // 2 * 3
        out[j] = (r << 3) | (r >> 2)
        out[j + 1] = (g << 2) | (g >> 4)
        out[j + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def max_radius(rgb, w, h):
    """Largest distance from the screen centre of any non-black pixel."""
    best = 0.0
    cx, cy = w / 2.0, h / 2.0
    for y in range(h):
        row = rgb[y * w * 3 : (y + 1) * w * 3]
        if not any(row):
            continue
        xs = [x for x in range(w) if row[3 * x] or row[3 * x + 1] or row[3 * x + 2]]
        for x in (xs[0], xs[-1]):
            best = max(best, ((x + 0.5 - cx) ** 2 + (y + 0.5 - cy) ** 2) ** 0.5)
    return best


def cmd_listen(link, args):
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        for t, p in link.poll():
            show_frame(t, p)


def cmd_ping(link, args):
    rtts = []
    for i in range(args.count):
        t0 = time.monotonic()
        link.send(T_PING, struct.pack("<I", i + 1))
        p = link.wait_for(T_PONG, 1.0, show=False, pred=lambda p, i=i: struct.unpack("<I", p)[0] == i + 1)
        if p is None:
            print("ping %d: timeout" % i)
        else:
            rtts.append((time.monotonic() - t0) * 1000)
    if rtts:
        print("PONG %d/%d rtt ms min %.1f avg %.1f max %.1f" % (len(rtts), args.count, min(rtts), sum(rtts) / len(rtts), max(rtts)))


def cmd_hello(link, args):
    link.send(T_HOST_HELLO, b'{"proto":%d}' % args.proto)
    link.wait_for(T_HELLO, 2.0)
    link.wait_for(T_WIFI_STATUS, 0.5)


def cmd_state(link, args):
    link.send(T_SET_STATE, bytes([STATES.get(args.state, None) if args.state in STATES else int(args.state)]))


def cmd_volume(link, args):
    """Sets the volume for this session only; --save stores it on the board."""
    link.send(T_SET_VOLUME, bytes([args.value, 1]) if args.save else bytes([args.value]))


def cmd_selftest(link, args):
    link.send(T_SET_VOLUME, bytes([args.volume]))
    link.send(T_SELFTEST)
    p = link.wait_for(T_SELFTEST_RESULT, 6.0)
    print("result:", p.decode() if p else "timeout")


def snapshot(link, path, timeout=8.0, attempts=3):
    for _ in range(attempts):
        if snapshot_once(link, path, timeout):
            return True
    return False


def snapshot_once(link, path, timeout=8.0):
    link.send(T_PING, b"\0\0\0\0")
    link.send(T_SNAPSHOT)
    begin = link.wait_for(T_SNAP_BEGIN, 5.0, show=False)
    if begin is None:
        print("snapshot: no SNAP_BEGIN")
        return False
    info = json.loads(begin)
    data = bytearray()
    end = time.monotonic() + timeout
    total = None
    while time.monotonic() < end and total is None:
        for t, p in link.poll():
            if t == T_SNAP_DATA:
                data += p
            elif t == T_SNAP_END:
                total = struct.unpack("<I", p)[0]
            elif t == T_LOG:
                show_frame(t, p)
    if total is None or total != len(data) or total != info["w"] * info["h"] * 2:
        print("snapshot: incomplete (%d bytes, end=%s, crc errors %d)" % (len(data), total, link.crc_errors))
        return False
    rgb = rgb565_to_rgb(bytes(data))
    write_png(path, info["w"], info["h"], rgb)
    print("snapshot saved", path, total, "bytes, max content radius %.1f px" % max_radius(rgb, info["w"], info["h"]))
    return True


def cmd_snapshot(link, args):
    snapshot(link, args.out)


def cmd_snapshots(link, args):
    for name, val in STATES.items():
        link.send(T_SET_STATE, bytes([val]))
        time.sleep(0.5)
        link.poll()
        snapshot(link, "%s/state%d-%s.png" % (args.outdir, val, name))
        link.send(T_PING, b"\0\0\0\0")


def cmd_play(link, args):
    w = wave.open(args.wav, "rb")
    assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2, "need 16 kHz mono s16"
    pcm = w.readframes(w.getnframes())
    stream_pcm(link, pcm)


def stream_pcm(link, pcm, record=None, tail=1.0):
    """Stream PCM with the 64000-byte outstanding limit; optionally collect MIC_AUDIO into record (list).

    Pings while waiting so the board stays online; returns True once it reports the queue drained, False on a stall or a drain timeout."""
    sent = 0
    t0 = time.monotonic()
    drain_at = None
    progress, ping = (link.spk_consumed, t0), Keepalive(link)
    while True:
        ping.tick()
        frames = link.poll()
        if record is not None:
            for t, p in frames:
                if t == T_MIC:
                    record.append(p)
        while sent < len(pcm) and link.outstanding() + CHUNK <= OUTSTANDING_LIMIT:
            chunk = pcm[sent : sent + CHUNK]
            link.send_audio(chunk)
            sent += len(chunk)
        if sent >= len(pcm) and drain_at is None:
            link.send(T_SPK_END)
            drain_at = time.monotonic()
        if drain_at is not None and (link.spk_flags & 2):
            ok = True
            break
        now = time.monotonic()
        if link.spk_consumed != progress[0]:
            progress = (link.spk_consumed, now)
        elif now - progress[1] > STREAM_STALL_S:
            print("stream stalled: no speaker progress for %.0f s" % STREAM_STALL_S)
            ok = False
            break
        if drain_at is not None and now - drain_at > STREAM_DRAIN_S:
            print("drain timeout")
            ok = False
            break
    t_play = time.monotonic() - t0
    end = time.monotonic() + tail
    while time.monotonic() < end:
        ping.tick()
        for t, p in link.poll():
            if record is not None and t == T_MIC:
                record.append(p)
    print("played %d bytes in %.2fs (audio %.2fs)" % (len(pcm), t_play, len(pcm) / 32000))
    return ok


def save_wav(path, pcm):
    w = wave.open(path, "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(16000)
    w.writeframes(pcm)
    w.close()


def cmd_record(link, args):
    link.send(T_SET_STATE, bytes([1]))
    chunks = []
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        for t, p in link.poll():
            if t == T_MIC:
                chunks.append(p)
    link.send(T_SET_STATE, bytes([0]))
    save_wav(args.out, b"".join(chunks))
    print("recorded %d frames (%.2fs) to %s" % (len(chunks), len(chunks) * 0.02, args.out))


def cmd_loopback(link, args):
    w = wave.open(args.wav, "rb")
    assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2
    pcm = w.readframes(w.getnframes())
    link.send(T_SET_VOLUME, bytes([args.volume]))
    link.send(T_SET_STATE, bytes([1]))
    time.sleep(0.5)
    chunks = []
    stream_pcm(link, pcm, record=chunks, tail=args.tail)
    link.send(T_SET_STATE, bytes([0]))
    save_wav(args.out, b"".join(chunks))
    print("recorded %d frames (%.2fs) to %s" % (len(chunks), len(chunks) * 0.02, args.out))


def cmd_stress(link, args):
    """Mic stream in plus SPK_AUDIO/PING traffic out for N seconds; reports throughput and CRC errors."""
    link.send(T_SET_STATE, bytes([1]))
    mic_bytes = 0
    pings = pongs = 0
    sent = 0
    last_stats = ""
    tone = bytes(CHUNK)  # silence: throughput needs no audible audio
    link.send(T_SET_VOLUME, bytes([0]))
    link.crc_errors = 0
    rx0, tx0 = link.rx_bytes, link.tx_bytes
    t0 = time.monotonic()
    next_ping = t0
    while time.monotonic() - t0 < args.seconds:
        for t, p in link.poll():
            if t == T_MIC:
                mic_bytes += len(p)
            elif t == T_PONG:
                pongs += 1
            elif t == T_LOG and b"stats" in p:
                last_stats = p.decode()
        if link.outstanding() + CHUNK <= OUTSTANDING_LIMIT:
            link.send_audio(tone)
            sent += CHUNK
        if time.monotonic() >= next_ping:
            link.send(T_PING, struct.pack("<I", pings))
            pings += 1
            next_ping += 0.02
    dt = time.monotonic() - t0
    link.send(T_SPK_FLUSH)
    link.send(T_SET_STATE, bytes([0]))
    print("device:", last_stats)
    print("stress %.1fs: rx %d B/s (mic payload %d B/s), tx %d B/s (spk payload %d B/s), crc errors %d, pings %d pongs %d"
          % (dt, (link.rx_bytes - rx0) / dt, mic_bytes / dt, (link.tx_bytes - tx0) / dt, sent / dt, link.crc_errors, pings, pongs))


def cmd_perf(link, args):
    """Silent mic plus speaker traffic while the UI runs a bench mode at a fixed angle; prints fps and audio counters."""
    link.send(T_SET_VOLUME, bytes([0]))
    link.send(T_SET_STATE, bytes([STATES[args.state]]))
    angle = "null" if args.angle is None else repr(float(args.angle))
    link.send(T_DEBUG, ('{"angle_override":%s,"bench":%d}' % (angle, args.mode)).encode())
    tone = bytes(CHUNK)
    fps, stats = [], ""
    mic_frames = 0
    t0 = time.monotonic()
    while time.monotonic() - t0 < args.seconds:
        for t, p in link.poll():
            if t == T_MIC:
                mic_frames += 1
            elif t == T_LOG and b"fps=" in p:
                fps.append(p.decode())
            elif t == T_LOG and b"stats" in p:
                stats = p.decode()
            elif t == T_LOG and b"cover miss" in p:
                print(p.decode())
        if link.outstanding() + CHUNK <= OUTSTANDING_LIMIT:
            link.send_audio(tone)
    dt = time.monotonic() - t0
    link.send(T_SPK_FLUSH)
    link.send(T_DEBUG, b'{"angle_override":null,"bench":0}')
    link.send(T_SET_STATE, bytes([0]))
    for line in fps[-3:]:
        print(line)
    print(stats)
    print("mic frames %d in %.1fs (expected %d), crc errors %d" % (mic_frames, dt, int(dt / 0.02), link.crc_errors))


def cmd_spkdrain(link, args):
    """Queue 2 s of tone and report SPK_STATUS samples to show the real-time drain."""
    tone = bytes(CHUNK)
    link.send(T_SET_VOLUME, bytes([0]))
    link.send(T_SPK_FLUSH)
    time.sleep(0.2)
    link.poll()
    t0 = time.monotonic()
    for _ in range(args.bytes // CHUNK):
        link.send_audio(tone)
    link.send(T_SPK_END)
    samples = []
    while time.monotonic() - t0 < args.bytes / 32000 + 2:
        for t, p in link.poll():
            if t == T_SPK_STATUS:
                q, fl, _c = struct.unpack("<IBI", p[:9])
                samples.append((time.monotonic() - t0, q, fl))
                if fl & 2:
                    break
        else:
            continue
        break
    for s in samples:
        print("t=%.2fs queued=%d flags=%d" % s)
    if len(samples) > 3:
        a, b = samples[1], samples[-2]
        if b[0] > a[0]:
            print("drain rate %.0f B/s (real time = 32000)" % ((a[1] - b[1]) / (b[0] - a[0])))


def cmd_angle(link, args):
    val = "null" if args.deg == "null" else repr(float(args.deg))
    link.send(T_DEBUG, ('{"angle_override":%s}' % val).encode())


IMU_DUMP_LINE = re.compile(rb"imu_dump i=(\d+) ([-\d,;]+)")
IMU_DUMP_DONE = re.compile(rb"imu_dump done n=(\d+)")
IMU_DUMP_BATCH = 5


def parse_imu_dump(payloads):
    """Return (rows, gaps) from LOG payloads; rows are (index, x, y, z) raw counts, gaps counts index discontinuities and a lost tail."""
    rows, gaps, expect_next, total = [], 0, 0, None
    for p in payloads:
        m = IMU_DUMP_LINE.search(p)
        if m:
            first = int(m.group(1))
            if first != expect_next:
                gaps += 1
            for k, triple in enumerate(m.group(2).decode().split(";")):
                x, y, z = (int(v) for v in triple.split(","))
                rows.append((first + k, x, y, z))
            expect_next = first + IMU_DUMP_BATCH
            continue
        m = IMU_DUMP_DONE.search(p)
        if m:
            total = int(m.group(1))
    if total is not None and total - expect_next >= IMU_DUMP_BATCH:
        gaps += 1
    return rows, gaps


def cmd_imu_dump(link, args):
    """Record raw accelerometer samples to a CSV file; a trace with an index gap is rejected and not written."""
    link.send(T_DEBUG, b'{"imu_dump":%d}' % args.seconds)
    payloads, done = [], False
    end = time.monotonic() + args.seconds + 5.0
    while time.monotonic() < end and not done:
        for t, p in link.poll():
            if t == T_LOG and b"imu_dump" in p:
                payloads.append(p)
                done = done or IMU_DUMP_DONE.search(p) is not None
    rows, gaps = parse_imu_dump(payloads)
    if not done or gaps or not rows:
        print("imu_dump rejected: done=%s gaps=%d samples=%d" % (done, gaps, len(rows)))
        sys.exit(1)
    with open(args.out, "w") as f:
        f.write("index,x,y,z\n")
        for r in rows:
            f.write("%d,%d,%d,%d\n" % r)
    print("imu_dump saved %d samples to %s" % (len(rows), args.out))


def cmd_disp(link, args):
    """Display power test aids: speed up the timeline, pin a stage, the ACTIVE brightness or an orbit index, fill the screen with a grey."""
    fields = []
    for key, val in (("disp_scale", args.scale), ("disp_force", args.force), ("orbit", args.orbit), ("fill", args.fill), ("arc", args.arc), ("bench", args.bench),
                     ("brt", args.brt), ("brt_arc", args.brt_arc), ("arc_over", args.arc_over), ("anim_clip", args.anim_clip)):
        if val is not None:
            fields.append('"%s":%s' % (key, val if val == "null" else int(val)))
    link.send(T_DEBUG, ("{%s}" % ",".join(fields)).encode())
    end = time.monotonic() + args.seconds
    sampler, samples, ping = AnimSampler(), [], Keepalive(link)
    while time.monotonic() < end:
        ping.tick()
        for t, p in link.poll():
            if t == T_LOG and (b"disp:" in p or b"fps=" in p or b"bright " in p or b"swipe:" in p or b"anim" in p):
                print(p.decode())
            s = sampler.feed(p) if t == T_LOG else None
            if s:
                samples.append(s)
    if len(samples) >= 2:
        a, b = samples[0], samples[-1]
        print("anim over %.1f s: frames +%d late +%d cuts +%d" % ((b["ms"] - a["ms"]) / 1000, u32_delta(a["frames"], b["frames"]),
                                                                  u32_delta(a["late"], b["late"]), u32_delta(a["cuts"], b["cuts"])))


def parse_bright(p):
    """Return the counters of a 'bright lvl=' LOG line as a dict, or None."""
    m = BRIGHT_LINE.search(p)
    return dict(zip(BRIGHT_KEYS, (int(v) for v in m.groups()))) if m else None


def parse_batt(p):
    """Return the fields of a 'batt pct=' LOG line as a dict (look as str), {"look": "stale"} for 'batt: stale', or None."""
    if BATT_STALE.search(p):
        return {"look": "stale"}
    m = BATT_LINE.search(p)
    return dict(zip(BATT_KEYS, (int(v) for v in m.groups()[:-1])), look=m.group(len(BATT_KEYS) + 1).decode()) if m else None


def board_ms(p):
    """Return the board uptime in ms from the leading 'I (<ms>) ' of a LOG line, or None."""
    m = BOARD_MS.search(p)
    return int(m.group(1)) if m else None


def final_by_board(ms, first_ms, window_s):
    """True when a bright line's board time lies at or after the baseline's board time plus the window and the guard."""
    return ms is not None and first_ms is not None and ms >= first_ms + (window_s + SWIPE_FINAL_GUARD_S) * 1000


def parse_swipe(p):
    """Return the fields of a 'swipe: end' LOG line as a dict (numbers as int or float), or None."""
    m = SWIPE_LINE.search(p)
    if not m:
        return None
    out = {}
    for kv in m.group(1).decode("utf-8", "replace").split():
        k, _, v = kv.partition("=")
        try:
            out[k] = int(v)
        except ValueError:
            try:
                out[k] = float(v)
            except ValueError:
                out[k] = v
    return out


class Keepalive:
    """Sends a PING every KEEPALIVE_S so the board does not go OFFLINE during a long wait."""

    def __init__(self, link):
        self.link, self.last = link, time.monotonic()

    def tick(self):
        if time.monotonic() - self.last >= KEEPALIVE_S:
            self.link.send(T_PING, b"\0\0\0\0")
            self.last = time.monotonic()


def parse_kv(text):
    """Return key=value pairs as a dict: 'a/b' values as tuples of ints, other numbers as int; terminal colour codes are dropped."""
    out = {}
    for kv in ANSI_CODE.sub("", text).split():
        k, _, v = kv.partition("=")
        try:
            out[k] = tuple(int(x) for x in v.split("/")) if "/" in v else int(v)
        except ValueError:
            out[k] = v
    return out


def numeric(v):
    """True for an int or a tuple of ints, as parse_kv returns numbers."""
    return isinstance(v, int) or (isinstance(v, tuple) and len(v) > 0)


class AnimSampler:
    """Groups the stats, 'anim:' and 'animt:' lines of one log period; a group missing a line is dropped.

    anim and animt match by id; stats belongs to the anim line within STATS_PAIR_MS of board time after it."""

    def __init__(self):
        self.pending = None
        self.stats = None

    def feed(self, p):
        st = parse_stats(p)
        if st is not None:
            self.stats = (board_ms(p), st)
            return None
        m = re.search(rb"\banim: (pack=.*)", p)
        if m:
            d = parse_kv(m.group(1).decode("utf-8", "replace"))
            d["ms"], (st_ms, st), self.stats = board_ms(p), self.stats or (None, None), None
            ok = all(numeric(d.get(k)) for k in ANIM_KEYS) and d["ms"] is not None and st_ms is not None
            self.pending = dict(d, stats=st) if ok and 0 <= d["ms"] - st_ms < STATS_PAIR_MS else None
            return None
        m = re.search(rb"\banimt: (dec_us=.*)", p)
        if not m or self.pending is None:
            return None
        d = parse_kv(m.group(1).decode("utf-8", "replace"))
        sample, self.pending = self.pending, None
        if not all(numeric(d.get(k)) for k in ANIMT_KEYS) or d["id"] != sample["id"]:
            return None
        sample.update(d)
        return sample


def parse_stats(p):
    """Return the audio counters of a 'stats rx=' LOG line as a dict, or None."""
    m = re.search(rb"\bstats (rx=.*)", p)
    if not m:
        return None
    d = parse_kv(m.group(1).decode("utf-8", "replace"))
    return {k: d[k] for k in AUDIO_KEYS} if all(isinstance(d.get(k), int) for k in AUDIO_KEYS) else None


def u32_delta(a, b):
    """Difference of two cumulative u32 counters, safe across one wrap."""
    return (b - a) & 0xFFFFFFFF


def anim_report(a, b, sa, sb):
    """Return (lines, failed) for two anim samples and two stats dicts; rules without a fixed limit print raw values only."""
    lines, failed = [], False
    secs = (b["ms"] - a["ms"]) / 1000 if a.get("ms") is not None and b.get("ms") is not None else 0
    frames = u32_delta(a["frames"], b["frames"])
    played = frames > 0 and all(s["skin"] == 1 and s["clip"] >= 0 for s in (a, b))
    failed |= not played
    lines.append("G0 %s sticker clip %d (skin=%d) frames +%d%s" % ("PASS" if played else "FAIL", b["clip"], b["skin"], frames,
                                                                "" if played else "; no sticker played, so this clip cannot be used for the gate"))
    late = u32_delta(a["late"], b["late"])
    failed |= late != 0
    lines.append("G1 %s late +%d; frames +%d in %.1f s (%.2f/s)" % ("PASS" if late == 0 else "FAIL", late, frames, secs, frames / secs if secs else 0))
    work = u32_delta(a["work_us"][0], b["work_us"][0])
    dec = u32_delta(a["dec_us"][0], b["dec_us"][0])
    lines.append("G2 work mean %d us, worst so far %d us; decode mean %d us, worst so far %d us; limits come from the pack's frame durations"
                 % (work // frames if frames else 0, b["work_us"][1], dec // frames if frames else 0, b["dec_us"][1]))
    lines.append("G2 boot frame %d ms; patch_us over the last window: mean %d, worst so far %d; ring updates deferred in it: %d"
                 % (b["boot_ms"], b["patch_us"][0], b["patch_us"][1], b["ring_defer"]))
    if sa is None or sb is None:
        failed = True
        lines.append("G3 FAIL no stats line before or after")
    else:
        d = {k: u32_delta(sa[k], sb[k]) for k in AUDIO_KEYS}
        bad = any(d.values())
        failed |= bad
        lines.append("G3 %s %s" % ("FAIL" if bad else "PASS", " ".join("%s +%d" % (k, d[k]) for k in AUDIO_KEYS)))
    lines.append("G4 cut_ms last %d worst %d; cuts +%d" % (b["cut_ms"][0], b["cut_ms"][1], u32_delta(a["cuts"], b["cuts"])))
    dfree = b["iheap"][0] - a["iheap"][0]
    heap_ok = dfree >= -GATE_HEAP_DROP_MAX and b["ilarge"] >= GATE_HEAP_LARGEST_MIN
    failed |= not heap_ok
    lines.append("G5 %s over this run: internal free %d -> %d (%+d), lowest %d, largest block %d"
                 % ("PASS" if heap_ok else "FAIL", a["iheap"][0], b["iheap"][0], dfree, b["iheap"][1], b["ilarge"]))
    lines.append("G6 not measured here; run 'bench 1' and read fps=")
    verify_ok = b["pack"] == 1 and b["verify_ms"] <= GATE_VERIFY_MS_MAX
    failed |= not verify_ok
    lines.append("G7 %s pack=%d verify_ms=%d" % ("PASS" if verify_ok else "FAIL", b["pack"], b["verify_ms"]))
    lines.append("G8 not measured here; run 'disp --force 2' and 'disp --force 1', each prints its frames delta")
    return lines, failed


def wait_sample(link, timeout, after=None):
    """Return (anim sample, its period's stats dict) for the first complete group arriving after the host time 'after', or (None, None)."""
    end = time.monotonic() + timeout
    sampler, ping = AnimSampler(), Keepalive(link)
    while time.monotonic() < end:
        ping.tick()
        for t, p in link.poll():
            s = sampler.feed(p) if t == T_LOG else None
            if s and (after is None or time.monotonic() > after):
                return s, s.pop("stats")
    return None, None


def gate_volume(text):
    """argparse type: a speech volume no louder than the gate rule allows."""
    v = int(text)
    if not 0 <= v <= GATE_VOLUME_MAX:
        raise argparse.ArgumentTypeError("volume must be 0-%d for a gate run" % GATE_VOLUME_MAX)
    return v


def pcm16k(path):
    """argparse type: the PCM of a 16 kHz mono 16-bit WAV file."""
    try:
        w = wave.open(path, "rb")
    except (OSError, wave.Error) as e:
        raise argparse.ArgumentTypeError("cannot read %s: %s" % (path, e))
    with w:
        if w.getframerate() != 16000 or w.getnchannels() != 1 or w.getsampwidth() != 2:
            raise argparse.ArgumentTypeError("%s is not 16 kHz mono s16" % path)
        return w.readframes(w.getnframes())


def knob_eff(p):
    """Return the effective volume of a 'knob vol=' LOG line, or None."""
    m = re.search(rb"knob vol=\d+ eff=(\d+)", p)
    return int(m.group(1)) if m else None


def wait_eff(link, vol):
    """True once a knob LOG line shows the effective volume vol within EFF_WAIT_S."""
    end, ping = time.monotonic() + EFF_WAIT_S, Keepalive(link)
    while time.monotonic() < end:
        ping.tick()
        if any(t == T_LOG and knob_eff(p) == vol for t, p in link.poll()):
            return True
    return False


def gate_run(link, args):
    """The measured part of gate-anim; returns True when a judged rule failed or a step could not run."""
    link.send(T_SET_STATE, bytes([STATES["speaking"]]))
    link.send(T_SET_VOLUME, bytes([args.volume, 0]))
    if not wait_eff(link, args.volume):
        print("gate-anim: the board did not confirm volume %d; not streaming" % args.volume)
        return True
    a, sa = wait_sample(link, GATE_SAMPLE_WAIT_S)
    if a is None:
        print("gate-anim: no anim and stats lines before the run")
        return True
    if not stream_pcm(link, args.wav):
        print("gate-anim: playback stalled or was not confirmed")
        return True
    b, sb = wait_sample(link, GATE_SAMPLE_WAIT_S, after=time.monotonic())
    if b is None:
        print("gate-anim: no anim and stats lines after the run")
        return True
    lines, failed = anim_report(a, b, sa, sb)
    for line in lines:
        print(line)
    return failed


def cmd_gate_anim(link, args):
    """Gate 0 speech run: SPEAKING at a capped transient volume, stream the WAV, end the override, print the anim rule deltas."""
    try:
        failed = gate_run(link, args)
    finally:
        ended = end_override(link)
    if failed or not ended:
        sys.exit(1)


def end_override(link):
    """Flush queued audio, end the transient volume with HOST_HELLO and go IDLE; True once eff equals the reply's vol."""
    link.send(T_SPK_FLUSH)
    link.wait_for(T_SPK_STATUS, 1.0, show=False)
    link.hellos.clear()
    link.send(T_HOST_HELLO, b'{"proto":%d}' % PROTO)
    link.wait_for(T_HELLO, 2.0, show=False)
    link.send(T_SET_STATE, bytes([STATES["idle"]]))
    vol = link.hellos[-1].get("vol") if link.hellos else None
    if not isinstance(vol, int) or not wait_eff(link, vol):
        print("gate-anim: the end of the transient volume was not confirmed (HELLO vol %r)" % vol)
        return False
    print("gate-anim: transient volume ended; effective volume %d" % vol)
    return True


def brt_snap(pct):
    """The ladder level the board stores for pct, as brt_snap in firmware/main/brtmap.c (ties go to the lower level)."""
    ladder = [round(BRT_MIN_PERCENT * (100 / BRT_MIN_PERCENT) ** (i / BRT_STEPS)) for i in range(BRT_STEPS + 1)]
    pct = min(max(pct, BRT_MIN_PERCENT), 100)
    return min(ladder, key=lambda level: (abs(level - pct), level))


def wait_bright(link, timeout, pred=None):
    """Return the next 'bright lvl=' LOG payload whose counters satisfy pred, or None after timeout seconds."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for t, p in link.poll():
            b = parse_bright(p) if t == T_LOG else None
            if b and (pred is None or pred(b)):
                return p
    return None


def cmd_brightness(link, args):
    """Set and store the ACTIVE brightness (snapped to the ladder) as a swipe would, then print the board's first brightness line showing it."""
    want = None
    if args.value is not None:
        want = brt_snap(args.value)
        link.send(T_DEBUG, b'{"brt_set":%d}' % args.value)
    p = wait_bright(link, BRIGHT_WAIT_S, None if want is None else (lambda b: b["lvl"] == want))
    if p is None:
        print("brightness: no 'bright lvl=%s' line within %.0f s; the firmware may not have the brightness swipe, or a swipe was armed"
              % ("" if want is None else want, BRIGHT_WAIT_S))
        sys.exit(1)
    print(p.decode("utf-8", "replace"))


def swipe_report(label, first, last, swipes):
    """Return (lines, passed) for a swipecheck run between the baseline and the final bright line; grip and wipe pass only when nothing armed."""
    if first is None or last is None:
        return ["swipecheck %s: no %s 'bright lvl=' line; FAIL" % (label, "baseline" if first is None else "final")], False
    d = {k: last[k] - first[k] for k in BRIGHT_COUNTERS}
    lines = ["swipecheck %s: arms +%d rim +%d cancels +%d slides +%d stops +%d lvl %d -> %d"
             % (label, d["arms"], d["rim"], d["cancels"], d["slides"], d["stops"], first["lvl"], last["lvl"])]
    unarmed = [s for s in swipes if not s.get("arm")]
    armed = [s for s in swipes if s.get("arm")]
    by_why = {}
    for s in unarmed:
        by_why[s.get("why")] = by_why.get(s.get("why"), 0) + 1
    lines.append("unarmed %d by why %s" % (len(unarmed), json.dumps(by_why, sort_keys=True)))
    if unarmed:
        top = max(unarmed, key=lambda s: s.get("pk", 0))
        lines.append("largest unarmed pk %s lat %s cls %s ku %s kl %s" % tuple(top.get(k) for k in ("pk", "lat", "cls", "ku", "kl")))
    for s in armed:
        lines.append("armed cls %s pk %s lat %s ku %s kl %s arm_ms %s gap %s stp %s rev %s r1 %s b0 %s b %s"
                     % tuple(s.get(k) for k in ("cls", "pk", "lat", "ku", "kl", "arm_ms", "gap", "stp", "rev", "r1", "b0", "b")))
    back = [k for k in BRIGHT_COUNTERS if d[k] < 0]
    if back:
        lines.append("counters went backwards (%s): the board restarted" % " ".join(back))
        passed = False
    elif label == "free":
        passed = True
    else:
        passed = d["arms"] == 0 and first["lvl"] == last["lvl"] and not armed
    lines.append("swipecheck %s %s" % (label, "PASS" if passed else "FAIL"))
    return lines, passed


def drain(link):
    """Read and discard everything the link holds until a poll returns nothing, at most BRIGHT_WAIT_S."""
    end = time.monotonic() + BRIGHT_WAIT_S
    while link.poll() and time.monotonic() < end:
        pass


def cmd_swipecheck(link, args, is_final=final_by_board):
    """Hold the screen ACTIVE for SECONDS between a baseline and a final bright line and judge the swipes in between by label; only lines after the board answers a marker ping sent behind the pin count."""
    first = last = first_ms = None
    swipes = []
    marker = time.monotonic_ns() & 0xFFFFFFFF
    seq, marked = marker, False
    drain(link)
    link.send(T_DEBUG, b'{"disp_force":0}')
    link.send(T_PING, struct.pack("<I", marker))
    next_ping = time.monotonic() + 1.0
    try:
        end = time.monotonic() + BRIGHT_WAIT_S
        while last is None and time.monotonic() < end:
            if time.monotonic() >= next_ping:
                seq = (seq + 1) & 0xFFFFFFFF
                link.send(T_PING, struct.pack("<I", seq))
                next_ping = time.monotonic() + 1.0
            for t, p in link.poll():
                if not marked:
                    if t == T_PONG and len(p) == 4 and struct.unpack("<I", p)[0] == marker:
                        marked = True
                        end = time.monotonic() + BRIGHT_WAIT_S
                    continue
                if t != T_LOG or last is not None:
                    continue
                b, s = parse_bright(p), parse_swipe(p)
                if b or s:
                    print(p.decode("utf-8", "replace"))
                if first is None:
                    if b:
                        first, first_ms = b, board_ms(p)
                        end = time.monotonic() + args.seconds + SWIPE_FINAL_GUARD_S + BRIGHT_WAIT_S
                else:
                    if s:
                        swipes.append(s)
                    if b and is_final(board_ms(p), first_ms, args.seconds):
                        last = b
    finally:
        link.send(T_DEBUG, b'{"disp_force":null}')
    if not marked:
        print("swipecheck %s: the board did not answer the marker ping within %.0f s; FAIL" % (args.label, BRIGHT_WAIT_S))
        sys.exit(1)
    lines, passed = swipe_report(args.label, first, last, swipes)
    print("\n".join(lines))
    if not passed:
        sys.exit(1)


def cmd_bench(link, args):
    """Run a UI frame-rate bench mode (1 spin, 2 redraw, 3 canvas, 4 screen) and print the device fps log lines."""
    link.send(T_SET_STATE, bytes([STATES.get(args.state, 0)]))
    link.send(T_DEBUG, b'{"bench":%d}' % args.mode)
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        for t, p in link.poll():
            if t == T_LOG and (b"fps=" in p or b"stats" in p):
                print(p.decode())
    link.send(T_DEBUG, b'{"bench":0}')


def cmd_snapburst(link, args):
    """Send N SNAPSHOT requests spaced by the interval, then collect the images in order."""
    frames = []
    for i in range(args.count):
        link.send(T_SNAPSHOT)
        t_end = time.monotonic() + args.interval
        while time.monotonic() < t_end:
            frames += link.poll()
    got = 0
    data = bytearray()
    info = None
    end = time.monotonic() + 8.0 + args.count * 3.0
    while got < args.count and time.monotonic() < end:
        for t, p in frames:
            if t == T_SNAP_BEGIN:
                data = bytearray()
                info = json.loads(p)
            elif t == T_SNAP_DATA:
                data += p
            elif t == T_SNAP_END and info and len(data) == info["w"] * info["h"] * 2:
                write_png("%s-%d.png" % (args.prefix, got), info["w"], info["h"], rgb565_to_rgb(bytes(data)))
                got += 1
        frames = link.poll()
    print("snapburst: %d/%d images (crc errors %d)" % (got, args.count, link.crc_errors))


def expect(name, ok, detail=""):
    print("%s %s %s" % ("PASS" if ok else "FAIL", name, detail))
    return ok


def cmd_v3check(link, args):
    """Silent protocol v3 checks: HELLO fields including the volume, consumed counter, flush, resume, proto refusal, PAIR refusal, FORGET."""
    ok = True
    h = link.hellos[-1] if link.hellos else {}
    paired = h.get("paired")
    ok &= expect("hello fields", h.get("proto") == PROTO and h.get("link") == "usb" and isinstance(paired, bool)
                 and h.get("store") in ("ok", "error")
                 and h.get("reason") in ("boot", "reply") and isinstance(h.get("vol"), int) and isinstance(h.get("vol_set"), bool),
                 json.dumps(h))
    vol0 = h.get("vol")
    link.send(T_SET_VOLUME, bytes([0]))
    pcm = bytes(64000)
    stream_pcm(link, pcm, tail=0.2)
    ok &= expect("consumed == sent after playback", (link.spk_consumed - link.spk_base) & 0xFFFFFFFF == link.spk_sent,
                 "sent %d consumed %d" % (link.spk_sent, (link.spk_consumed - link.spk_base) & 0xFFFFFFFF))
    for _ in range(12):
        link.send_audio(bytes(CHUNK))
    link.send(T_SPK_END)
    time.sleep(0.15)
    link.poll()
    link.hellos.clear()
    link.spk_flags = 0xFF
    link.send(T_SPK_FLUSH)
    p = link.wait_for(T_SPK_STATUS, 0.5, show=False)
    ok &= expect("flush status immediate", p is not None and link.spk_queued == 0 and link.spk_flags == 0,
                 "queued %d flags %d" % (link.spk_queued, link.spk_flags))
    time.sleep(0.3)
    ok &= expect("consumed == sent after flush", (link.spk_consumed - link.spk_base) & 0xFFFFFFFF == link.spk_sent,
                 "sent %d consumed %d" % (link.spk_sent, (link.spk_consumed - link.spk_base) & 0xFFFFFFFF))
    drained = False
    end = time.monotonic() + 1.0
    while time.monotonic() < end:
        for t, p in link.poll():
            drained |= t == T_SPK_STATUS and bool(p[4] & 2)
    ok &= expect("flush cancels pending SPK_END", not drained)
    link.send(T_HOST_HELLO, b'{"proto":%d}' % PROTO)
    got_hello = link.wait_for(T_HELLO, 1.0, show=False) is not None
    status = link.wait_for(T_WIFI_STATUS, 1.0, show=False)
    if paired:
        print("WIFI_STATUS", status.decode() if status else None)
    ok &= expect("HELLO reply + WIFI_STATUS", got_hello and status is not None
                 and (paired or status == b'{"wifi":"off","relay":"none"}')
                 and link.hellos[-1]["reason"] == "reply", json.dumps(link.hellos[-1:]))
    ok &= expect("transient 0 left the stored volume unchanged", bool(link.hellos) and link.hellos[-1].get("vol") == vol0,
                 "vol %r then %r" % (vol0, link.hellos[-1:] and link.hellos[-1].get("vol")))
    link.send(T_SET_STATE, bytes([5]))
    time.sleep(0.2)
    link.hellos.clear()
    link.send(T_SNAPSHOT)
    snap_done = link.wait_for(T_SNAP_END, 10.0, show=False) is not None
    ok &= expect("SNAPSHOT does not leave OFFLINE", snap_done and not link.hellos)
    link.send(T_PING, b"\0\0\0\0")
    link.wait_for(T_HELLO, 1.0, show=False)
    ok &= expect("resume HELLO", bool(link.hellos) and link.hellos[-1]["reason"] == "resume", json.dumps(link.hellos[-1:]))
    link.hellos.clear()
    link.send(T_HOST_HELLO, b'{"proto":1}')
    ok &= expect("proto mismatch ignored", link.wait_for(T_HELLO, 1.0, show=False) is None)
    link.send(T_PAIR, b"{}")
    p = link.wait_for(T_PAIR_RESULT, 1.0, show=False)
    r = json.loads(p) if p else {}
    ok &= expect("PAIR {} refused", r.get("ok") is False and isinstance(r.get("error"), str), p.decode() if p else "no reply")
    ok &= forget_check(link, h)
    print("v3check", "PASS" if ok else "FAIL")


def forget_check(link, hello):
    """FORGET only a board whose HELLO says it holds no usable pairing; any other board keeps its pairing."""
    if hello.get("paired") is False and hello.get("store") == "ok":
        link.send(T_FORGET)
        return expect("FORGET while unpaired", link.wait_for(T_FORGET_RESULT, 1.0, show=False) == b'{"ok":true}')
    print("SKIP FORGET while unpaired: paired=%s store=%s" % (json.dumps(hello.get("paired")), json.dumps(hello.get("store"))))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--serial", help="USB serial number; default env CLAWD_SERIAL or the single connected board")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("listen"); s.add_argument("seconds", type=float, nargs="?", default=5); s.set_defaults(fn=cmd_listen)
    s = sub.add_parser("ping"); s.add_argument("count", type=int, nargs="?", default=20); s.set_defaults(fn=cmd_ping)
    s = sub.add_parser("hello"); s.add_argument("--proto", type=int, default=PROTO); s.set_defaults(fn=cmd_hello)
    s = sub.add_parser("state"); s.add_argument("state"); s.set_defaults(fn=cmd_state)
    s = sub.add_parser("volume"); s.add_argument("value", type=int); s.add_argument("--save", action="store_true"); s.set_defaults(fn=cmd_volume)
    s = sub.add_parser("selftest"); s.add_argument("--volume", type=int, default=0); s.set_defaults(fn=cmd_selftest)
    s = sub.add_parser("snapshot"); s.add_argument("out"); s.set_defaults(fn=cmd_snapshot)
    s = sub.add_parser("snapshots"); s.add_argument("outdir"); s.set_defaults(fn=cmd_snapshots)
    s = sub.add_parser("play"); s.add_argument("wav"); s.set_defaults(fn=cmd_play)
    s = sub.add_parser("record"); s.add_argument("out"); s.add_argument("seconds", type=float); s.set_defaults(fn=cmd_record)
    s = sub.add_parser("loopback"); s.add_argument("wav"); s.add_argument("out"); s.add_argument("--tail", type=float, default=1.0); s.add_argument("--volume", type=int, default=30); s.set_defaults(fn=cmd_loopback)
    s = sub.add_parser("stress"); s.add_argument("seconds", type=float, nargs="?", default=30); s.set_defaults(fn=cmd_stress)
    s = sub.add_parser("spkdrain"); s.add_argument("bytes", type=int, nargs="?", default=64000); s.set_defaults(fn=cmd_spkdrain)
    s = sub.add_parser("angle"); s.add_argument("deg"); s.set_defaults(fn=cmd_angle)
    s = sub.add_parser("snapburst"); s.add_argument("prefix"); s.add_argument("count", type=int); s.add_argument("interval", type=float); s.set_defaults(fn=cmd_snapburst)
    s = sub.add_parser("bench"); s.add_argument("mode", type=int); s.add_argument("state", nargs="?", default="idle"); s.add_argument("seconds", type=float, nargs="?", default=8); s.set_defaults(fn=cmd_bench)
    s = sub.add_parser("perf"); s.add_argument("state"); s.add_argument("mode", type=int); s.add_argument("--angle", type=float); s.add_argument("--seconds", type=float, default=10); s.set_defaults(fn=cmd_perf)
    s = sub.add_parser("imu-dump"); s.add_argument("seconds", type=int); s.add_argument("out"); s.set_defaults(fn=cmd_imu_dump)
    s = sub.add_parser("disp"); s.add_argument("--scale"); s.add_argument("--force"); s.add_argument("--orbit"); s.add_argument("--fill"); s.add_argument("--arc"); s.add_argument("--bench"); s.add_argument("--brt"); s.add_argument("--brt-arc", dest="brt_arc"); s.add_argument("--arc-over", dest="arc_over"); s.add_argument("--anim-clip", dest="anim_clip"); s.add_argument("--seconds", type=float, default=10); s.set_defaults(fn=cmd_disp)
    s = sub.add_parser("brightness"); s.add_argument("value", type=int, nargs="?"); s.set_defaults(fn=cmd_brightness)
    s = sub.add_parser("swipecheck"); s.add_argument("seconds", type=float); s.add_argument("--label", choices=("grip", "wipe", "free"), required=True); s.set_defaults(fn=cmd_swipecheck)
    s = sub.add_parser("v3check"); s.set_defaults(fn=cmd_v3check)
    s = sub.add_parser("gate-anim"); s.add_argument("--wav", type=pcm16k, required=True); s.add_argument("--volume", type=gate_volume, required=True); s.set_defaults(fn=cmd_gate_anim)
    args = ap.parse_args()
    link = Link(args.port, args.serial)
    try:
        if args.cmd != "listen":
            link.connect()
        args.fn(link, args)
    finally:
        link.ser.close()


if __name__ == "__main__":
    main()
