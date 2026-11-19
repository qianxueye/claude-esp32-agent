#!/usr/bin/env python3
"""Builds, inspects, verifies and decodes Clawd animation packs; the format is the one firmware/main/apack.h reads."""

import argparse
import re
import struct
import sys
import tomllib
import zlib
from pathlib import Path

MAGIC = b"CLWDANIM"
VERSION = 1
HEADER = 48
CLIP_ENTRY = 24
FRAME_ENTRY = 8
CODEC_DELTA = 1
KEY = 0x8000
MS_MASK = 0x7FFF
NO_CLIP = 0xFF
STATE_SLOTS = 8
EVENT_SLOTS = 4
MAX_CLIPS = 16
MAX_COLORS = 250
CANVAS_MAX = 240
SAFE_UP = 92
SAFE_DOWN = 96
STATES = ("idle", "listening", "thinking", "speaking", "error", "offline")
EVENTS = ("pickup",)
HEAD_FMT = "<8sHBBHHIII8s4sI"
CLIP_FMT = "<HHHHHHIIBBH"


class PackError(Exception):
    """A mapping, an input file or a pack that cannot be used."""


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def layout_ok(canvas_w, canvas_h, scale, x, y, w, h):
    """Mirror of ap_layout_ok in apack.c."""
    if scale not in (1, 2) or canvas_w == 0 or canvas_h == 0 or w == 0 or h == 0:
        return False
    cw, ch = canvas_w * scale, canvas_h * scale
    if cw > CANVAS_MAX or ch > CANVAS_MAX or x + w > canvas_w or y + h > canvas_h:
        return False
    return y * scale - ch // 2 >= -SAFE_UP and (y + h) * scale - ch // 2 <= SAFE_DOWN


# Frame tokens


def _skip(n):
    return bytes([n - 1]) if n <= 127 else bytes([0x7F]) + struct.pack("<H", n - 128)


def _literal(data):
    out = bytearray()
    for i in range(0, len(data), 64):
        chunk = data[i : i + 64]
        out += bytes([0x7F + len(chunk)]) + chunk
    return bytes(out)


def _fill(value, run):
    out = bytearray()
    while run > 0:
        take = min(run, 65)
        if run - take == 1:
            take -= 1
        out += bytes([0xBE + take, value])
        run -= take
    return bytes(out)


def _stretch(values):
    out, lit, k = bytearray(), bytearray(), 0
    while k < len(values):
        r = 1
        while k + r < len(values) and values[k + r] == values[k]:
            r += 1
        if r >= 3:
            out += _literal(bytes(lit))
            lit.clear()
            out += _fill(values[k], r)
        else:
            lit += values[k : k + r]
        k += r
    return bytes(out + _literal(bytes(lit)))


def encode_frame(prev, cur):
    """Tokens that turn prev into cur; unchanged runs of 3+ are skipped, shorter ones join the changed stretch."""
    n, out, i, start = len(cur), bytearray(), 0, None
    while i < n:
        j = i
        if cur[i] == prev[i]:
            while j < n and cur[j] == prev[j]:
                j += 1
            if j == n:
                break
            if j - i >= 3:
                if start is not None:
                    out += _stretch(cur[start:i])
                    start = None
                out += _skip(j - i)
            elif start is None:
                start = i
        else:
            while j < n and cur[j] != prev[j]:
                j += 1
            if start is None:
                start = i
        i = j
    if start is not None:
        out += _stretch(cur[start:i])
    return bytes(out)


