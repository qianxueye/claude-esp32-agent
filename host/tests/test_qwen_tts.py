import asyncio
import dataclasses
import glob
import os
import shutil
import sys
import tempfile
import time

import httpx
import pytest
from test_relay import make, until

from claude_voice.audio import pcm_to_wav, tone
from claude_voice.config import Config, load
from claude_voice.protocol import Input, State
from claude_voice.qwen_tts import QwenTts
from claude_voice.tts import SayTts

needs_afconvert = pytest.mark.skipif(not os.path.exists("/usr/bin/afconvert"), reason="afconvert missing")

SAY_PCM = b"\x01\x00" * 100


class FakeSay:
    def __init__(self):
        self.texts = []

    async def synth(self, text):
        self.texts.append(text)
        return SAY_PCM


def wav24k(ms=1000):
    n = 24000 * ms // 1000
    return pcm_to_wav(b"\x10\x10" * n, rate=24000)


def cfg_for(**over):
    return dataclasses.replace(Config(), tts_engine="qwen3", qwen_url="http://speech.test", **over)


class Server:
    def __init__(self, handler=None):
        self.requests = []
        self.handler = handler or (lambda req: httpx.Response(200, content=wav24k()))

    def __call__(self, req):
        self.requests.append(req)
        return self.handler(req)


def engine(server, say=None, **over):
    say = say or FakeSay()
    return QwenTts(cfg_for(**over), say, transport=httpx.MockTransport(server)), say


@needs_afconvert
async def test_success_resamples_to_16k_and_sends_config():
    server = Server()
    q, say = engine(server, qwen_voices={"japanese": "x"})
    await q.start()
    server.requests.clear()
    pcm = await q.synth("你好。")
    assert say.texts == []
    assert abs(len(pcm) / 2 / 16000 - 1.0) < 0.05
    import json

    body = json.loads(server.requests[0].content)
    assert body == {
        "model": Config().qwen_model, "input": "你好。", "voice": "ono_anna",
        "lang_code": "chinese", "response_format": "wav",
    }
    await q.close()


@needs_afconvert
async def test_per_language_voice_override():
    server = Server()
    q, _ = engine(server, qwen_lang_code="japanese", qwen_voices={"japanese": "sohee"})
    await q.start()
    import json

    assert json.loads(server.requests[-1].content)["voice"] == "sohee"


def failures():
    def refuse(req):
        raise httpx.ConnectError("refused")

    def timeout(req):
        raise httpx.ReadTimeout("slow")

    return {
        "connect": refuse,
        "timeout": timeout,
        "status": lambda req: httpx.Response(500),
        "bad_wav": lambda req: httpx.Response(200, content=b"not a wav"),
        "empty": lambda req: httpx.Response(200, content=pcm_to_wav(b"", rate=24000)),
    }


@needs_afconvert
@pytest.mark.parametrize("kind", list(failures()))
async def test_failure_falls_back_to_say(kind):
    server = Server()
    q, say = engine(server)
    await q.start()
    server.handler = failures()[kind]
    q._skip_until = 0.0
    assert await q.synth("再见。") == SAY_PCM
    assert say.texts == ["再见。"]


@needs_afconvert
async def test_afconvert_failure_falls_back(tmp_path):
    bad = tmp_path / "afconvert"
    bad.write_text("#!/bin/sh\nexit 3\n")
    bad.chmod(0o755)
    q, say = engine(Server(), afconvert_bin=str(bad))
    await q.start()
    q._skip_until = 0.0
    assert await q.synth("再见。") == SAY_PCM


@needs_afconvert
async def test_skip_interval_and_recovery(caplog):
    server = Server()
    q, say = engine(server, qwen_skip_s=0.3)
    await q.start()
    server.handler = failures()["status"]
    caplog.set_level("WARNING", logger="claude_voice.qwen_tts")
    await q.synth("一。")
    n = len(server.requests)
    await q.synth("二。")
    await q.synth("三。")
    assert len(server.requests) == n
    assert say.texts == ["一。", "二。", "三。"]
    assert len([r for r in caplog.records if r.levelname == "WARNING"]) == 1
    server.handler = lambda req: httpx.Response(200, content=wav24k())
    await asyncio.sleep(0.35)
    assert await q.synth("四。") != SAY_PCM
    assert len(server.requests) == n + 1


@needs_afconvert
async def test_reply_text_not_logged_above_debug(caplog):
    server = Server()
    q, _ = engine(server)
    caplog.set_level("INFO", logger="claude_voice.qwen_tts")
    await q.start()
    server.handler = failures()["status"]
    await q.synth("机密内容。")
    assert "机密内容" not in caplog.text


