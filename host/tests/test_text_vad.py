import math
import random
from array import array

from claude_voice.audio import earcons, pcm_to_wav, wav_to_pcm
from claude_voice.config import Config
from claude_voice.text import SentenceSplitter, clean, speakable
from claude_voice.vad import Vad, VadEvent


def split_all(chunks, first_min=12):
    s = SentenceSplitter(first_min)
    out = []
    for c in chunks:
        out += s.feed(c)
    return out + s.flush()


def test_split_chinese_and_ascii_terminators():
    assert split_all(["今天星期三。明天", "是星期四！好吧；再见?"]) == ["今天星期三。", "明天是星期四！", "好吧；", "再见?"]


def test_split_streamed_char_by_char():
    text = "第一句。第二句！Third one. Done"
    assert split_all(list(text)) == ["第一句。", "第二句！", "Third one.", "Done"]


def test_decimal_point_does_not_split():
    assert split_all(["圆周率是3.14159。好"]) == ["圆周率是3.14159。", "好"]


def test_first_chunk_ends_at_comma_only_once():
    out = split_all(["好的，我来帮你查一下今天的天气情况，然后告诉你。"], first_min=6)
    assert out == ["好的，我来帮你查一下今天的天气情况，", "然后告诉你。"]
    assert split_all(["好的，我来，然后。再见，朋友。"], first_min=3) == ["好的，", "我来，然后。", "再见，朋友。"]


def test_newline_splits_and_blank_dropped():
    assert split_all(["一行\n\n二行\n"]) == ["一行", "二行"]


def test_clean_markdown_emoji_urls():
    assert clean("**你好**，访问 https://example.com/a?b=1 看看 😀") == "你好，访问 看看"
    assert clean("## 标题") == "标题"
    assert clean("- 第一项") == "第一项"
    assert clean("见[官网](https://x.y)了解") == "见官网了解"
    assert clean("用 `ls` 命令") == "用 ls 命令"
    assert clean("snake_case 和 _斜体_") == "snake_case 和 斜体"
    assert clean("温度 21°C ✅") == "温度 21°C"


def test_unspeakable_dropped():
    assert not speakable("。！ ")
    assert split_all(["😀。", "好。"]) == ["好。"]


def tone_frames(amp, ms, seed=1):
    rng = random.Random(seed)
    n = 16 * ms
    s = array("h", (int(amp * math.sin(i * 0.3) + rng.randint(-40, 40)) for i in range(n)))
    b = s.tobytes()
    return [b[i : i + 640] for i in range(0, len(b), 640)]


def run_vad(frames, cfg=None):
    v = Vad(cfg or Config())
    events = []
    for i, f in enumerate(frames):
        e = v.feed(f)
        if e is not VadEvent.NONE:
            events.append((i, e))
            if e is VadEvent.END:
                break
    return events


def test_vad_detects_speech_then_end():
    frames = tone_frames(0, 400) + tone_frames(4000, 1000) + tone_frames(0, 1200)
    ev = run_vad(frames)
    assert [e for _, e in ev] == [VadEvent.SPEECH_START, VadEvent.END]
    start_frame, end_frame = ev[0][0], ev[1][0]
    assert 20 + 14 <= start_frame <= 20 + 16  # 400 ms noise + 300 ms speech
    assert 20 + 50 + 38 <= end_frame <= 20 + 50 + 42  # speech ends at 1400 ms, +800 ms silence


def test_vad_ignores_short_click():
    ev = run_vad(tone_frames(0, 400) + tone_frames(4000, 120) + tone_frames(0, 1500))
    assert ev == []


def test_vad_adapts_to_loud_noise_floor():
    loud = array("h", (int(500 * math.sin(i * 1.1)) for i in range(16 * 1000))).tobytes()
    loud_frames = [loud[i : i + 640] for i in range(0, len(loud), 640)]
    ev = run_vad(loud_frames + tone_frames(6000, 600) + loud_frames)
    kinds = [e for _, e in ev]
    assert kinds[0] is VadEvent.SPEECH_START and kinds[-1] is VadEvent.END


def test_wav_roundtrip_and_earcons():
    pcm = earcons(0.3)["listen_start"]
    assert wav_to_pcm(pcm_to_wav(pcm)) == pcm
    assert all(len(p) % 2 == 0 and len(p) > 1000 for p in earcons(0.3).values())