def decode_frame(data, buf, key):
    """Mirror of ap_decode; raises PackError where the C decoder returns false."""
    if key:
        buf[:] = bytes(len(buf))
    area, s, p = len(buf), 0, 0
    while s < len(data):
        c = data[s]
        s += 1
        if c < 0x7F:
            n = c + 1
        elif c == 0x7F:
            if len(data) - s < 2:
                raise PackError("frame ends inside a SKIP length")
            n = 128 + data[s] + (data[s + 1] << 8)
            s += 2
        elif c < 0xC0:
            n = c - 0x7F
            if n > len(data) - s or n > area - p:
                raise PackError("LIT runs past the frame or the buffer")
            buf[p : p + n] = data[s : s + n]
            s += n
            p += n
            continue
        else:
            if s >= len(data):
                raise PackError("frame ends inside a FILL")
            n, v = c - 0xBE, data[s]
            s += 1
            if n > area - p:
                raise PackError("FILL runs past the buffer")
            buf[p : p + n] = bytes([v]) * n
            p += n
            continue
        if n > area - p:
            raise PackError("SKIP runs past the buffer")
        p += n


# Input


def _composite(im):
    from PIL import Image

    rgba = im.convert("RGBA")
    black = Image.new("RGBA", rgba.size, (0, 0, 0, 255))
    return Image.alpha_composite(black, rgba).convert("RGB")


