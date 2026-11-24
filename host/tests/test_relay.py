import asyncio
import dataclasses
import logging

import pytest
from fake_device import FakeDevice, speech_pcm

from claude_voice.audio import tone
from claude_voice.config import Config
from claude_voice.link import Link, memory_pair
from claude_voice.protocol import Input, State, T
from claude_voice.relay import Relay

SPEED = 25.0


class FakeStt:
    def __init__(self, text="你好", fail=False, delay=0.0):
        self.text, self.fail, self.delay, self.calls = text, fail, delay, []

    async def transcribe(self, pcm):
        self.calls.append(len(pcm))
        await asyncio.sleep(self.delay)
        if self.fail:
            raise RuntimeError("stt boom")
        return self.text


class FakeAgent:
    def __init__(self, chunks=("你好。", "再见。"), hang=False, fail=False):
        self.chunks, self.hang, self.fail = chunks, hang, fail
        self.asked, self.interrupts, self.resets = [], 0, 0
        self.proceed = asyncio.Event()

    async def ask(self, text):
        self.asked.append(text)
        if self.fail:
            raise RuntimeError("agent boom")
        if self.hang:
            await asyncio.Event().wait()
        for c in self.chunks:
            yield c
            await asyncio.sleep(0.005)
        if getattr(self, "gate_after", False):
            await self.proceed.wait()

    async def interrupt(self):
        self.interrupts += 1

    async def reset(self):
        self.resets += 1

    def idle_seconds(self):
        return 0.0


class FakeTts:
    def __init__(self, ms=400):
        self.ms, self.texts = ms, []

    async def synth(self, text):
        self.texts.append(text)
        return tone(300, self.ms, 0.2)


class Harness:
    def __init__(self, cfg, stt, agent, tts, mic, speed=SPEED, dev_kw=None):
        self.cfg, self.stt, self.agent, self.tts = cfg, stt, agent, tts
        self.dev_kw = dev_kw or {}
        self.mic = mic
        self.speed = speed
        self.pairs = []
        self.devices = []
        self.relay = Relay(cfg, stt, agent, tts)
        self.link = Link(cfg, self._open, self.relay)
        self.relay.attach(self.link)
        self.allow_open = True

    def _open(self):
        if not self.allow_open:
            return None
        host, dev = memory_pair()
        d = FakeDevice(dev, speed=self.speed, mic=self.mic, **self.dev_kw)
        self.pairs.append((host, dev))
        self.devices.append(d)
        d.start()
        return host

    @property
    def dev(self):
        return self.devices[-1]

    async def up(self):
        self.task = asyncio.create_task(self.link.run())
        async with asyncio.timeout(3):
            while not (self.devices and T.SET_STATE in self.dev.received):
                await asyncio.sleep(0.01)
        await asyncio.sleep(0.05)
        self.dev.states.clear()

    async def down(self):
        self.task.cancel()
        for d in self.devices:
            await d.stop()


DEVICE_KEYS = ("boot_delay", "hello_delay", "consumed0", "proto", "vol")


def make(mic=None, stt=None, agent=None, tts=None, speed=SPEED, **over):
    dev_kw = {k: over.pop(k) for k in DEVICE_KEYS if k in over}
    cfg = dataclasses.replace(
        Config(), error_hold_s=0.1, ping_interval_s=0.05, reconnect_backoff_min_s=0.02,
        reconnect_backoff_max_s=0.05, read_poll_s=0.01, usb_hello_retry_s=0.02, usb_open_timeout_s=3.0, **over,
    )
    return Harness(cfg, stt or FakeStt(), agent or FakeAgent(), tts or FakeTts(), speech_pcm() if mic is None else mic, speed, dev_kw)


@pytest.fixture
async def h():
    harness = make()
    await harness.up()
    yield harness
    await harness.down()


async def settle(dev, state=State.IDLE, timeout=8.0):
    await dev.wait_state(state, timeout)


async def until(pred, timeout=5.0):
    async with asyncio.timeout(timeout):
        while not pred():
            await asyncio.sleep(0.005)


async def test_happy_path_states_and_audio(h):
    h.dev.inject(Input.TAP)
    await until(lambda: h.dev.states[-1:] == [State.IDLE] and State.SPEAKING in h.dev.states)
    assert h.dev.states == [State.LISTENING, State.THINKING, State.SPEAKING, State.IDLE]
    assert h.agent.asked == ["你好"]
    assert h.tts.texts == ["你好。", "再见。"]
    assert len(h.dev.spk) > 2 * 12800
    assert h.dev.max_queued <= 64000
    assert h.dev.vol == 70 and h.relay.device_volume == 70
    assert h.relay.turn_log and h.relay.turn_log[0]["t_first_spk"] >= h.relay.turn_log[0]["t_transcript"]


async def test_boot_button_starts_listening(h):
    h.dev.inject(Input.BOOT_BUTTON)
    await settle(h.dev, State.LISTENING)


async def test_tap_ends_listening_early():
    h = make(mic=speech_pcm(lead_ms=200, speech_ms=20000), stt=FakeStt(delay=0.5))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.LISTENING)
        await asyncio.sleep(0.1)
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.THINKING)
    finally:
        await h.down()


async def test_no_speech_returns_to_idle():
    h = make(mic=b"", no_speech_timeout_s=0.3)
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.LISTENING)
        await until(lambda: h.dev.states[-1] == State.IDLE)
        assert h.stt.calls == []
    finally:
        await h.down()


async def test_max_listen_limit():
    h = make(mic=speech_pcm(speech_ms=60000), max_listen_s=0.4, stt=FakeStt(delay=0.5))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.THINKING)
    finally:
        await h.down()


