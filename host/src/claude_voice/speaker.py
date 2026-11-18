"""Speaker output with the protocol's flow control: (sent − consumed) mod 2^32 stays at or below 64 000."""

from __future__ import annotations

import asyncio
import logging
import struct

from .config import Config
from .protocol import SAMPLE_RATE, T

log = logging.getLogger("claude_voice.speaker")
MOD = 1 << 32


class Speaker:
    def __init__(self, cfg: Config, send) -> None:
        self._cfg = cfg
        self._send = send
        self._sent = 0
        self._consumed = 0
        self._synced = False
        self._changed = asyncio.Event()
        self._drained = asyncio.Event()
        self._epoch = 0

    @property
    def outstanding(self) -> int:
        return (self._sent - self._consumed) % MOD

    @property
    def synced(self) -> bool:
        return self._synced

    def on_status(self, payload: bytes) -> None:
        if len(payload) < 9:
            return
        queued, flags, consumed = struct.unpack_from("<IBI", payload)
        self._consumed = consumed
        if not self._synced:
            # nothing is sent while unsynced, so the board has received exactly consumed + queued
            self._sent = (consumed + queued) % MOD
            self._synced = True
        elif self.outstanding > MOD // 2:
            log.warning("board consumed more than was sent; resynchronising the speaker counter")
            self._sent = (consumed + queued) % MOD
        if flags & 2:
            self._drained.set()
        self._changed.set()

    def boot(self) -> None:
        """The board rebooted: both counters restart at zero."""
        self._sent = self._consumed = 0
        self._synced = True
        self._abort()

    def new_session(self) -> None:
        """A new link session: the byte count is unknown until the next SPK_STATUS."""
        self._synced = False
        self._abort()

    def request_sync(self) -> None:
        """Ask for an SPK_STATUS; the board answers SPK_FLUSH with one immediately."""
        if not self._synced:
            self._send(T.SPK_FLUSH)

    def flush(self) -> None:
        self._send(T.SPK_FLUSH)
        self._abort()

    def _abort(self) -> None:
        self._epoch += 1
        self._drained.set()
        self._changed.set()

    async def _wait_room(self, need: int, epoch: int) -> bool:
        cfg = self._cfg
        while not self._synced or self.outstanding + need > cfg.spk_max_outstanding:
            self._changed.clear()
            try:
                await asyncio.wait_for(self._changed.wait(), cfg.spk_status_gap_s)
            except TimeoutError:
                if not self._synced:
                    self.request_sync()
                else:
                    # bytes lost on the link are never consumed, and an empty board stops reporting
                    log.warning("no SPK_STATUS for %.1f s; %d bytes presumed lost", cfg.spk_status_gap_s, self.outstanding)
                    self._sent = self._consumed
            if epoch != self._epoch:
                return False
        return True

    async def send_pcm(self, pcm: bytes) -> None:
        cfg = self._cfg
        epoch = self._epoch
        for i in range(0, len(pcm), cfg.spk_chunk):
            chunk = pcm[i : i + cfg.spk_chunk]
            chunk = chunk[: len(chunk) & ~1]
            if not chunk:
                continue
            if not await self._wait_room(len(chunk), epoch):
                return
            if self._send(T.SPK_AUDIO, chunk):
                self._sent = (self._sent + len(chunk)) % MOD

    def end(self) -> None:
        self._drained.clear()
        self._send(T.SPK_END)

    async def wait_drained(self) -> bool:
        """Wait for the device's drained flag; True if it arrived."""
        epoch = self._epoch
        expected_s = self.outstanding / (2 * SAMPLE_RATE)
        try:
            await asyncio.wait_for(self._drained.wait(), expected_s + self._cfg.drained_slack_s)
        except TimeoutError:
            return False
        return epoch == self._epoch

    async def play(self, pcm: bytes, *, wait: bool) -> None:
        """Send a complete clip followed by SPK_END; optionally wait until it has played."""
        await self.send_pcm(pcm)
        self.end()
        if wait:
            await self.wait_drained()
