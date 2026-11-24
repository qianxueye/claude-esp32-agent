"""Host-side state machine: tap/voice/agent/speech turns (docs/protocol.md, States)."""

from __future__ import annotations

import asyncio
import contextlib
import logging
import time
from collections.abc import Awaitable, Callable

from .agent import Agent
from .audio import earcons
from .config import Config
from .link import Link
from .protocol import Input, State, T, VolMode
from .speaker import Speaker
from .stt import Stt
from .text import SentenceSplitter
from .tts import Tts
from .vad import Vad, VadEvent

log = logging.getLogger("claude_voice.relay")
_STOP = object()


class Relay:
    def __init__(self, cfg: Config, stt: Stt, agent: Agent, tts: Tts) -> None:
        self.cfg = cfg
        self.stt, self.agent, self.tts = stt, agent, tts
        self.link: Link | None = None
        self.speaker = Speaker(cfg, lambda t, p=b"": self.link.send(t, p) if self.link else False)
        self.state = State.IDLE
        self.sounds = earcons(cfg.earcon_gain)
        self.metrics: dict[str, float] = {}
        self.turn_log: list[dict[str, float]] = []
        self.spoken: list[bytes] = []
        self._task: asyncio.Task | None = None
        self._mic: asyncio.Queue | None = None
        self.device_volume: int | None = None

    # ---- link handler ----
    def current_state(self) -> int:
        return int(self.state)

    def on_connect(self) -> None:
        self.speaker.new_session()

    def on_disconnect(self) -> None:
        self._cancel_activity()
        self.state = State.IDLE
        asyncio.get_running_loop().create_task(self.agent.interrupt())

    def on_hello(self, reason: str, info: dict) -> None:
        if reason == "boot":
            self.speaker.boot()
        else:
            self.speaker.request_sync()
        vol = info.get("vol")
        if isinstance(vol, int):
            self.device_volume = vol
            log.info("board volume %d, stored on the board: %s", vol, info.get("vol_set"))
        if self.link and info.get("vol_set") is False:
            self.link.send(T.SET_VOLUME, bytes([self.cfg.volume, VolMode.SEED]))

    def on_frame(self, ftype: int, payload: bytes) -> None:
        if ftype == T.MIC_AUDIO:
            if self._mic is not None and self.state == State.LISTENING:
                self._mic.put_nowait(payload)
        elif ftype == T.SPK_STATUS:
            self.speaker.on_status(payload)
        elif ftype == T.INPUT and payload:
            self._on_input(payload[0])
        elif ftype == T.VOLUME and len(payload) == 1:
            self.device_volume = payload[0]
            log.info("board volume set to %d", payload[0])
        elif ftype == T.WIFI_STATUS:
            log.info("board Wi-Fi: %s", payload.decode(errors="replace"))

    # ---- input handling ----
    def _on_input(self, code: int) -> None:
        log.info("input %d in state %s", code, self.state.name)
        if code != Input.BOOT_BUTTON:
            log.info("input %d ignored: only the BOOT or PWR key acts", code)
            return
        if self.state == State.LISTENING and self._mic is not None:
            self._mic.put_nowait(_STOP)
        elif self.state in (State.THINKING, State.SPEAKING):
            self._cancel_activity()
            self.speaker.flush()
            asyncio.get_running_loop().create_task(self.agent.interrupt())
            self._spawn(self._listen)
        elif self.state == State.IDLE and self._busy():
            log.info("input ignored: activity in progress")
        else:
            self._cancel_activity()
            self._spawn(self._listen)

    def _busy(self) -> bool:
        return self._task is not None and not self._task.done()

    def _cancel_activity(self) -> None:
        if self._task is not None and not self._task.done():
            self._task.cancel()

    def _spawn(self, fn: Callable[[], Awaitable[None]]) -> None:
        prev = self._task

        async def run() -> None:
            if prev is not None:
                with contextlib.suppress(asyncio.CancelledError, Exception):
                    await prev
            try:
                await fn()
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.error("turn failed: %r", e)
                await self._fail()

        self._task = asyncio.create_task(run())

    def _set_state(self, state: State) -> None:
        self.state = state
        if self.link:
            self.link.send_state(state)

    # ---- activities ----
    async def _fail(self) -> None:
        self._mic = None
        self.speaker.flush()
        self._set_state(State.ERROR)
        with contextlib.suppress(Exception):
            await self.speaker.play(self.sounds["error"], wait=False)
        await asyncio.sleep(self.cfg.error_hold_s)
        self._set_state(State.IDLE)

    async def _listen(self) -> None:
        cfg = self.cfg
        await self.speaker.play(self.sounds["listen_start"], wait=True)
        self._mic = queue = asyncio.Queue()
        self._set_state(State.LISTENING)
        pcm = await self._capture(queue)
        self._mic = None
        if pcm is None:
            self._set_state(State.IDLE)
            return
        self.metrics = {"t_speech_end": time.monotonic()}
        await self.speaker.send_pcm(self.sounds["listen_end"])
        self._set_state(State.THINKING)
        text = await asyncio.wait_for(self.stt.transcribe(pcm), cfg.whisper_timeout_s)
        self.metrics["t_transcript"] = time.monotonic()
        log.info("transcript: %r", text)
        if not text:
            self.speaker.end()
            self._set_state(State.IDLE)
            return
        await self._reply(text)

    async def _capture(self, queue: asyncio.Queue) -> bytes | None:
        cfg = self.cfg
        vad = Vad(cfg)
        buf = bytearray()
        loop = asyncio.get_running_loop()
        t0 = loop.time()
        while True:
            now = loop.time()
            deadline = t0 + cfg.max_listen_s
            if not vad.started:
                deadline = min(deadline, t0 + cfg.no_speech_timeout_s)
            if now >= deadline:
                if not vad.started and now < t0 + cfg.max_listen_s:
                    log.info("no speech within %.0f s", cfg.no_speech_timeout_s)
                    return None
                break
            try:
                frame = await asyncio.wait_for(queue.get(), deadline - now)
            except TimeoutError:
                continue
            if frame is _STOP:
                break
            buf += frame
            if vad.feed(frame) is VadEvent.END:
                break
        if len(buf) < cfg.min_utterance_ms * 32:
            return None
        return bytes(buf)

    async def _reply(self, text: str) -> None:
        cfg = self.cfg
        q: asyncio.Queue = asyncio.Queue()
        producer = asyncio.create_task(self._produce(text, q))
        spoke = False
        try:
            while True:
                item = await q.get()
                if item is None:
                    break
                if isinstance(item, BaseException):
                    raise item
                if not spoke:
                    spoke = True
                    self._set_state(State.SPEAKING)
                    self.metrics["t_first_spk"] = time.monotonic()
                self.spoken.append(item)
                await self.speaker.send_pcm(item)
            self.speaker.end()
            if spoke:
                await self.speaker.wait_drained()
            self._set_state(State.IDLE)
            self._log_metrics()
        finally:
            producer.cancel()
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await producer

    async def _produce(self, text: str, q: asyncio.Queue) -> None:
        cfg = self.cfg
        try:
            async with asyncio.timeout(cfg.claude_turn_timeout_s):
                splitter = SentenceSplitter(cfg.first_chunk_min_chars)
                async for delta in self.agent.ask(text):
                    self.metrics.setdefault("t_first_token", time.monotonic())
                    for sentence in splitter.feed(delta):
                        await self._speak(sentence, q)
                for sentence in splitter.flush():
                    await self._speak(sentence, q)
            q.put_nowait(None)
        except asyncio.CancelledError:
            raise
        except BaseException as e:
            q.put_nowait(e)

    async def _speak(self, sentence: str, q: asyncio.Queue) -> None:
        self.metrics.setdefault("t_first_tts_chunk", time.monotonic())
        log.info("speak: %s", sentence)
        q.put_nowait(await self.tts.synth(sentence))

    def _log_metrics(self) -> None:
        m = self.metrics
        base = m.get("t_speech_end")
        if base is None:
            return
        rec = {k: round(v - base, 3) for k, v in m.items() if k != "t_speech_end"}
        self.turn_log.append(rec)
        log.info("latency since end of speech (s): %s", rec)

    # ---- lifecycle ----
    async def idle_reset_loop(self) -> None:
        while True:
            await asyncio.sleep(5)
            if self.state == State.IDLE and self.agent.idle_seconds() > self.cfg.conversation_idle_reset_s:
                log.info("conversation idle, starting a fresh session")
                await self.agent.reset()

    def attach(self, link: Link) -> None:
        self.link = link