async def test_empty_transcript_returns_to_idle():
    h = make(stt=FakeStt(text=""))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await until(lambda: h.dev.states[-2:] == [State.THINKING, State.IDLE])
        assert h.agent.asked == []
    finally:
        await h.down()


async def test_barge_in_while_thinking():
    h = make(agent=FakeAgent(hang=True))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.THINKING)
        flushes = h.dev.flushes
        h.dev.inject(Input.TAP)
        await until(lambda: h.dev.states[-1] == State.LISTENING)
        assert h.agent.interrupts >= 1 and h.dev.flushes > flushes
    finally:
        await h.down()


async def test_barge_in_while_speaking():
    h = make(tts=FakeTts(ms=3000))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.SPEAKING)
        flushes = h.dev.flushes
        h.dev.inject(Input.TAP)
        await until(lambda: h.dev.states[-1] == State.LISTENING and h.dev.flushes > flushes)
        assert h.agent.interrupts >= 1
        await until(lambda: h.dev.states[-1] == State.IDLE or State.SPEAKING in h.dev.states[-3:])
    finally:
        await h.down()


async def test_long_press_resets_conversation():
    h = make(tts=FakeTts(ms=3000))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.SPEAKING)
        h.dev.inject(Input.LONG_PRESS)
        await until(lambda: h.agent.resets == 1 and h.dev.states[-1] == State.IDLE)
        assert h.dev.flushes >= 1
        assert h.dev.clips  # reset earcon was ended
    finally:
        await h.down()


async def test_long_press_while_idle():
    h = make()
    await h.up()
    try:
        h.dev.inject(Input.LONG_PRESS)
        await until(lambda: h.agent.resets == 1)
        assert h.dev.state == State.IDLE
    finally:
        await h.down()


async def test_stt_failure_goes_error_then_idle():
    h = make(stt=FakeStt(fail=True))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await until(lambda: State.ERROR in h.dev.states)
        await until(lambda: h.dev.states[-1] == State.IDLE)
        assert h.dev.flushes >= 1
    finally:
        await h.down()


async def test_agent_failure_goes_error_then_idle():
    h = make(agent=FakeAgent(fail=True))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await until(lambda: State.ERROR in h.dev.states)
        await until(lambda: h.dev.states[-1] == State.IDLE)
    finally:
        await h.down()


async def test_boot_hello_mid_session_gets_state_but_no_host_hello_or_volume():
    h = make(agent=FakeAgent(hang=True))
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.THINKING)
        hellos = h.dev.host_hellos
        sets = len(h.dev.set_volumes)
        h.dev.reset()
        await until(lambda: h.dev.state == State.THINKING)
        await asyncio.sleep(0.3)
        assert len(h.dev.set_volumes) == sets
        assert h.dev.host_hellos == hellos
        assert h.dev.hellos == ["boot", "reply", "boot"]
        assert h.relay.speaker.outstanding == 0
    finally:
        await h.down()


async def test_reconnect_after_port_disappears():
    h = make()
    await h.up()
    try:
        first = h.dev
        h.allow_open = False
        await h.pairs[-1][1].close()
        await asyncio.sleep(0.15)
        assert not h.link.connected
        h.allow_open = True
        await until(lambda: len(h.devices) == 2 and T.SET_STATE in h.dev.received)
        assert h.dev is not first and 1 <= h.dev.host_hellos <= 2
        h.dev.inject(Input.TAP)
        await settle(h.dev, State.LISTENING)
    finally:
        await h.down()


async def test_pings_flow():
    h = make()
    await h.up()
    try:
        await until(lambda: h.dev.pings >= 3)
    finally:
        await h.down()


async def test_flow_control_never_exceeds_64000():
    h = make(tts=FakeTts(ms=4000), speed=1.0)
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await until(lambda: h.dev.states[-1:] == [State.IDLE] and State.SPEAKING in h.dev.states, timeout=20)
        assert len(h.dev.spk) >= 2 * 4000 * 32
        assert 60000 < h.dev.max_queued <= 64000
    finally:
        await h.down()


async def test_seed_only_when_board_has_no_volume():
    h = make(vol=35)
    await h.up()
    try:
        await asyncio.sleep(0.3)
        assert T.SET_VOLUME not in h.dev.received and h.relay.device_volume == 35
    finally:
        await h.down()
    h = make()
    await h.up()
    try:
        await until(lambda: h.dev.vol_set)
        assert h.dev.set_volumes and set(h.dev.set_volumes) == {bytes([70, 2])} and h.relay.device_volume == 70
    finally:
        await h.down()


async def test_knob_report_is_kept_across_hello():
    h = make()
    await h.up()
    try:
        await until(lambda: h.dev.vol_set)
        h.dev.knob(20)
        await until(lambda: h.relay.device_volume == 20)
        sets = len(h.dev.set_volumes)
        h.dev.hello("resume")
        await asyncio.sleep(0.3)
        assert h.relay.device_volume == 20 and len(h.dev.set_volumes) == sets and h.dev.vol == 20
    finally:
        await h.down()


@pytest.mark.parametrize("store", ["ok", "error", None])
async def test_unpaired_hello_log_follows_the_store_field(store, caplog):
    caplog.set_level(logging.INFO, logger="claude_voice")
    h = make()
    await h.up()
    try:
        caplog.clear()
        h.dev.store = store
        h.dev.hello("resume")
        await until(lambda: any("board HELLO" in r.getMessage() for r in caplog.records))
    finally:
        await h.down()
    logged = "\n".join(r.getMessage() for r in caplog.records)
    assert ("not paired" in logged) is (store != "error")
    assert ("could not read its pairing storage" in logged) is (store == "error")
