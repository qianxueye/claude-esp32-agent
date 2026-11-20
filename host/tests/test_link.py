import asyncio
import dataclasses
import struct

from fake_device import FakeDevice
from test_relay import FakeTts, make, settle, until

from claude_voice.audio import tone
from claude_voice.config import Config
from claude_voice.protocol import Input, State, T
from claude_voice.speaker import MOD, Speaker


def status(queued, flags, consumed):
    return struct.pack("<IBI", queued, flags, consumed % MOD)


class Sent:
    def __init__(self):
        self.frames = []

    def __call__(self, ftype, payload=b""):
        self.frames.append((ftype, payload))
        return True

    def audio_bytes(self):
        return sum(len(p) for t, p in self.frames if t == T.SPK_AUDIO)


def speaker(**over):
    sent = Sent()
    cfg = dataclasses.replace(Config(), **{"spk_status_gap_s": 0.2, **over})
    return Speaker(cfg, sent), sent


async def test_speaker_sends_nothing_until_synced_then_counts_from_status():
    spk, sent = speaker()
    spk.new_session()
    task = asyncio.create_task(spk.send_pcm(bytes(8192)))
    await asyncio.sleep(0.05)
    assert sent.audio_bytes() == 0
    spk.on_status(status(1000, 1, 5_000_000))
    await asyncio.wait_for(task, 1)
    assert sent.audio_bytes() == 8192
    assert spk.outstanding == 1000 + 8192


async def test_speaker_outstanding_is_wrap_safe():
    spk, sent = speaker()
    spk.new_session()
    spk.on_status(status(0, 0, MOD - 3000))
    await spk.send_pcm(bytes(60000))
    assert spk.outstanding == 60000
    spk.on_status(status(0, 0, MOD - 3000 + 60000))
    assert spk.outstanding == 0
    task = asyncio.create_task(spk.send_pcm(bytes(64000)))
    await asyncio.wait_for(task, 1)
    assert spk.outstanding == 64000


async def test_speaker_blocks_at_64000_outstanding():
    spk, sent = speaker(spk_status_gap_s=5.0)
    spk.boot()
    task = asyncio.create_task(spk.send_pcm(bytes(100000)))
    await asyncio.sleep(0.05)
    assert sent.audio_bytes() <= 64000 and not task.done()
    spk.on_status(status(64000 - 40000, 1, 40000))
    await asyncio.wait_for(task, 1)
    assert sent.audio_bytes() == 100000


async def test_speaker_short_status_is_ignored():
    spk, _ = speaker()
    spk.new_session()
    spk.on_status(struct.pack("<IB", 0, 0))
    assert not spk.synced


async def test_speaker_stall_guard_recovers_lost_bytes():
    spk, sent = speaker()
    spk.boot()
    await spk.send_pcm(bytes(64000))
    spk.on_status(status(0, 0, 60000))  # 4000 bytes never reached the board
    task = asyncio.create_task(spk.send_pcm(bytes(8192)))
    await asyncio.wait_for(task, 1)
    assert sent.audio_bytes() == 64000 + 8192


async def test_speaker_flush_aborts_sender_without_resetting_counter():
    spk, sent = speaker(spk_status_gap_s=5.0)
    spk.boot()
    task = asyncio.create_task(spk.send_pcm(bytes(200000)))
    await asyncio.sleep(0.02)
    spk.flush()
    await asyncio.wait_for(task, 1)
    assert spk.outstanding == sent.audio_bytes()
    spk.on_status(status(0, 0, sent.audio_bytes()))
    assert spk.outstanding == 0


async def test_usb_open_sends_one_host_hello_after_the_reset_boot():
    h = make()
    await h.up()
    try:
        await asyncio.sleep(0.2)
        # a retry in flight when the boot HELLO arrives may add one reply; nothing follows the open phase
        n = h.dev.host_hellos
        assert h.dev.hellos == ["boot"] + ["reply"] * n and 1 <= n <= 2
        assert 1 <= h.dev.received.count(T.SET_VOLUME) <= n + 1 and h.dev.received.count(T.SET_STATE) >= n + 1
        assert all(p == bytes([70, 2]) for p in h.dev.set_volumes)
        assert h.relay.speaker.synced and h.dev.flushes == 0
        await asyncio.sleep(0.3)
        assert h.dev.host_hellos == n
    finally:
        await h.down()


