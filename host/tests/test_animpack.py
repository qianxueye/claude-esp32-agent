import importlib.util
import random
import sys
from pathlib import Path

import pytest
from PIL import Image

TOOL = Path(__file__).parents[2] / "firmware" / "tools" / "animpack.py"
GOLDEN = Path(__file__).parents[2] / "firmware" / "test" / "apack_golden.h"
SPEC = importlib.util.spec_from_file_location("animpack", TOOL)
animpack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(animpack)

# RGB565-exact colours, so palettes and decoded frames compare without rounding.
BLACK = (0, 0, 0)
A = (248, 120, 88)
B = (240, 240, 232)
C = (16, 16, 16)
D = (200, 64, 64)


def frame(size, pixels):
    im = Image.new("RGB", (size, size), BLACK)
    for (x, y), rgb in pixels.items():
        im.putpixel((x, y), rgb)
    return im


def save_gif(path, frames, durations):
    """GIF with one fixed palette and no optimisation, so every colour reads back exactly."""
    colours = sorted({c for f in frames for _, c in f.getcolors(256)})
    pal = Image.new("P", (1, 1))
    pal.putpalette([v for c in colours for v in c] + [0] * 3 * (256 - len(colours)))
    p = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]
    p[0].save(path, save_all=True, append_images=p[1:], duration=durations, loop=0, optimize=False, disposal=1)


