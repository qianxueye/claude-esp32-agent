"""Real whisper-server, Claude session and `say` against the fake device (spends Claude quota)."""

import asyncio
import dataclasses
import os
import subprocess
import tempfile
from pathlib import Path

import pytest
from fake_device import FakeDevice

from claude_voice.agent import ClaudeAgent
from claude_voice.audio import pcm_to_wav, silence, wav_to_pcm
from claude_voice.config import HOST_DIR, load
from claude_voice.link import Link, memory_pair
from claude_voice.protocol import Input, State
from claude_voice.relay import Relay
from claude_voice.stt import WhisperStt
from claude_voice.tts import SayTts

pytestmark = [
    pytest.mark.e2e,
    pytest.mark.skipif(os.environ.get("CLAUDE_VOICE_E2E") != "1", reason="set CLAUDE_VOICE_E2E=1"),
]
OUT = HOST_DIR / "test-output"
QUESTIONS = os.environ.get("CLAUDE_VOICE_E2E_QUESTIONS", "今天星期几？|那明天呢？").split("|")


def mic_pcm(text: str, tmp: Path, name: str) -> bytes:
    aiff = tmp / f"{name}.aiff"
    wav = tmp / f"{name}.wav"
    subprocess.run(["say", "-v", "Tingting", "-o", str(aiff), text], check=True)
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(aiff), "-ar", "16000", "-ac", "1", "-sample_fmt", "s16", str(wav)],
        check=True,
    )
    return silence(500) + wav_to_pcm(str(wav))


async def test_two_turns_one_session():
    OUT.mkdir(exist_ok=True)
    cfg = dataclasses.replace(load(), log_dir=str(OUT))
    stt, agent = WhisperStt(cfg), ClaudeAgent(cfg)
    relay = Relay(cfg, stt, agent, SayTts(cfg))
    host, dev_end = memory_pair()
    dev = FakeDevice(dev_end, speed=1.0, boot_delay=None)
    link = Link(cfg, lambda: host, relay)
    relay.attach(link)
    transcripts: list[str] = []
    orig = stt.transcribe

    async def spy(pcm):
        text = await orig(pcm)
        transcripts.append(text)
        return text

    try:
        await stt.start()
        stt.transcribe = spy
        agent.start()
        dev.start()
        runner = asyncio.create_task(link.run())
        with tempfile.TemporaryDirectory() as td:
            mics = [mic_pcm(q, Path(td), f"q{i}") for i, q in enumerate(QUESTIONS)]
        mark = 0
        for i, mic in enumerate(mics):
            dev.mic = mic
            dev.inject(Input.TAP)
            async with asyncio.timeout(120):
                while len(relay.turn_log) <= i:
                    await asyncio.sleep(0.1)
                await dev.wait_state(State.IDLE, 30)
            reply = bytes(dev.spk[mark:])
            mark = len(dev.spk)
            (OUT / f"turn{i + 1}.wav").write_bytes(pcm_to_wav(reply))
            print(f"turn {i + 1}: transcript={transcripts[i]!r} latency={relay.turn_log[i]}")
            assert transcripts[i]
            assert len(reply) > 16000
        print("init:", agent.init_info)
        runner.cancel()
    finally:
        await dev.stop()
        await agent.close()
        await stt.close()
