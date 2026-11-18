"""One persistent, isolated Claude Code session driven through claude-agent-sdk."""

from __future__ import annotations

import asyncio
import logging
import os
import time
from collections.abc import AsyncIterator
from typing import Protocol

from claude_agent_sdk import ClaudeAgentOptions, ClaudeSDKClient
from claude_agent_sdk.types import AssistantMessage, ResultMessage, StreamEvent, SystemMessage, TextBlock

from .config import Config

log = logging.getLogger("claude_voice.agent")
_END = object()


class AgentError(RuntimeError):
    pass


class Agent(Protocol):
    def ask(self, text: str) -> AsyncIterator[str]: ...
    async def interrupt(self) -> None: ...
    async def reset(self) -> None: ...
    def idle_seconds(self) -> float: ...


class ClaudeAgent:
    def __init__(self, cfg: Config) -> None:
        self._cfg = cfg
        self._cmds: asyncio.Queue = asyncio.Queue()
        self._client: ClaudeSDKClient | None = None
        self._supervisor: asyncio.Task | None = None
        self._busy = False
        self._turn = 0
        self._last = time.monotonic()
        self._closed = False
        self.init_info: dict = {}

    def options(self) -> ClaudeAgentOptions:
        cfg = self._cfg
        workdir = os.path.expanduser(cfg.claude_workdir)
        os.makedirs(workdir, exist_ok=True)
        return ClaudeAgentOptions(
            cli_path=cfg.claude_cli,
            cwd=workdir,
            model=cfg.model,
            effort=cfg.effort,
            system_prompt=cfg.system_prompt,
            tools=list(cfg.tools),
            allowed_tools=list(cfg.tools),
            strict_mcp_config=True,
            include_partial_messages=True,
            setting_sources=[],
            extra_args={"safe-mode": None, "restricted": None, "no-session-persistence": None},
        )

    def start(self) -> None:
        self._supervisor = asyncio.create_task(self._supervise())

    async def close(self) -> None:
        self._closed = True
        self._cmds.put_nowait(None)
        if self._supervisor:
            try:
                await asyncio.wait_for(self._supervisor, 10)
            except (TimeoutError, asyncio.CancelledError):
                self._supervisor.cancel()

    def idle_seconds(self) -> float:
        return 0.0 if self._busy else time.monotonic() - self._last

    async def reset(self) -> None:
        await self.interrupt()
        old, self._cmds = self._cmds, asyncio.Queue()
        old.put_nowait(None)
        self._last = time.monotonic()

    async def interrupt(self) -> None:
        client = self._client
        if not self._busy or client is None:
            return
        turn = self._turn
        try:
            await client.interrupt()
        except Exception as e:
            log.warning("interrupt failed: %s", e)
        asyncio.get_running_loop().call_later(
            self._cfg.interrupt_drain_timeout_s, lambda: self._watchdog(turn)
        )

    def _watchdog(self, turn: int) -> None:
        if self._busy and self._turn == turn:
            log.warning("interrupted turn did not finish; restarting session")
            old, self._cmds = self._cmds, asyncio.Queue()
            old.put_nowait(None)
            if self._supervisor is not None:
                self._supervisor.cancel()
                self._supervisor = asyncio.create_task(self._supervise())

    async def ask(self, text: str) -> AsyncIterator[str]:
        out: asyncio.Queue = asyncio.Queue()
        self._cmds.put_nowait((text, out))
        while True:
            item = await out.get()
            if item is _END:
                return
            if isinstance(item, BaseException):
                raise item
            yield item

    async def _supervise(self) -> None:
        while not self._closed:
            q = self._cmds
            try:
                await self._run_once(q)
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log.error("claude session failed: %r", e)
                self._fail_pending(q, AgentError(f"claude session failed: {e!r}"))
                await asyncio.sleep(1)

    @staticmethod
    def _fail_pending(q: asyncio.Queue, err: Exception) -> None:
        while not q.empty():
            cmd = q.get_nowait()
            if cmd is not None:
                cmd[1].put_nowait(err)

    async def _run_once(self, q: asyncio.Queue) -> None:
        async with ClaudeSDKClient(self.options()) as client:
            self._client = client
            log.info("claude session connected")
            try:
                while True:
                    cmd = await q.get()
                    if cmd is None:
                        return
                    text, out = cmd
                    await self._turn_once(client, text, out)
            finally:
                self._client = None

    async def _turn_once(self, client: ClaudeSDKClient, text: str, out: asyncio.Queue) -> None:
        self._busy = True
        self._turn += 1
        streamed = False
        fallback: list[str] = []
        try:
            await client.query(text)
            async for msg in client.receive_response():
                if isinstance(msg, SystemMessage) and msg.subtype == "init":
                    self._check_init(msg.data)
                elif isinstance(msg, StreamEvent):
                    ev = msg.event
                    delta = ev.get("delta") or {}
                    if ev.get("type") == "content_block_delta" and delta.get("type") == "text_delta":
                        streamed = True
                        out.put_nowait(delta.get("text", ""))
                elif isinstance(msg, AssistantMessage):
                    fallback += [b.text for b in msg.content if isinstance(b, TextBlock)]
                elif isinstance(msg, ResultMessage):
                    if msg.is_error:
                        raise AgentError(f"claude error: {msg.result or msg.subtype}")
            if not streamed and fallback:
                out.put_nowait("".join(fallback))
            out.put_nowait(_END)
        except AgentError as e:
            out.put_nowait(e)
        except Exception as e:
            out.put_nowait(AgentError(f"claude turn failed: {e!r}"))
            raise
        finally:
            self._busy = False
            self._last = time.monotonic()

    def _check_init(self, data: dict) -> None:
        self.init_info = {k: data.get(k) for k in ("tools", "mcp_servers", "model", "permissionMode")}
        log.info("claude init: %s", self.init_info)
        if set(data.get("tools") or []) != set(self._cfg.tools) or data.get("mcp_servers"):
            raise AgentError(f"session isolation check failed: {self.init_info}")