async def test_usb_open_without_reset_syncs_counter_by_flush():
    h = make(boot_delay=None, consumed0=7_777_777)
    await h.up()
    try:
        await until(lambda: h.relay.speaker.synced)
        n = h.dev.host_hellos
        assert h.dev.hellos == ["reply"] * n and 1 <= n <= 2 and h.dev.flushes >= 1
        h.dev.inject(Input.BOOT_BUTTON)
        await until(lambda: h.dev.states[-1:] == [State.IDLE] and State.SPEAKING in h.dev.states)
    finally:
        await h.down()


async def test_resume_hello_gets_state_and_no_volume():
    h = make()
    await h.up()
    try:
        await asyncio.sleep(0.1)
        before = list(h.dev.received)
        h.dev.hello("resume")
        await until(lambda: T.SET_STATE in h.dev.received[len(before):])
        await asyncio.sleep(0.1)
        new = h.dev.received[len(before):]
        assert T.HOST_HELLO not in new and T.SET_STATE in new and T.SPK_FLUSH not in new and T.SET_VOLUME not in new
    finally:
        await h.down()


async def test_v1_board_is_refused():
    h = make(proto=1)
    h.task = asyncio.create_task(h.link.run())
    try:
        await until(lambda: h.devices and h.dev.state == State.ERROR)
        h.dev.inject(Input.BOOT_BUTTON)
        await asyncio.sleep(0.3)
        assert h.dev.state == State.ERROR and T.SET_VOLUME not in h.dev.received
        assert not h.link.connected and h.relay.state == State.IDLE
        assert T.SPK_AUDIO not in h.dev.received
    finally:
        await h.down()


async def test_counter_wraps_during_playback():
    h = make(tts=FakeTts(ms=2500), boot_delay=None, consumed0=MOD - 50000)
    await h.up()
    try:
        h.dev.inject(Input.BOOT_BUTTON)
        await until(lambda: h.dev.states[-1:] == [State.IDLE] and State.SPEAKING in h.dev.states, timeout=10)
        assert h.dev.consumed > MOD and h.dev.max_queued <= 64000
    finally:
        await h.down()


async def test_whole_reply_is_delivered_in_order():
    h = make(tts=FakeTts(ms=700))
    await h.up()
    try:
        h.dev.inject(Input.BOOT_BUTTON)
        await until(lambda: h.dev.states[-1:] == [State.IDLE] and State.SPEAKING in h.dev.states)
        clip = tone(300, 700, 0.2)
        assert bytes(h.dev.spk).count(clip) == 2
    finally:
        await h.down()


async def test_listening_and_mic_require_v3_hello():
    h = make()
    await h.up()
    try:
        h.dev.inject(Input.BOOT_BUTTON)
        await settle(h.dev, State.LISTENING)
        assert h.link.active_kind == "usb"
    finally:
        await h.down()


def test_fake_device_status_layout():
    d = FakeDevice.__new__(FakeDevice)
    d.queued, d.consumed = 10, MOD + 5
    out = []
    d.send = lambda t, p=b"": out.append((t, p))
    d.status(2)
    assert out == [(T.SPK_STATUS, struct.pack("<IBI", 10, 2, 5))]


async def test_non_v3_hello_after_v3_stops_processing():
    h = make()
    await h.up()
    try:
        h.dev.proto = 1
        h.dev.hello("resume")
        await until(lambda: h.dev.state == State.ERROR)
        h.dev.inject(Input.BOOT_BUTTON)
        await asyncio.sleep(0.3)
        assert h.dev.state == State.ERROR and not h.link.connected and h.relay.state == State.IDLE
    finally:
        await h.down()
