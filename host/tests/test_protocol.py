import struct

from claude_voice.protocol import SYNC, Decoder, T, crc16, encode


def test_crc_check_value():
    assert crc16(b"123456789") == 0x29B1


def test_encode_layout():
    f = encode(T.PING, struct.pack("<I", 7))
    assert f[:2] == SYNC and f[2] == 0xA0 and f[3:5] == b"\x04\x00"
    assert struct.unpack("<H", f[-2:])[0] == crc16(f[2:-2])


def test_roundtrip_and_split_feed():
    d = Decoder()
    f = encode(T.INPUT, b"\x01") + encode(T.HELLO, b'{"proto":1}')
    out = []
    for i in range(0, len(f), 3):
        out += d.feed(f[i : i + 3], 0.0)
    assert out == [(0x20, b"\x01"), (0x01, b'{"proto":1}')]


def test_resync_after_garbage():
    d = Decoder()
    garbage = b"ESP-ROM:esp32s3\r\n\xc1\xc1\xad\xff\xff"
    out = d.feed(garbage + encode(T.HELLO, b"{}"), 0.0)
    assert out == [(0x01, b"{}")]


def test_resync_after_bad_crc_keeps_following_frame():
    d = Decoder()
    bad = bytearray(encode(T.LOG, b"hello"))
    bad[-1] ^= 0xFF
    out = d.feed(bytes(bad) + encode(T.INPUT, b"\x03"), 0.0)
    assert out == [(0x20, b"\x03")]


def test_sync_inside_corrupted_frame_is_found():
    d = Decoder()
    inner = encode(T.INPUT, b"\x02")
    bad = SYNC + b"\x02\x08\x00" + inner  # claims 8 payload bytes, contains a whole frame
    out = d.feed(bad + b"\x00" * 16, 0.0)
    assert (0x20, b"\x02") in out


def test_oversize_length_rejected():
    d = Decoder()
    out = d.feed(SYNC + b"\x10\x01\x20" + encode(T.INPUT, b"\x01"), 0.0)
    assert out == [(0x20, b"\x01")]


def test_partial_frame_abandoned_after_200ms():
    d = Decoder()
    f = encode(T.LOG, b"abcdefgh")
    assert d.feed(f[:6], 0.0) == []
    assert d.poll(0.15) == []
    assert d.poll(0.25) == []
    # the tail of the abandoned frame must not complete it later
    assert d.feed(f[6:], 0.3) == []
    assert d.feed(encode(T.INPUT, b"\x01"), 0.31) == [(0x20, b"\x01")]


def test_partial_frame_completes_within_200ms():
    d = Decoder()
    f = encode(T.LOG, b"abcdefgh")
    assert d.feed(f[:6], 0.0) == []
    assert d.feed(f[6:], 0.15) == [(0x02, b"abcdefgh")]


def test_partial_frame_kept_when_rule_disabled():
    d = Decoder(partial_timeout=None)
    f = encode(T.INPUT, b"\x01")
    assert d.feed(f[:4], 0.0) == []
    assert d.poll(5.0) == []
    assert d.feed(f[4:], 10.0) == [(0x20, b"\x01")]


def test_split_sync_across_feeds():
    d = Decoder()
    f = encode(T.INPUT, b"\x01")
    assert d.feed(b"xx" + f[:1], 0.0) == []
    assert d.feed(f[1:], 0.01) == [(0x20, b"\x01")]