def read_clip(base, spec, canvas):
    """Frames as RGB on black and their durations in ms, from an animated file or a frame sheet."""
    from PIL import Image

    if "file" not in spec:
        raise PackError("a clip has no file")
    path = (base / spec["file"]).resolve()
    if not path.is_file():
        raise PackError(f"missing file: {path}")
    frames, durations = [], []
    with Image.open(path) as im:
        sheet = spec.get("sheet")
        if sheet:
            cell, cols, count = int(sheet["cell"]), int(sheet["columns"]), int(sheet["frames"])
            if cell <= 0 or cols <= 0 or count <= 0:
                raise PackError("sheet cell, columns and frames must be positive")
            rows = (count + cols - 1) // cols
            if im.size[0] < cols * cell or im.size[1] < rows * cell:
                raise PackError(f"sheet smaller than {cols} x {rows} cells: {path}")
            whole = im.convert("RGBA")
            frames = [whole.crop((i % cols * cell, i // cols * cell, i % cols * cell + cell, i // cols * cell + cell)) for i in range(count)]
            durations = [0] * count
        else:
            for i in range(getattr(im, "n_frames", 1)):
                im.seek(i)
                frames.append(im.convert("RGBA"))
                durations.append(int(im.info.get("duration", 0) or 0))
    pattern = spec.get("frame_ms")
    if pattern:
        durations = [int(pattern[i % len(pattern)]) for i in range(len(frames))]
    if any(d <= 0 or d > MS_MASK for d in durations):
        raise PackError(f"frame durations must be 1-{MS_MASK} ms; set frame_ms: {path}")
    out = []
    for f in frames:
        if f.size != (canvas, canvas):
            raise PackError(f"frame size {f.size} is not the canvas {canvas}: {path}")
        out.append(_composite(f))
    return out, durations


def thin(frames, durations, k):
    """Keeps every k-th frame; each kept frame lasts as long as the frames it replaces."""
    if k < 1:
        raise PackError("--fps-div must be at least 1")
    return frames[::k], [sum(durations[i : i + k]) for i in range(0, len(frames), k)]


def settle(qb, raw, entries):
    """Palette and index plane from a quantiser's indices qb over RGB bytes raw, with black owning index 0 alone."""
    used = sorted(set(qb))
    darkest = min(used, key=lambda i: (sum(entries[i]), i))
    order = [i for i in used if i != darkest]
    if not order:
        raise PackError("a clip has no visible pixel")
    remap = bytearray(256)
    for new, old in enumerate(order, start=1):
        remap[old] = new
    palette = [tuple(entries[i]) for i in order]
    moved, nearest = bytearray(qb.translate(bytes(remap))), {}
    # The darkest entry becomes black, so its non-black pixels move to the closest kept colour.
    for m in re.finditer(re.escape(bytes([darkest])), qb):
        px = raw[3 * m.start() : 3 * m.start() + 3]
        if px != b"\0\0\0":
            if px not in nearest:
                nearest[px] = 1 + min(range(len(palette)), key=lambda k: (sum((a - b) ** 2 for a, b in zip(px, palette[k])), k))
            moved[m.start()] = nearest[px]
    n = len(qb)
    lit = int.from_bytes(raw[0::3], "big") | int.from_bytes(raw[1::3], "big") | int.from_bytes(raw[2::3], "big")
    keep = lit.to_bytes(n, "big").translate(bytes([0] + [255] * 255))
    return palette, (int.from_bytes(moved, "big") & int.from_bytes(keep, "big")).to_bytes(n, "big")


def index_clip(frames, colors):
    """Palette (RGB, index 1 up) and one index plane per frame; pure black is always index 0."""
    from PIL import Image

    w, h = frames[0].size
    mosaic = Image.new("RGB", (w, h * len(frames)))
    for i, f in enumerate(frames):
        mosaic.paste(f, (0, i * h))
    raw = mosaic.tobytes()
    found = mosaic.getcolors(maxcolors=colors + 1)
    if found is not None and sum(1 for _, c in found if c != (0, 0, 0)) <= colors:
        palette = sorted(c for _, c in found if c != (0, 0, 0))
        lut = {bytes(c): i + 1 for i, c in enumerate(palette)}
        lut[b"\0\0\0"] = 0
        plane = bytes(lut[raw[j : j + 3]] for j in range(0, len(raw), 3))
    else:
        q = mosaic.quantize(colors=colors + 1, method=Image.Quantize.MEDIANCUT, kmeans=2, dither=Image.Dither.NONE)
        pal = q.getpalette()
        palette, plane = settle(q.tobytes(), raw, [tuple(pal[i : i + 3]) for i in range(0, len(pal), 3)])
    return palette, [plane[i * w * h : (i + 1) * w * h] for i in range(len(frames))]


def crop_box(planes, w, h):
    """Union box (x, y, w, h) of all non-zero indices."""
    x0, y0, x1, y1 = w, h, 0, 0
    for p in planes:
        for y in range(h):
            row = p[y * w : (y + 1) * w]
            if any(row):
                y0, y1 = min(y0, y), max(y1, y + 1)
                x0 = min(x0, next(i for i, v in enumerate(row) if v))
                x1 = max(x1, w - next(i for i, v in enumerate(reversed(row)) if v))
    if x1 <= x0:
        raise PackError("a clip has no visible pixel")
    return x0, y0, x1 - x0, y1 - y0


def _clip_from_frames(frames, durations, colors, canvas_px, scale):
    from PIL import Image

    if scale == 2:
        frames = [f.resize((canvas_px // 2, canvas_px // 2), Image.Resampling.BOX) for f in frames]
    if any(d > MS_MASK for d in durations):
        raise PackError(f"a frame lasts more than {MS_MASK} ms")
    side = canvas_px // scale
    palette, planes = index_clip(frames, colors)
    x, y, w, h = crop_box(planes, side, side)
    if not layout_ok(side, side, scale, x, y, w, h):
        raise PackError(f"content {x},{y} {w}x{h} lies outside the safe box ({SAFE_UP} px up, {SAFE_DOWN} px down)")
    cropped = [b"".join(p[(y + r) * side + x : (y + r) * side + x + w] for r in range(h)) for p in planes]
    data, prev = [], bytes(w * h)
    for i, cur in enumerate(cropped):
        tok = encode_frame(bytes(w * h) if i == 0 else prev, cur)
        if len(tok) > 0xFFFF:
            raise PackError("a frame encodes to more than 65535 bytes")
        data.append(tok)
        prev = cur
    return {"crop": (x, y, w, h), "palette": palette, "frames": cropped, "data": data, "ms": durations}


def assemble(clips, canvas_side, scale, state_clip, event_clip, loops):
    """Header, index (clip table, then each clip's palette and frame table) and payload, with both CRCs."""
    n = len(clips)
    off = HEADER + n * CLIP_ENTRY
    pal_off, tbl_off = [], []
    for c in clips:
        pal_off.append(off)
        off += 2 * len(c["palette"])
        tbl_off.append(off)
        off += FRAME_ENTRY * len(c["data"])
    index_len = off - HEADER
    table, tail, payload = bytearray(), bytearray(), bytearray()
    for i, c in enumerate(clips):
        x, y, w, h = c["crop"]
        table += struct.pack(CLIP_FMT, x, y, w, h, len(c["data"]), len(c["palette"]), pal_off[i], tbl_off[i], 1 if loops[i] else 0, CODEC_DELTA, 0)
        tail += b"".join(struct.pack("<H", rgb565(*rgb)) for rgb in c["palette"])
        for k, (tok, ms) in enumerate(zip(c["data"], c["ms"])):
            tail += struct.pack("<IHH", HEADER + index_len + len(payload), len(tok), ms | (KEY if k == 0 else 0))
            payload += tok
    index = bytes(table + tail)
    total = HEADER + index_len + len(payload)
    head = struct.pack(
        HEAD_FMT, MAGIC, VERSION, scale, n, canvas_side, canvas_side, total, index_len, zlib.crc32(payload), bytes(state_clip), bytes(event_clip), 0
    )
    return head + struct.pack("<I", zlib.crc32(index, zlib.crc32(head))) + index + bytes(payload)


def build(mapping_path, colors=None, fps_div=1, scale=1):
    """Pack bytes for a mapping file; clips are numbered in sorted key order and the pack holds no key or file name."""
    mapping_path = Path(mapping_path)
    try:
        cfg = tomllib.loads(mapping_path.read_text())
    except (OSError, tomllib.TOMLDecodeError) as e:
        raise PackError(f"cannot read mapping: {e}") from e
    canvas = int(cfg.get("canvas", 0))
    colors = int(colors if colors is not None else cfg.get("colors", MAX_COLORS))
    if scale not in (1, 2):
        raise PackError("--scale must be 1 or 2")
    if canvas <= 0 or canvas > CANVAS_MAX or canvas % scale:
        raise PackError(f"canvas must be 1-{CANVAS_MAX} px and divisible by the scale")
    if not 1 <= colors <= MAX_COLORS:
        raise PackError(f"colors must be 1-{MAX_COLORS}")
    specs = cfg.get("clips", {})
    keys = sorted(specs)
    if not 1 <= len(keys) <= MAX_CLIPS:
        raise PackError(f"the mapping needs 1-{MAX_CLIPS} clips")
    number = {k: i for i, k in enumerate(keys)}
    state_clip, event_clip = [NO_CLIP] * STATE_SLOTS, [NO_CLIP] * EVENT_SLOTS
    for table, names, slots in ((cfg.get("states", {}), STATES, state_clip), (cfg.get("once", {}), EVENTS, event_clip)):
        for name, key in table.items():
            if name not in names:
                raise PackError(f"unknown state or event: {name}")
            if key not in number:
                raise PackError(f"unknown clip key: {key}")
            slots[names.index(name)] = number[key]
    clips, loops = [], []
    for key in keys:
        frames, durations = read_clip(mapping_path.parent, specs[key], canvas)
        frames, durations = thin(frames, durations, fps_div)
        clips.append(_clip_from_frames(frames, durations, colors, canvas, scale))
        loops.append(bool(specs[key].get("loop", True)))
    return assemble(clips, canvas // scale, scale, state_clip, event_clip, loops), clips


# Reading packs


def parse(pack):
    """Header and clip table of a pack, validated as apack.c does; raises PackError with the reason."""
    if len(pack) < HEADER:
        raise PackError("shorter than the header")
    if pack[:HEADER] == b"\xff" * HEADER:
        raise PackError("empty (erased) pack")
    f = struct.unpack(HEAD_FMT, pack[:44])
    magic, version, scale, count, cw, ch, total, index_len, payload_crc, states, events, _ = f
    if magic != MAGIC or version != VERSION:
        raise PackError("not a version 1 pack")
    if total > len(pack) or total < HEADER or index_len > total - HEADER:
        raise PackError("lengths do not fit")
    if zlib.crc32(pack[HEADER : HEADER + index_len], zlib.crc32(pack[:44])) != struct.unpack_from("<I", pack, 44)[0]:
        raise PackError("index CRC mismatch")
    if scale not in (1, 2) or not 1 <= count <= MAX_CLIPS:
        raise PackError("bad scale or clip count")
    payload_off, tbl_end = HEADER + index_len, HEADER + count * CLIP_ENTRY
    if tbl_end > payload_off or any(v != NO_CLIP and v >= count for v in states + events):
        raise PackError("bad clip table or map")
    clips = []
    for i in range(count):
        x, y, w, h, frames, colors, pal, tbl, flags, codec, _ = struct.unpack_from(CLIP_FMT, pack, HEADER + i * CLIP_ENTRY)
        if codec != CODEC_DELTA or not 1 <= colors <= MAX_COLORS or frames == 0:
            raise PackError(f"clip {i}: bad codec, colours or frame count")
        if not (tbl_end <= pal and pal + 2 * colors <= payload_off and tbl_end <= tbl and tbl + FRAME_ENTRY * frames <= payload_off):
            raise PackError(f"clip {i}: palette or frame table outside the index")
        entries = [struct.unpack_from("<IHH", pack, tbl + FRAME_ENTRY * k) for k in range(frames)]
        for k, (doff, dlen, meta) in enumerate(entries):
            if not (payload_off <= doff and doff + dlen <= total) or meta & MS_MASK == 0 or (k == 0 and not meta & KEY):
                raise PackError(f"clip {i} frame {k}: bad entry")
        if not layout_ok(cw, ch, scale, x, y, w, h):
            raise PackError(f"clip {i}: crop outside the canvas or the safe box")
        palette = list(struct.unpack_from(f"<{colors}H", pack, pal))
        clips.append({"crop": (x, y, w, h), "palette": palette, "loop": bool(flags & 1), "entries": entries})
    return {
        "scale": scale,
        "canvas": (cw, ch),
        "total": total,
        "payload_ok": zlib.crc32(pack[payload_off:total]) == payload_crc,
        "states": list(states),
        "events": list(events),
        "clips": clips,
    }


def decode_clips(pack, info):
    """Index planes of every frame of every clip, decoded in order."""
    out = []
    for c in info["clips"]:
        x, y, w, h = c["crop"]
        buf, planes = bytearray(w * h), []
        for doff, dlen, meta in c["entries"]:
            decode_frame(pack[doff : doff + dlen], buf, bool(meta & KEY))
            planes.append(bytes(buf))
        out.append(planes)
    return out


def emit_c(pack, planes, prefix):
    """C header with the pack bytes and the decoded index planes of every clip."""

    def arr(name, data):
        rows = [", ".join(f"0x{b:02X}" for b in data[i : i + 16]) for i in range(0, len(data), 16)]
        return f"static const uint8_t {name}[{len(data)}] = {{\n    " + ",\n    ".join(rows) + ",\n};\n"

    parts = ["/* Generated by firmware/tools/animpack.py build --emit-c; do not edit. */\n#pragma once\n#include <stdint.h>\n"]
    parts.append(arr(f"{prefix}_PACK", pack))
    for i, frames in enumerate(planes):
        parts.append(arr(f"{prefix}_CLIP{i}", b"".join(frames)))
    parts.append(f"static const uint8_t *const {prefix}_CLIPS[{len(planes)}] = {{" + ", ".join(f"{prefix}_CLIP{i}" for i in range(len(planes))) + "};\n")
    return "\n".join(parts)


def in_git_tree(path):
    p = Path(path).resolve()
    return any((d / ".git").exists() for d in [p, *p.parents])


def _guard(path, allow):
    if not allow and in_git_tree(path):
        raise PackError(f"refusing to write inside a git work tree (pass --allow-in-repo): {path}")


def _summary(info):
    lines = [f"canvas {info['canvas'][0]}x{info['canvas'][1]} scale {info['scale']} bytes {info['total']} payload_crc {'ok' if info['payload_ok'] else 'BAD'}"]
    lines.append("states " + " ".join(f"{n}={'-' if v == NO_CLIP else v}" for n, v in zip(STATES, info["states"])))
    lines.append("once " + " ".join(f"{n}={'-' if v == NO_CLIP else v}" for n, v in zip(EVENTS, info["events"])))
    for i, c in enumerate(info["clips"]):
        x, y, w, h = c["crop"]
        ms = sum(m & MS_MASK for _, _, m in c["entries"])
        size = sum(n for _, n, _ in c["entries"])
        lines.append(f"clip {i}: crop {x},{y} {w}x{h} frames {len(c['entries'])} colours {len(c['palette'])} loop_ms {ms} data {size}")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="animpack.py", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    b = sub.add_parser("build", help="convert the clips of a mapping file into a pack")
    b.add_argument("--map", required=True)
    b.add_argument("--out", required=True)
    b.add_argument("--colors", type=int)
    b.add_argument("--fps-div", type=int, default=1)
    b.add_argument("--scale", type=int, default=1)
    b.add_argument("--emit-c", metavar="HEADER", help="also write a C header with the pack and its decoded frames")
    b.add_argument("--allow-in-repo", action="store_true")
    for name, text in (("info", "print the header and a summary per clip"), ("verify", "check both CRCs and decode every frame")):
        s = sub.add_parser(name, help=text)
        s.add_argument("pack")
    d = sub.add_parser("decode", help="write every frame as a PNG")
    d.add_argument("pack")
    d.add_argument("dir")
    d.add_argument("--allow-in-repo", action="store_true")
    args = ap.parse_args(argv)
    try:
        if args.cmd == "build":
            _guard(args.out, args.allow_in_repo)
            if args.emit_c:
                _guard(args.emit_c, args.allow_in_repo)
            pack, _ = build(args.map, args.colors, args.fps_div, args.scale)
            info = parse(pack)
            planes = decode_clips(pack, info)
            Path(args.out).write_bytes(pack)
            if args.emit_c:
                Path(args.emit_c).write_text(emit_c(pack, planes, Path(args.emit_c).stem.upper()))
            print(_summary(info))
            return 0
        pack = Path(args.pack).read_bytes()
        info = parse(pack)
        if args.cmd == "info":
            print(_summary(info))
            return 0
        if not info["payload_ok"]:
            raise PackError("payload CRC mismatch")
        planes = decode_clips(pack, info)
        if args.cmd == "verify":
            print(f"ok: {len(info['clips'])} clips, {sum(len(p) for p in planes)} frames")
            return 0
        _guard(args.dir, args.allow_in_repo)
        write_pngs(info, planes, Path(args.dir))
        return 0
    except (PackError, OSError) as e:
        print(f"animpack: {e}", file=sys.stderr)
        return 2


def write_pngs(info, planes, out_dir):
    """Each frame on its full canvas, at screen scale, as c<clip>_f<frame>.png."""
    from PIL import Image

    out_dir.mkdir(parents=True, exist_ok=True)
    cw, ch = info["canvas"]
    for i, (c, frames) in enumerate(zip(info["clips"], planes)):
        x, y, w, h = c["crop"]
        lut = [(0, 0, 0)] + [(((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3) for v in c["palette"]]
        for k, plane in enumerate(frames):
            im = Image.new("RGB", (cw, ch))
            crop = Image.new("RGB", (w, h))
            crop.putdata([lut[v] if v < len(lut) else (0, 0, 0) for v in plane])
            im.paste(crop, (x, y))
            if info["scale"] == 2:
                im = im.resize((cw * 2, ch * 2), Image.Resampling.NEAREST)
            im.save(out_dir / f"c{i:02d}_f{k:03d}.png")


if __name__ == "__main__":
    sys.exit(main())