async def test_unset_url_and_cmd_uses_say(caplog):
    say = FakeSay()
    q = QwenTts(dataclasses.replace(Config(), tts_engine="qwen3"), say)
    await q.start()
    assert await q.synth("好。") == SAY_PCM
    assert "qwen_url" in caplog.text


async def test_server_command_that_exits_keeps_relay_on_say(tmp_path):
    cfg = dataclasses.replace(
        Config(), tts_engine="qwen3", qwen_server_cmd=[sys.executable, "-c", "raise SystemExit(2)"],
        log_dir=str(tmp_path),
    )
    say = FakeSay()
    q = QwenTts(cfg, say)
    await q.start()
    assert await q.synth("好。") == SAY_PCM
    await q.close()


async def test_local_server_started_warmed_and_stopped(tmp_path):
    srv = tmp_path / "srv.py"
    (tmp_path / "ok.wav").write_bytes(wav24k())
    srv.write_text(
        "import sys, http.server\n"
        "a = sys.argv\n"
        "port = int(a[a.index('--port') + 1])\n"
        "class H(http.server.BaseHTTPRequestHandler):\n"
        "    def do_GET(self):\n        self.send_response(200); self.end_headers()\n"
        "    def do_POST(self):\n"
        "        d = open(sys.argv[0].replace('srv.py', 'ok.wav'), 'rb').read()\n"
        "        self.send_response(200); self.send_header('Content-Length', str(len(d))); self.end_headers(); self.wfile.write(d)\n"
        "    def log_message(self, *x): pass\n"
        "http.server.HTTPServer(('127.0.0.1', port), H).serve_forever()\n"
    )
    cfg = dataclasses.replace(
        Config(), tts_engine="qwen3", qwen_server_cmd=[sys.executable, str(srv)], log_dir=str(tmp_path),
        qwen_ready_timeout_s=20.0,
    )
    q = QwenTts(cfg, FakeSay())
    await q.start()
    proc = q._proc
    assert proc is not None and proc.poll() is None
    assert q._http is not None
    assert "--host" in proc.args and "127.0.0.1" in proc.args
    await q.close()
    assert proc.poll() is not None


@needs_afconvert
async def test_cancel_during_request_leaves_nothing(tmp_path, monkeypatch):
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path))
    server = Server()
    q, say = engine(server)
    await q.start()
    started = asyncio.Event()

    async def slow(req):
        started.set()
        await asyncio.sleep(30)

    q._http = httpx.AsyncClient(base_url="http://speech.test", transport=httpx.MockTransport(slow))
    t = asyncio.create_task(q.synth("好。"))
    await started.wait()
    t.cancel()
    with pytest.raises(asyncio.CancelledError):
        await t
    assert say.texts == []
    assert glob.glob(str(tmp_path / "claude-voice-*")) == []


@needs_afconvert
async def test_cancel_during_afconvert_kills_child(tmp_path, monkeypatch):
    monkeypatch.setattr(tempfile, "tempdir", str(tmp_path))
    pidfile = tmp_path / "pid"
    slow = tmp_path / "afconvert"
    slow.write_text(f"#!/bin/sh\necho $$ > {pidfile}\nexec sleep 30\n")
    slow.chmod(0o755)
    q, _ = engine(Server())
    await q.start()
    q._cfg = dataclasses.replace(q._cfg, afconvert_bin=str(slow))
    t = asyncio.create_task(q.synth("好。"))
    for _ in range(200):
        if pidfile.exists() and pidfile.read_text().strip():
            break
        await asyncio.sleep(0.02)
    pid = int(pidfile.read_text())
    t.cancel()
    with pytest.raises(asyncio.CancelledError):
        await t
    with pytest.raises(ProcessLookupError):
        os.kill(pid, 0)
    assert [p for p in glob.glob(str(tmp_path / "claude-voice-*"))] == []


def test_defaults_keep_say(tmp_path):
    assert Config().tts_engine == "say"
    p = tmp_path / "c.toml"
    p.write_text('voice = "Tingting"\n')
    assert load(p).tts_engine == "say"
    p.write_text('tts_engine = "other"\n')
    with pytest.raises(ValueError):
        load(p)


@needs_afconvert
async def test_relay_speaks_from_qwen_engine():
    server = Server()
    q, say = engine(server)
    await q.start()
    h = make(tts=q)
    await h.up()
    try:
        h.dev.inject(Input.TAP)
        await until(lambda: h.dev.states[-1:] == [State.IDLE] and State.SPEAKING in h.dev.states)
        assert say.texts == []
        assert len(server.requests) >= 3
        assert len(h.dev.spk) > 2 * 12800
    finally:
        await h.down()