def save_sheet(path, frames, columns):
    cell = frames[0].size[0]
    rows = (len(frames) + columns - 1) // columns
    sheet = Image.new("RGBA", (cell * columns, cell * rows), (0, 0, 0, 0))
    for i, f in enumerate(frames):
        sheet.paste(f, (i % columns * cell, i // columns * cell))
    sheet.save(path)


def write_map(path, text):
    path.write_text(text)
    return path


def golden_frames():
    """Clip 0: two frames with fills, literals and a short gap; clip 1: one frame with a long gap."""
    f0 = {(x, 4): A for x in range(3, 13)}
    f0.update({(x, 5): (A if x % 2 else B) for x in range(3, 13)})
    f0.update({(3, y): C for y in range(6, 9)})
    f0.update({(12, y): C for y in range(6, 9)})
    f0.update({(x, 9): B for x in range(3, 13)})
    f1 = dict(f0)
    f1.update({(3, 5): D, (4, 5): D, (5, 5): D, (8, 7): A})
    g0 = {(0, 2): A, (1, 2): B}
    g0.update({(x, 13): D for x in range(10, 16)})
    return [frame(16, f0), frame(16, f1)], [frame(16, g0)]


def golden_inputs(d):
    clip0, clip1 = golden_frames()
    save_gif(d / "first_clip_src.gif", clip0, [50, 40])
    save_sheet(d / "second_clip_src.png", clip1, 1)
    return write_map(
        d / "mapping.toml",
        """canvas = 16
colors = 250
[states]
idle = "alpha_key"
listening = "beta_key"
[once]
pickup = "alpha_key"
[clips.alpha_key]
file = "first_clip_src.gif"
[clips.beta_key]
file = "second_clip_src.png"
sheet = { cell = 16, columns = 1, frames = 1 }
frame_ms = [30]
loop = false
""",
    )


def emit_golden(d):
    out = d / "out.clawdpack"
    header = d / "apack_golden.h"
    assert animpack.main(["build", "--map", str(golden_inputs(d)), "--out", str(out), "--emit-c", str(header)]) == 0
    return out.read_bytes(), header.read_text()


def rgb565_of(rgb):
    return animpack.rgb565(*rgb)


def test_golden_header_matches_the_tool(tmp_path):
    pack, text = emit_golden(tmp_path)
    assert text == GOLDEN.read_text(), "regenerate with: uv run python tests/test_animpack.py --write-golden"
    info = animpack.parse(pack)
    assert info["canvas"] == (16, 16) and info["scale"] == 1 and len(pack) < 300
    assert info["states"][:2] == [0, 1] and info["events"][0] == 0
    assert [c["crop"] for c in info["clips"]] == [(3, 4, 10, 6), (0, 2, 16, 12)]
    assert [c["loop"] for c in info["clips"]] == [True, False]


def test_round_trip_and_names_absent(tmp_path):
    pack, _ = emit_golden(tmp_path)
    info = animpack.parse(pack)
    planes = animpack.decode_clips(pack, info)
    for clip, frames, decoded in zip(info["clips"], golden_frames(), planes):
        x, y, w, h = clip["crop"]
        lut = [0] + clip["palette"]
        for src, plane in zip(frames, decoded):
            for yy in range(16):
                for xx in range(16):
                    inside = x <= xx < x + w and y <= yy < y + h
                    got = lut[plane[(yy - y) * w + xx - x]] if inside else 0
                    assert got == rgb565_of(src.getpixel((xx, yy)))
    for word in (b"alpha", b"beta", b"_key", b"first_clip", b"second_clip", b".gif", b".png", b"mapping"):
        assert word not in pack


def test_two_builds_are_identical(tmp_path):
    m = golden_inputs(tmp_path)
    assert animpack.build(m)[0] == animpack.build(m)[0]


def test_sheet_equals_gif(tmp_path):
    frames, _ = golden_frames()
    save_gif(tmp_path / "x.gif", frames, [50, 40])
    save_sheet(tmp_path / "x.png", frames, 2)
    gif = write_map(tmp_path / "g.toml", 'canvas = 16\n[states]\nidle = "k"\n[clips.k]\nfile = "x.gif"\n')
    sheet = write_map(tmp_path / "s.toml", 'canvas = 16\n[states]\nidle = "k"\n[clips.k]\nfile = "x.png"\nsheet = { cell = 16, columns = 2, frames = 2 }\nframe_ms = [50, 40]\n')
    assert animpack.build(gif)[0] == animpack.build(sheet)[0]


def test_crop_durations_and_fps_div(tmp_path):
    frames = [frame(32, {(10 + i, 12): A, (14, 20 - i): B}) for i in range(6)]
    save_gif(tmp_path / "x.gif", frames, [50, 40, 30, 50, 40, 30])
    m = write_map(tmp_path / "m.toml", 'canvas = 32\n[states]\nidle = "k"\n[clips.k]\nfile = "x.gif"\n')
    info = animpack.parse(animpack.build(m)[0])
    clip = info["clips"][0]
    assert clip["crop"] == (10, 12, 6, 9)
    assert [e[3] & animpack.MS_MASK for e in clip["entries"]] == [50, 40, 30, 50, 40, 30]
    assert [bool(e[3] & animpack.KEY) for e in clip["entries"]] == [True] + [False] * 5
    info2 = animpack.parse(animpack.build(m, fps_div=2)[0])
    assert [e[3] & animpack.MS_MASK for e in info2["clips"][0]["entries"]] == [90, 80, 70]
    m3 = write_map(tmp_path / "p.toml", 'canvas = 32\n[states]\nidle = "k"\n[clips.k]\nfile = "x.gif"\nframe_ms = [70, 20]\n')
    info3 = animpack.parse(animpack.build(m3)[0])
    assert [e[3] & animpack.MS_MASK for e in info3["clips"][0]["entries"]] == [70, 20, 70, 20, 70, 20]


def test_scale_two_halves_the_canvas(tmp_path):
    frames = [frame(32, {(x, y): A for x in range(8, 12) for y in range(10, 14)})]
    save_gif(tmp_path / "x.gif", frames, [50])
    m = write_map(tmp_path / "m.toml", 'canvas = 32\n[states]\nidle = "k"\n[clips.k]\nfile = "x.gif"\n')
    info = animpack.parse(animpack.build(m, scale=2)[0])
    assert info["canvas"] == (16, 16) and info["scale"] == 2 and info["clips"][0]["crop"] == (4, 5, 2, 2)


def test_many_colours_are_quantised_and_black_stays_zero(tmp_path):
    frames = []
    for f in range(2):
        px = {(2 + x, 2 + y): (64 + 8 * x, 64 + 8 * y, 96 + 64 * f + 4 * ((x + y) % 8)) for x in range(20) for y in range(20)}
        frames.append(frame(24, px))
    assert sum(len(f.getcolors(1 << 16)) - 1 for f in frames) > animpack.MAX_COLORS
    palette, planes = animpack.index_clip(frames, animpack.MAX_COLORS)
    assert 1 <= len(palette) <= animpack.MAX_COLORS
    for src, plane in zip(frames, planes):
        raw = src.tobytes()
        for i in range(len(plane)):
            assert (plane[i] == 0) == (raw[3 * i : 3 * i + 3] == bytes(BLACK))


def test_settle_gives_black_index_zero_alone():
    entries = [(4, 2, 4), A, B, (0, 0, 0)] + [(0, 0, 0)] * 252
    raw = b"".join(bytes(c) for c in (BLACK, (8, 4, 8), A, BLACK, B, (200, 100, 80)))
    qb = bytes([0, 0, 1, 2, 2, 0])
    palette, plane = animpack.settle(qb, raw, entries)
    assert palette == [A, B]
    # The darkest entry's non-black pixels take the closest kept colour; a black pixel in any box is 0.
    assert list(plane) == [0, 1, 1, 0, 2, 1]
    # Red proof: renumbering alone leaves a visible pixel at 0 and a black one at a colour.
    assert list(qb.translate(bytes([0, 1, 2] + [0] * 253))) == [0, 0, 1, 2, 2, 0]


def test_exact_colours_keep_their_values(tmp_path):
    frames = [frame(8, {(1, 1): A, (2, 2): C, (3, 3): B})]
    palette, planes = animpack.index_clip(frames, 3)
    assert palette == sorted([A, B, C]) and planes[0][9] == palette.index(A) + 1 and planes[0][0] == 0


@pytest.mark.parametrize(
    "prev,cur,want",
    [
        ([0] * 8, [5, 0, 0, 6, 0, 0, 0, 0], [0x83, 5, 0, 0, 6]),
        ([0] * 8, [5, 0, 0, 0, 6, 0, 0, 0], [0x80, 5, 0x02, 0x80, 6]),
        ([0] * 21, [5] + [0] * 20, [0x80, 5]),
        ([0] * 130, [0] * 127 + [7, 7, 7], [0x7E, 0xC1, 7]),
        ([0] * 131, [0] * 128 + [7, 7, 7], [0x7F, 0, 0, 0xC1, 7]),
        ([0] * 4, [9, 9, 3, 3], [0x83, 9, 9, 3, 3]),
        ([1] * 66, [2] * 66, [0xFE, 2, 0xC0, 2]),
        ([1] * 67, [2] * 67, [0xFF, 2, 0xC0, 2]),
    ],
)
def test_encoder_rules(prev, cur, want):
    tok = animpack.encode_frame(bytes(prev), bytes(cur))
    assert list(tok) == want
    buf = bytearray(prev)
    animpack.decode_frame(tok, buf, False)
    assert bytes(buf) == bytes(cur)


def test_fill_chunks_never_leave_one_pixel():
    def naive(run):
        out = []
        while run > 0:
            out.append(min(run, 65))
            run -= out[-1]
        return out

    lengths = lambda tok: [tok[i] - 0xBE for i in range(0, len(tok), 2)]
    for run in range(3, 300):
        assert 1 not in lengths(animpack._fill(9, run)) and sum(lengths(animpack._fill(9, run))) == run
    # Red proof: plain 65-pixel chunks leave a single pixel, which no FILL token can carry.
    assert naive(66) == [65, 1]


def test_literal_chunks_of_64():
    cur = bytes((i % 2) + 1 for i in range(70))
    tok = animpack.encode_frame(bytes(70), cur)
    assert tok[0] == 0xBF and tok[65] == 0x85 and len(tok) == 72


def test_random_frames_round_trip():
    rng = random.Random(7)
    for _ in range(400):
        n = rng.randrange(1, 400)
        prev = bytes(rng.choice([0, 0, 0, 1, 2]) for _ in range(n))
        cur = bytes(v if rng.random() < 0.6 else rng.choice([0, 1, 2, 3]) for v in prev)
        for key in (False, True):
            base = bytes(n) if key else prev
            tok = animpack.encode_frame(base, cur)
            buf = bytearray(prev)
            animpack.decode_frame(tok, buf, key)
            assert bytes(buf) == cur


@pytest.mark.parametrize(
    "tok",
    [bytes([0x0A]), bytes([0x84, 1, 2, 3]), bytes([0xC0]), bytes([0x7F, 0]), bytes([0x02, 0xC7, 9])],
)
def test_decoder_refuses_overlong_tokens(tok):
    buf = bytearray(10)
    with pytest.raises(animpack.PackError):
        animpack.decode_frame(tok, buf, False)
    assert len(buf) == 10


def build_error(tmp_path, text, files=()):
    for name, frames in files:
        save_gif(tmp_path / name, frames, [50] * len(frames))
    with pytest.raises(animpack.PackError) as e:
        animpack.build(write_map(tmp_path / "m.toml", text))
    return str(e.value)


def test_errors(tmp_path):
    one = [frame(16, {(4, 4): A})]
    assert "missing file" in build_error(tmp_path, 'canvas = 16\n[states]\nidle = "k"\n[clips.k]\nfile = "nope.gif"\n')
    assert "unknown clip key" in build_error(tmp_path, 'canvas = 16\n[states]\nidle = "z"\n[clips.k]\nfile = "a.gif"\n', [("a.gif", one)])
    assert "unknown state" in build_error(tmp_path, 'canvas = 16\n[states]\nsleep = "k"\n[clips.k]\nfile = "a.gif"\n', [("a.gif", one)])
    assert "canvas" in build_error(tmp_path, 'canvas = 520\n[states]\nidle = "k"\n[clips.k]\nfile = "a.gif"\n', [("a.gif", one)])
    assert "no visible pixel" in build_error(tmp_path, 'canvas = 16\n[states]\nidle = "k"\n[clips.k]\nfile = "b.gif"\n', [("b.gif", [frame(16, {})])])
    assert "frame size" in build_error(tmp_path, 'canvas = 32\n[states]\nidle = "k"\n[clips.k]\nfile = "a.gif"\n', [("a.gif", one)])


def test_output_inside_a_git_tree_is_refused(tmp_path):
    m = golden_inputs(tmp_path)
    repo = tmp_path / "repo"
    (repo / ".git").mkdir(parents=True)
    out = repo / "sub" / "x.clawdpack"
    out.parent.mkdir()
    assert animpack.main(["build", "--map", str(m), "--out", str(out)]) == 2 and not out.exists()
    assert animpack.main(["build", "--map", str(m), "--out", str(tmp_path / "x.clawdpack"), "--emit-c", str(repo / "g.h")]) == 2
    assert not (tmp_path / "x.clawdpack").exists()
    assert animpack.main(["build", "--map", str(m), "--out", str(out), "--allow-in-repo"]) == 0 and out.exists()
    assert animpack.main(["decode", str(out), str(repo / "frames")]) == 2


def test_info_verify_and_decode(tmp_path, capsys):
    pack, _ = emit_golden(tmp_path)
    p = tmp_path / "out.clawdpack"
    assert animpack.main(["info", str(p)]) == 0
    assert "clip 1: crop 0,2 16x12 reach 10 bubble right 0 frames 1" in capsys.readouterr().out
    assert animpack.main(["verify", str(p)]) == 0
    assert animpack.main(["decode", str(p), str(tmp_path / "frames")]) == 0
    with Image.open(tmp_path / "frames" / "c00_f001.png") as im:
        assert im.convert("RGB").getpixel((3, 5)) == D and im.convert("RGB").getpixel((0, 0)) == BLACK
    bad = bytearray(pack)
    bad[-1] ^= 1
    p.write_bytes(bytes(bad))
    assert animpack.main(["verify", str(p)]) == 2
    bad = bytearray(pack)
    bad[60] ^= 1
    p.write_bytes(bytes(bad))
    assert animpack.main(["info", str(p)]) == 2
    p.write_bytes(b"\xff" * 64)
    assert animpack.main(["info", str(p)]) == 2


def one_clip(tmp_path, canvas, frames, extra="", name="x"):
    save_gif(tmp_path / f"{name}.gif", frames, [50] * len(frames))
    return write_map(tmp_path / f"{name}.toml", f'canvas = {canvas}\n[states]\nidle = "k"\n[clips.k]\nfile = "{name}.gif"\n{extra}')


def test_skip_beyond_u16_becomes_two_tokens():
    tokens = animpack._skip(70000)
    assert tokens == animpack._skip(animpack.SKIP_MAX) + animpack._skip(70000 - animpack.SKIP_MAX) and len(tokens) == 6
    buf = bytearray(70001)
    animpack.decode_frame(tokens + bytes([0x80, 7]), buf, True)
    assert buf[70000] == 7 and not any(buf[:70000])
    assert animpack._skip(animpack.SKIP_MAX) == bytes([0x7F, 0xFF, 0xFF])


def test_reach_of_a_pixel_at_a_known_radius():
    def plane(w, x, y):
        p = bytearray(w * w)
        p[y * w + x] = 1
        return [bytes(p)]

    # Canvas 100, centre 50: pixel 52,46 has its far corner at (53, 46), 3 px right and 4 px above, exactly 5.
    assert animpack.lit_reach(plane(100, 52, 46), 100, 100) == 5
    # Pixel 80,50: far corner (81, 51) is hypot(31, 1) = 31.02 px away, rounded up.
    assert animpack.lit_reach(plane(100, 80, 50), 100, 100) == 32
    # Odd side 101, centre 50.5: pixel 50,50 spans 49.5..50.5 either side, far corner 0.5 px from the centre in both axes, rounded up.
    assert animpack.lit_reach(plane(101, 50, 50), 101, 101) == 1


def test_reach_over_the_cap_raises(tmp_path):
    corner = [frame(400, {(0, 0): A})]
    assert "reaches" in build_error(tmp_path, 'canvas = 400\n[states]\nidle = "k"\n[clips.k]\nfile = "c.gif"\n', [("c.gif", corner)])
    at_cap = animpack.ART_REACH_MAX
    inside = [frame(416, {(2, 208): A})]
    save_gif(tmp_path / "i.gif", inside, [50])
    m = write_map(tmp_path / "i.toml", 'canvas = 416\n[states]\nidle = "k"\n[clips.k]\nfile = "i.gif"\n')
    assert animpack.parse(animpack.build(m)[0])["clips"][0]["reach"] == at_cap
    edge = [frame(416, {(1, 208): A})]
    assert "reaches" in build_error(tmp_path, 'canvas = 416\n[states]\nidle = "k"\n[clips.k]\nfile = "j.gif"\n', [("j.gif", edge)])


def test_width_multiple_of_64_gains_a_column(tmp_path):
    wide = [frame(400, {(40, 190): A, (359, 209): B})]
    clip = animpack.parse(animpack.build(one_clip(tmp_path, 400, wide))[0])["clips"][0]
    assert clip["crop"][2] == 321 and clip["crop"][0] in (39, 40)
    full = [frame(64, {(0, 30): A, (63, 33): B})]
    assert animpack.parse(animpack.build(one_clip(tmp_path, 64, full, name="f"))[0])["clips"][0]["crop"] == (0, 30, 64, 4)
    other = [frame(400, {(40, 190): A, (358, 209): B})]
    assert animpack.parse(animpack.build(one_clip(tmp_path, 400, other, name="o"))[0])["clips"][0]["crop"][2] == 319


def test_bubble_side_picks_the_emptier_half(tmp_path):
    def side(pixels, extra=""):
        m = one_clip(tmp_path, 400, [frame(400, pixels)], extra)
        clip = animpack.parse(animpack.build(m)[0])["clips"][0]
        return "left" if clip["bubble_left"] else "right", clip["bubble_y"]

    assert side({(250, 200): A, (200, 100): B}) == ("left", 0)
    assert side({(150, 200): A, (200, 100): B}) == ("right", 0)
    assert side({(200, 100): A, (200, 300): B}) == ("right", 0)
    assert side({(250, 200): A, (200, 100): B}, 'bubble_side = "right"\nbubble_y = -12\n') == ("right", -12)
    assert side({(150, 200): A, (200, 100): B}, 'bubble_side = "left"\n') == ("left", 0)
    with pytest.raises(animpack.PackError):
        side({(150, 200): A}, 'bubble_side = "up"\n')


def test_card_option_writes_animpack_bin(tmp_path):
    m = golden_inputs(tmp_path)
    card = tmp_path / "card"
    card.mkdir()
    assert animpack.main(["build", "--map", str(m), "--card", str(card)]) == 0
    assert [p.name for p in card.iterdir()] == ["ANIMPACK.BIN"]
    assert animpack.main(["verify", str(card / "ANIMPACK.BIN")]) == 0
    repo = tmp_path / "repo"
    (repo / ".git").mkdir(parents=True)
    assert animpack.main(["build", "--map", str(m), "--card", str(repo)]) == 2 and not (repo / "ANIMPACK.BIN").exists()


def test_v2_round_trip_with_long_skip_and_frame_crcs(tmp_path):
    a = {(70, 70): A, (330, 330): A}
    b = {(70, 70): B, (330, 330): B}
    frames = [frame(400, a), frame(400, b)]
    pack, _ = animpack.build(one_clip(tmp_path, 400, frames))
    assert pack[8:10] == b"\x02\x00" and pack[24:28] == bytes(4)
    info = animpack.parse(pack)
    clip = info["clips"][0]
    assert clip["crop"] == (70, 70, 261, 261) and clip["reach"] == 186 and animpack.frame_crcs_ok(pack, info)
    assert [e[0] + e[1] for e in clip["entries"][:-1]] == [e[0] for e in clip["entries"][1:]]
    assert len(animpack._skip(261 * 261 - 2)) == 6
    planes = animpack.decode_clips(pack, info)[0]
    lut = [0] + clip["palette"]
    assert [lut[v] for v in (planes[0][0], planes[0][-1], planes[1][0], planes[1][-1])] == [rgb565_of(A), rgb565_of(A), rgb565_of(B), rgb565_of(B)]
    flipped = bytearray(pack)
    flipped[clip["entries"][1][0]] ^= 1
    assert not animpack.frame_crcs_ok(bytes(flipped), animpack.parse(bytes(flipped)))
    skewed = bytearray(pack)
    entry = animpack.HEADER + animpack.CLIP_ENTRY + 2 * len(clip["palette"]) + animpack.FRAME_ENTRY
    skewed[entry : entry + 4] = (clip["entries"][1][0] + 1).to_bytes(4, "little")
    skewed[44:48] = animpack.zlib.crc32(bytes(skewed[48 : animpack.HEADER + int.from_bytes(skewed[20:24], "little")]), animpack.zlib.crc32(bytes(skewed[:44]))).to_bytes(4, "little")
    with pytest.raises(animpack.PackError):
        animpack.parse(bytes(skewed))


if __name__ == "__main__" and sys.argv[1:] == ["--write-golden"]:
    import tempfile

    with tempfile.TemporaryDirectory() as d:
        GOLDEN.write_text(emit_golden(Path(d))[1])
    print(f"wrote {GOLDEN}")
