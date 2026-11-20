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

FAKE_SERVER = """
import http.server, os, socketserver, sys, time
def stamp(msg):
    print(f"{time.time():.3f} {msg}", file=sys.stderr, flush=True)
port = int(sys.argv[sys.argv.index('--port') + 1])
here = os.path.dirname(os.path.abspath(__file__))
post_status = POST_STATUS
class Server(http.server.HTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = self.server_address[0], self.server_address[1]
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.end_headers()
    def do_POST(self):
        stamp("POST")
        if post_status != 200:
            self.send_response(post_status); self.send_header('Content-Length', '0'); self.end_headers(); return
        d = open(os.path.join(here, 'ok.wav'), 'rb').read()
        self.send_response(200); self.send_header('Content-Length', str(len(d))); self.end_headers(); self.wfile.write(d)
    def log_message(self, *x): pass
stamp("constructing server")
srv = Server(('127.0.0.1', port), H)
stamp("listening")
srv.serve_forever()
"""


def write_fake_server(tmp_path, post_status=200):
    """Write a fake speech server script; returns its path."""
    (tmp_path / "ok.wav").write_bytes(wav24k())
    srv = tmp_path / "srv.py"
    srv.write_text(FAKE_SERVER.replace("POST_STATUS", str(post_status)))
    return srv


def server_cfg(tmp_path, srv, **over):
    over.setdefault("qwen_ready_timeout_s", 20.0)
    return dataclasses.replace(
        Config(), tts_engine="qwen3", qwen_server_cmd=[sys.executable, str(srv)], log_dir=str(tmp_path), **over
    )


def server_log(tmp_path):
    f = tmp_path / "qwen-tts-server.log"
    return f.read_text(errors="replace") if f.exists() else "(no server log)"


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


@needs_afconvert
async def test_local_server_started_warmed_and_stopped(tmp_path, caplog):
    caplog.set_level("INFO", logger="claude_voice.qwen_tts")
    q = QwenTts(server_cfg(tmp_path, write_fake_server(tmp_path)), FakeSay())
    await q.start()
    proc = q._proc
    msg = f"{caplog.text}\n--- server log ---\n{server_log(tmp_path)}"
    assert proc is not None and proc.poll() is None, msg
    assert q._http is not None, msg
    assert "--host" in proc.args and "127.0.0.1" in proc.args
    await q.close()
    assert proc.poll() is not None


@needs_afconvert
async def test_managed_server_exit_after_warmup_uses_say(tmp_path, caplog):
    caplog.set_level("INFO", logger="claude_voice.qwen_tts")
    q = QwenTts(server_cfg(tmp_path, write_fake_server(tmp_path), qwen_skip_s=0.0), FakeSay())
    await q.start()
    msg = f"{caplog.text}\n--- server log ---\n{server_log(tmp_path)}"
    assert q._proc is not None and q._http is not None, msg
    proc = q._proc
    proc.kill()
    proc.wait()
    assert await q.synth("一。") == SAY_PCM
    assert await q.synth("二。") == SAY_PCM
    assert q._http is None and q._proc is None
    exits = [r for r in caplog.records if r.levelname == "ERROR" and "exited with" in r.getMessage()]
    assert len(exits) == 1
    await q.close()


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


@needs_afconvert
async def test_warmup_uses_ready_timeout():
    seen = []

    def handler(req):
        seen.append(req.extensions["timeout"]["read"])
        return httpx.Response(200, content=wav24k())

    q, say = engine(Server(handler), tts_timeout_s=5.0, qwen_ready_timeout_s=77.0)
    await q.start()
    await q.synth("好。")
    assert seen == [77.0, 5.0]
    assert say.texts == []


@needs_afconvert
async def test_slow_warmup_longer_than_tts_timeout_is_ready():
    async def slow(req):
        await asyncio.sleep(0.4)
        return httpx.Response(200, content=wav24k())

    q, say = engine(slow, tts_timeout_s=0.1, qwen_ready_timeout_s=5.0)
    await q.start()
    assert q._http is not None and q._skip_until == 0.0


async def test_own_server_failed_warmup_drops_client(tmp_path, caplog):
    caplog.set_level("INFO", logger="claude_voice.qwen_tts")
    srv = write_fake_server(tmp_path, post_status=500)
    say = FakeSay()
    q = QwenTts(server_cfg(tmp_path, srv, qwen_skip_s=0.0), say)
    await q.start()
    msg = f"{caplog.text}\n--- server log ---\n{server_log(tmp_path)}"
    assert "not ready in time" not in caplog.text, msg
    assert "HTTPStatusError" in caplog.text, msg
    assert server_log(tmp_path).count("POST") == 1, msg
    assert q._http is None and q._proc is None
    assert await q.synth("好。") == SAY_PCM


@needs_afconvert
async def test_url_mode_failed_warmup_retries_after_skip():
    server = Server(failures()["status"])
    q, say = engine(server, qwen_skip_s=0.2)
    await q.start()
    assert q._http is not None
    n = len(server.requests)
    await q.synth("一。")
    assert len(server.requests) == n
    server.handler = lambda req: httpx.Response(200, content=wav24k())
    await asyncio.sleep(0.25)
    assert await q.synth("二。") != SAY_PCM
