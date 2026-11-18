"""Streaming sentence splitter and spoken-text cleaning."""

from __future__ import annotations

import re

STRONG_END = "。！？；!?;\n"
CLOSERS = "”’」』）)\"'"
SOFT_END = "，,、："

_URL = re.compile(r"(?:https?://|www\.)\S+", re.I)
_LINK = re.compile(r"\[([^\]]*)\]\([^)]*\)")
_FENCE = re.compile(r"`{3,}[^\n`]*")
_LINE_MARK = re.compile(r"^\s{0,3}(?:#{1,6}\s+|>\s?|[-*+•]\s+|\d{1,3}[.)、]\s+)", re.M)
_EMPH = re.compile(r"(\*{1,3}|~~|`+)")
_UNDERSCORE = re.compile(r"(?<![\w])_+|_+(?![\w])")
_EMOJI = re.compile(
    "[\U0001f000-\U0001faff\U00002600-\U000027bf\U00002b00-\U00002bff"
    "\U0000fe00-\U0000fe0f\U0000200d\U000020e3\U0001f1e6-\U0001f1ff\U000e0020-\U000e007f]"
)
_SPACES = re.compile(r"[ \t　]+")
_SPEAKABLE = re.compile(r"\w")


def clean(text: str) -> str:
    text = _LINK.sub(r"\1", text)
    text = _URL.sub("", text)
    text = _FENCE.sub("", text)
    text = _LINE_MARK.sub("", text)
    text = _EMPH.sub("", text)
    text = _UNDERSCORE.sub("", text)
    text = _EMOJI.sub("", text)
    text = text.replace("\n", " ")
    return _SPACES.sub(" ", text).strip()


def speakable(text: str) -> bool:
    return bool(_SPEAKABLE.search(text))


class SentenceSplitter:
    """Cuts streamed text at sentence ends; the first chunk may also end at a comma."""

    def __init__(self, first_chunk_min_chars: int = 12) -> None:
        self._buf = ""
        self._first = True
        self._min = first_chunk_min_chars

    def feed(self, text: str) -> list[str]:
        self._buf += text
        return self._drain(final=False)

    def flush(self) -> list[str]:
        return self._drain(final=True)

    def _cut(self, final: bool) -> int:
        """Return the end index (exclusive) of the next chunk, or -1."""
        buf = self._buf
        n = len(buf)
        for i, ch in enumerate(buf):
            if ch in STRONG_END:
                end = i + 1
            elif ch == "." and (i + 1 < n and buf[i + 1].isspace() or (final and i + 1 == n)):
                if i > 0 and buf[i - 1].isdigit() and i + 1 < n and buf[i + 1].isdigit():
                    continue
                end = i + 1
            elif self._first and ch in SOFT_END and i + 1 >= self._min:
                end = i + 1
            else:
                continue
            while end < n and (buf[end] in CLOSERS or buf[end] in STRONG_END):
                end += 1
            return end
        return -1

    def _drain(self, final: bool) -> list[str]:
        out: list[str] = []
        while True:
            end = self._cut(final)
            if end < 0:
                break
            piece, self._buf = self._buf[:end], self._buf[end:]
            piece = clean(piece)
            if speakable(piece):
                out.append(piece)
                self._first = False
        if final and self._buf:
            piece = clean(self._buf)
            self._buf = ""
            if speakable(piece):
                out.append(piece)
        return out
