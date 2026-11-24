import ast
import asyncio
import base64
import contextlib
import dataclasses
import json
import os
import re
import select
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

from test_qwen_tts import SAY_PCM, FakeSay, needs_afconvert, write_fake_server
from test_relay import until

from claude_voice import config as config_mod
from claude_voice import qwen_tts as qwen_mod
from claude_voice.config import Config, load
from claude_voice.qwen_tts import QwenTts
from claude_voice.stt import free_port

SSH_ON = 'qwen_ssh_host = "speech-mac"\nqwen_server_cmd = ["/opt/venv/bin/python", "-m", "mlx_audio.server"]\n'


def write_config(tmp_path, text):
    p = tmp_path / "c.toml"
    p.write_text(text, encoding="utf-8")
    return p


def test_config_defaults_leave_ssh_off(tmp_path):
    cfg = load(write_config(tmp_path, SSH_ON))
    assert cfg.qwen_ssh_host == "speech-mac" and cfg.qwen_remote_port == 47803
    assert Config().qwen_ssh_host == ""


@pytest.mark.parametrize(
    ("text", "key"),
    [
        ('qwen_ssh_host = "-oProxyCommand=x"\nqwen_server_cmd = ["s"]\n', "qwen_ssh_host"),
        ('qwen_ssh_host = "speech mac"\nqwen_server_cmd = ["s"]\n', "qwen_ssh_host"),
        ('qwen_ssh_host = "speech-mac\\n"\nqwen_server_cmd = ["s"]\n', "qwen_ssh_host"),
        ('qwen_ssh_host = "speech-mac"\n', "qwen_server_cmd"),
        (SSH_ON + 'qwen_remote_python = "python3"\n', "qwen_remote_python"),
        (SSH_ON + 'qwen_remote_python = "/usr/bin/python3;rm"\n', "qwen_remote_python"),
        (SSH_ON + 'qwen_remote_python = "/my venv/python"\n', "qwen_remote_python"),
        (SSH_ON + "qwen_remote_port = 0\n", "qwen_remote_port"),
        (SSH_ON + "qwen_remote_port = 65536\n", "qwen_remote_port"),
        (SSH_ON + "qwen_link_timeout_s = 0.0\n", "qwen_link_timeout_s"),
        (SSH_ON + "qwen_ssh_connect_timeout_s = -1.0\n", "qwen_ssh_connect_timeout_s"),
        (SSH_ON + "qwen_restart_min_s = 0.0\n", "qwen_restart_min_s"),
        (SSH_ON + "qwen_restart_min_s = 10.0\nqwen_restart_max_s = 5.0\n", "qwen_restart_max_s"),
        (SSH_ON + "qwen_link_timeout_s = inf\n", "qwen_link_timeout_s"),
        (SSH_ON + "qwen_ssh_connect_timeout_s = nan\n", "qwen_ssh_connect_timeout_s"),
        (SSH_ON + "qwen_restart_max_s = inf\n", "qwen_restart_max_s"),
        (SSH_ON + "qwen_remote_port = 47803.0\n", "qwen_remote_port"),
        (SSH_ON + "qwen_remote_port = true\n", "qwen_remote_port"),
    ],
)
def test_config_ssh_validation(tmp_path, text, key):
    with pytest.raises(ValueError, match=key) as e:
        load(write_config(tmp_path, text))
    assert "unknown config keys" not in str(e.value)


GUARD_FILE = Path(config_mod.__file__).with_name("speech_guard.py")
# the stdlib python a Mac ships, so the guard runs under 3.9 where it exists; its path has no spaces
SYSTEM_PYTHON = "/usr/bin/python3"

SERVER = r'''
import os, signal, socket, subprocess, sys, time
here = os.path.dirname(os.path.abspath(__file__))
port = int(sys.argv[sys.argv.index("--port") + 1])
open(os.path.join(here, "server.pid"), "w").write(str(os.getpid()))
worker = subprocess.Popen([sys.executable, "-c", "import signal,time;signal.signal(15,signal.SIG_IGN);time.sleep(60)"])
open(os.path.join(here, "worker.pid"), "w").write(str(worker.pid))
signal.signal(signal.SIGTERM, signal.SIG_IGN)
while not os.path.exists(os.path.join(here, "go")):
    time.sleep(0.01)
if os.path.exists(os.path.join(here, "crash")):
    sys.exit(3)
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen()
time.sleep(60)
'''


def remote_python(tmp_path):
    """A python path the remote command can carry: no spaces, unlike this checkout's venv."""
    if os.path.exists(SYSTEM_PYTHON):
        return SYSTEM_PYTHON
    link = tmp_path / "py"
    if not link.exists():
        link.symlink_to(os.path.realpath(sys.executable))
    return str(link)


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def wait_dead(pid, timeout=10.0):
    deadline = time.monotonic() + timeout
    while alive(pid) and time.monotonic() < deadline:
        time.sleep(0.01)
    return not alive(pid)


def pid_in(path, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists() and path.read_text().strip():
            return int(path.read_text())
        time.sleep(0.01)
    raise AssertionError(f"no pid in {path}")


class Guard:
    """The guard run directly, with a reader for its status lines."""

    def __init__(self, tmp_path, port, deadline_s=30.0, grace_s=0.3, cmd=None):
        self.dir = tmp_path / "srv"
        self.dir.mkdir(exist_ok=True)
        (self.dir / "srv.py").write_text(SERVER)
        cmd = cmd or [remote_python(tmp_path), "-I", str(self.dir / "srv.py")]
        payload = {
            "cmd": [*cmd, "--host", "127.0.0.1", "--port", str(port)], "port": port,
            "deadline_s": deadline_s, "grace_s": grace_s,
        }
        b64 = base64.b64encode(json.dumps(payload).encode()).decode()
        self.err = open(tmp_path / "guard.err", "ab")
        self.proc = subprocess.Popen(
            [remote_python(tmp_path), "-I", str(GUARD_FILE), b64],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.err,
        )
        self.lines: list[str] = []
        self._buf = b""

    def until_line(self, text, timeout=10.0, beat=False):
        deadline = time.monotonic() + timeout
        while text not in self.lines:
            left = deadline - time.monotonic()
            if left <= 0:
                raise AssertionError(f"no {text!r} in {self.lines}")
            if beat:
                self.proc.stdin.write(b".")
                self.proc.stdin.flush()
            ready, _, _ = select.select([self.proc.stdout], [], [], min(left, 0.1))
            if not ready:
                continue
            chunk = os.read(self.proc.stdout.fileno(), 4096)
            if not chunk:
                raise AssertionError(f"guard stdout closed before {text!r}; got {self.lines}")
            self._buf += chunk
            *done, self._buf = self._buf.split(b"\n")
            self.lines += [d.decode() for d in done]

    def finish(self):
        for pid_file in ("server.pid", "worker.pid"):
            f = self.dir / pid_file
            if f.exists() and f.read_text().strip() and alive(int(f.read_text())):
                os.kill(int(f.read_text()), signal.SIGKILL)
        if self.proc.poll() is None:
            self.proc.kill()
        self.proc.wait()
        self.err.close()


@pytest.fixture
def guards():
    made = []
    yield made
    for g in made:
        g.finish()


def test_guard_parses_on_python39_and_is_stdlib_only():
    tree = ast.parse(GUARD_FILE.read_text(), feature_version=(3, 9))
    names = {a.name.split(".")[0] for n in ast.walk(tree) if isinstance(n, ast.Import) for a in n.names}
    names |= {n.module.split(".")[0] for n in ast.walk(tree) if isinstance(n, ast.ImportFrom)}
    assert names <= {"base64", "json", "os", "select", "signal", "socket", "subprocess", "sys", "time"}


@pytest.mark.parametrize("reason", ["eof", "heartbeat", "signal"])
def test_guard_stops_the_server_on_eof_heartbeat_or_signal(tmp_path, guards, reason):
    port = free_port()
    g = Guard(tmp_path, port, deadline_s=0.5 if reason == "heartbeat" else 30.0)
    guards.append(g)
    (g.dir / "go").touch()
    g.until_line("clawd-speech listening", beat=True)
    server = pid_in(g.dir / "server.pid")
    if reason == "eof":
        g.proc.stdin.close()
    elif reason == "signal":
        g.proc.send_signal(signal.SIGTERM)
    g.until_line(f"clawd-speech stop {reason}")
    assert g.proc.wait(10) == 128 + signal.SIGKILL
    assert wait_dead(server)


def test_guard_keeps_the_server_while_beats_arrive(tmp_path, guards):
    g = Guard(tmp_path, free_port(), deadline_s=1.0)
    guards.append(g)
    (g.dir / "go").touch()
    for _ in range(25):
        g.proc.stdin.write(b".")
        g.proc.stdin.flush()
        time.sleep(0.1)
    assert g.proc.poll() is None
    g.until_line("clawd-speech listening")
    assert all("stop" not in line for line in g.lines)


@pytest.mark.parametrize("end", ["eof", "server exit"])
def test_guard_kills_server_process_group(tmp_path, guards, end):
    g = Guard(tmp_path, free_port())
    guards.append(g)
    if end == "server exit":
        (g.dir / "crash").touch()
    (g.dir / "go").touch()
    worker = pid_in(g.dir / "worker.pid")
    if end == "eof":
        g.until_line("clawd-speech listening")
        g.proc.stdin.close()
        g.until_line("clawd-speech stop eof")
    else:
        g.until_line("clawd-speech exit 3")
        assert g.proc.wait(10) == 3
    assert wait_dead(worker)


def test_guard_reports_busy_port(tmp_path, guards):
    holder = socket.socket()
    holder.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    holder.bind(("127.0.0.1", 0))
    holder.listen()
    port = holder.getsockname()[1]
    try:
        g = Guard(tmp_path, port)
        guards.append(g)
        g.until_line(f"clawd-speech busy {port}")
        assert g.proc.wait(10) != 0
        assert not (g.dir / "server.pid").exists()
    finally:
        holder.close()


def test_guard_stops_when_its_stdout_is_gone(tmp_path, guards):
    g = Guard(tmp_path, free_port())
    guards.append(g)
    server = pid_in(g.dir / "server.pid")
    g.proc.stdout.close()
    (g.dir / "go").touch()
    assert g.proc.wait(10) == 128 + signal.SIGKILL
    assert wait_dead(server)


FAKE_SSH = r'''
import json, os, socket, subprocess, sys, threading, time
here = os.path.dirname(os.path.abspath(__file__))
mode = json.load(open(os.path.join(here, "mode.json")))
runs_path = os.path.join(here, "runs")
n = int(open(runs_path).read()) + 1 if os.path.exists(runs_path) else 1
open(runs_path, "w").write(str(n))
argv = sys.argv[1:]
record = {"argv": argv, "pid": os.getpid()}
def save():
    with open(os.path.join(here, "runs.jsonl"), "a") as f:
        f.write(json.dumps(record) + "\n")
plan = mode.get("plan", [])
if (plan[n - 1] if n <= len(plan) else "ok") == "unreachable":
    save()
    print("ssh: connect to host speech-mac port 22: Operation timed out", file=sys.stderr, flush=True)
    sys.exit(255)
_, lport, _, rport = argv[argv.index("-L") + 1].split(":")
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    listener.bind(("127.0.0.1", int(lport)))
except OSError:
    sys.exit(255)
listener.listen()
def pump(src, dst):
    try:
        while True:
            data = src.recv(65536)
            if not data:
                break
            dst.sendall(data)
    except OSError:
        pass
    for s in (src, dst):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
def forward():
    while True:
        c, _ = listener.accept()
        try:
            r = socket.create_connection(("127.0.0.1", int(rport)))
        except OSError:
            c.close()
            continue
        threading.Thread(target=pump, args=(c, r), daemon=True).start()
        threading.Thread(target=pump, args=(r, c), daemon=True).start()
threading.Thread(target=forward, daemon=True).start()
print("Last login: a line from a chatty dotfile", flush=True)
remote = argv[argv.index("--") + 2]
guard = subprocess.Popen(["/bin/zsh", "-f", "-c", remote], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
record["guard_pid"] = guard.pid
save()
frozen = threading.Event()
def stdin_relay():
    while True:
        data = os.read(0, 4096)
        if not data:
            break
        if not frozen.is_set():
            guard.stdin.write(data)
            guard.stdin.flush()
    guard.stdin.close()
def stdout_relay():
    for line in guard.stdout:
        with open(os.path.join(here, "guard-out"), "ab") as f:
            f.write(line)
        if not frozen.is_set():
            try:
                sys.stdout.buffer.write(line)
                sys.stdout.flush()
            except OSError:
                pass
threading.Thread(target=stdin_relay, daemon=True).start()
out = threading.Thread(target=stdout_relay, daemon=True)
out.start()
while guard.poll() is None:
    for name, act in (("drop", "drop"), ("freeze", "freeze")):
        path = os.path.join(here, name)
        if os.path.exists(path):
            os.unlink(path)
            if act == "drop":
                os._exit(255)
            frozen.set()
    time.sleep(0.01)
out.join(2)
sys.exit(255 if frozen.is_set() else guard.returncode)
'''

GATE = r'''
import os, runpy, sys, time
here = os.path.dirname(os.path.abspath(__file__))
with open(os.path.join(here, "server.pids"), "a") as f:
    f.write(f"{os.getpid()}\n")
while os.path.exists(os.path.join(here, "hold")):
    time.sleep(0.01)
srv = os.path.join(here, "srv.py")
sys.argv = [srv, *sys.argv[1:]]
runpy.run_path(srv, run_name="__main__")
'''


class Remote:
    """A fake ssh and a fake speech server behind it, with the pids they record."""

    def __init__(self, tmp_path, **mode):
        self.dir = tmp_path / "ssh"
        self.dir.mkdir()
        (self.dir / "fake.py").write_text(FAKE_SSH)
        (self.dir / "mode.json").write_text(json.dumps(mode))
        self.bin = self.dir / "ssh"
        self.bin.write_text(f'#!/bin/sh\nexec "{sys.executable}" -I "{self.dir / "fake.py"}" "$@"\n')
        self.bin.chmod(0o755)
        self.srv = tmp_path / "speech"
        self.srv.mkdir()
        write_fake_server(self.srv)
        (self.srv / "gate.py").write_text(GATE)
        self.log_dir = tmp_path / "logs"

    def cfg(self, tmp_path, **over):
        base = dict(
            tts_engine="qwen3", qwen_ssh_host="speech-mac", qwen_ssh_bin=str(self.bin),
            qwen_server_cmd=[sys.executable, "-I", str(self.srv / "gate.py")],
            qwen_remote_python=remote_python(tmp_path), qwen_remote_port=free_port(), qwen_link_timeout_s=5.0,
            qwen_restart_min_s=0.05, qwen_restart_max_s=0.2, qwen_ready_timeout_s=20.0, log_dir=str(self.log_dir),
        )
        return dataclasses.replace(Config(), **{**base, **over})

    def runs(self):
        f = self.dir / "runs.jsonl"
        return [json.loads(line) for line in f.read_text().splitlines()] if f.exists() else []

    def servers(self):
        f = self.srv / "server.pids"
        return [int(x) for x in f.read_text().split()] if f.exists() else []

    def guard_out(self):
        f = self.dir / "guard-out"
        return f.read_text() if f.exists() else ""

    def log(self):
        f = self.log_dir / "qwen-tts-server.log"
        return f.read_text(errors="replace") if f.exists() else "(no server log)"

    def pids(self):
        return [*(r["pid"] for r in self.runs()), *(r["guard_pid"] for r in self.runs() if "guard_pid" in r),
                *self.servers()]


@pytest.fixture
async def remotes():
    """Every engine is closed and every recorded process killed, even when a test fails half-way."""
    made: list[tuple[Remote, list]] = []
    yield made
    for remote, engines in made:
        for q in engines:
            with contextlib.suppress(Exception):
                await q.close()
        for pid in remote.pids():
            if alive(pid):
                os.kill(pid, signal.SIGKILL)


async def engine(tmp_path, remotes, say=None, **over):
    remote = Remote(tmp_path, plan=over.pop("plan", []))
    if over.pop("hold", False):
        (remote.srv / "hold").touch()
    q = QwenTts(remote.cfg(tmp_path, **over), say or FakeSay(), grace_s=0.3, stop_margin_s=10.0)
    remotes.append((remote, [q]))
    return q, remote


async def ready(q, remote, n=1):
    await until(lambda: (q._http is not None and len(remote.runs()) == n) or len(remote.runs()) > n, timeout=30)
    assert q._http is not None and len(remote.runs()) == n, remote.log()


def test_ssh_argv_neutralises_user_config_and_binds_loopback(tmp_path):
    cfg = dataclasses.replace(Config(), qwen_ssh_host="speech-mac", qwen_server_cmd=["s"], qwen_link_timeout_s=10.0)
    argv = QwenTts(cfg, FakeSay())._ssh_argv(50123)
    assert argv[:2] == ["/usr/bin/ssh", "-T"]
    opts = [argv[i + 1] for i, a in enumerate(argv) if a == "-o"]
    assert opts == [
        "BatchMode=yes", "StrictHostKeyChecking=yes", "UpdateHostKeys=no", "ExitOnForwardFailure=yes",
        "ConnectTimeout=10", "ServerAliveInterval=2",
        "ServerAliveCountMax=5", "ControlMaster=no", "ControlPath=none", "RemoteCommand=none", "RequestTTY=no",
        "SessionType=default", "StdinNull=no", "ForkAfterAuthentication=no", "ForwardAgent=no", "ForwardX11=no",
        "PermitLocalCommand=no",
    ]
    assert argv[-5:-1] == ["-L", "127.0.0.1:50123:127.0.0.1:47803", "--", "speech-mac"]


def test_remote_command_holds_only_fixed_text_python_and_base64():
    cfg = dataclasses.replace(
        Config(), qwen_ssh_host="speech-mac", qwen_remote_python="/opt/venv/bin/python3.12",
        qwen_server_cmd=["/my venv/python", "it's", '"q"', "$(x)"],
    )
    cmd = QwenTts(cfg, FakeSay())._remote_cmd()
    m = re.fullmatch(
        r'exec /opt/venv/bin/python3\.12 -I -c "import sys,base64;exec\(base64\.b64decode\(sys\.argv\[1\]\)\)"'
        r" ([A-Za-z0-9+/=]+) ([A-Za-z0-9+/=]+)",
        cmd,
    )
    assert m is not None, cmd
    assert base64.b64decode(m.group(1)) == GUARD_FILE.read_bytes()
    assert json.loads(base64.b64decode(m.group(2)))["cmd"][:4] == cfg.qwen_server_cmd


def test_remote_command_starts_the_server_argv_exactly_through_a_shell(tmp_path):
    out = tmp_path / "argv.json"
    odd = ["a b", "it's", 'say "hi"', "$HOME", "`x`;y"]
    script = f"import json,sys;open({str(out)!r},'w').write(json.dumps(sys.argv[1:]))"
    cfg = dataclasses.replace(
        Config(), qwen_ssh_host="speech-mac", qwen_remote_python=remote_python(tmp_path),
        qwen_server_cmd=[sys.executable, "-c", script, *odd], qwen_remote_port=free_port(),
    )
    cmd = QwenTts(cfg, FakeSay(), grace_s=0.3)._remote_cmd()
    beat_end, ours = os.pipe()  # held open, so the guard waits for the server instead of stopping it
    try:
        done = subprocess.run(["/bin/zsh", "-f", "-c", cmd], stdin=beat_end, capture_output=True, timeout=30)
    finally:
        os.close(beat_end)
        os.close(ours)
    assert done.stdout.endswith(b"clawd-speech exit 0\n"), done.stderr
    assert json.loads(out.read_text()) == [*odd, "--host", "127.0.0.1", "--port", str(cfg.qwen_remote_port)]


@needs_afconvert
async def test_ssh_mode_ready_and_speaks_through_forward(tmp_path, remotes, caplog):
    caplog.set_level("INFO", logger="claude_voice.qwen_tts")
    q, remote = await engine(tmp_path, remotes)
    await q.start()
    await ready(q, remote)
    assert await q.synth("你好。") != SAY_PCM
    assert q._fallback.texts == []
    assert remote.log().count("POST") == 2
    assert "Last login" in remote.log() and "Last login" not in caplog.text
    await q.close()


@needs_afconvert
async def test_ssh_mode_ignores_proxy_environment(tmp_path, remotes, monkeypatch):
    dead = f"http://127.0.0.1:{free_port()}"
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy"):
        monkeypatch.setenv(name, dead)
    for name in ("NO_PROXY", "no_proxy"):
        monkeypatch.delenv(name, raising=False)
    q, remote = await engine(tmp_path, remotes)
    await q.start()
    await ready(q, remote)
    assert await q.synth("你好。") != SAY_PCM
    await q.close()


@needs_afconvert
async def test_start_does_not_block_in_ssh_mode(tmp_path, remotes):
    q, remote = await engine(tmp_path, remotes, hold=True)
    starting = asyncio.create_task(q.start())
    for _ in range(5):
        await asyncio.sleep(0)
    assert starting.done()
    await until(lambda: remote.servers(), timeout=30)
    assert await q.synth("一。") == SAY_PCM
    (remote.srv / "hold").unlink()
    await ready(q, remote)
    assert await q.synth("二。") != SAY_PCM
    assert q._fallback.texts == ["一。"]
    await q.close()


@needs_afconvert
async def test_close_stops_server_guard_and_ssh(tmp_path, remotes):
    q, remote = await engine(tmp_path, remotes)
    await q.start()
    await ready(q, remote)
    ssh, threads = q._ssh, list(q._threads)
    await q.close()
    assert ssh.returncode >= 0 and "clawd-speech stop eof" in remote.guard_out(), remote.log()
    assert not any(alive(pid) for pid in remote.pids())
    await until(lambda: not any(t.is_alive() for t in threads))
    assert q._ssh is None and q._http is None


RELAY_STUB = r'''
import asyncio, dataclasses, json, sys
from claude_voice.config import Config
from claude_voice.qwen_tts import QwenTts

class Say:
    async def synth(self, text):
        return b""

async def main(path):
    cfg = dataclasses.replace(Config(), **json.load(open(path)))
    if cfg.qwen_ssh_bin == Config().qwen_ssh_bin:
        sys.exit("refusing the real ssh")
    await QwenTts(cfg, Say(), grace_s=0.3, stop_margin_s=0.5).start()
    print("up", flush=True)
    await asyncio.sleep(3600)

asyncio.run(main(sys.argv[1]))
'''


async def test_relay_sigkill_stops_remote_server(tmp_path, remotes):
    remote = Remote(tmp_path)
    remotes.append((remote, []))
    (remote.srv / "hold").touch()
    cfg = remote.cfg(tmp_path)
    (tmp_path / "cfg.json").write_text(json.dumps(dataclasses.asdict(cfg)))
    (tmp_path / "relay_stub.py").write_text(RELAY_STUB)
    relay = await asyncio.create_subprocess_exec(
        sys.executable, str(tmp_path / "relay_stub.py"), str(tmp_path / "cfg.json"), stdout=asyncio.subprocess.PIPE
    )
    try:
        assert await asyncio.wait_for(relay.stdout.readline(), 30) == b"up\n"
        await until(lambda: remote.servers(), timeout=30)
        server = remote.servers()[0]
        relay.kill()
        await relay.wait()
        await until(lambda: not alive(server), timeout=30)
        await until(lambda: "clawd-speech stop eof" in remote.guard_out(), timeout=30)
    finally:
        if relay.returncode is None:
            relay.kill()
            await relay.wait()


@needs_afconvert
async def test_silent_link_loss_stops_server_by_heartbeat_deadline(tmp_path, remotes, caplog):
    q, remote = await engine(tmp_path, remotes, qwen_link_timeout_s=2.0, qwen_restart_min_s=30.0,
                             qwen_restart_max_s=30.0)
    await q.start()
    await ready(q, remote)
    server = remote.servers()[0]
    (remote.dir / "freeze").touch()
    await until(lambda: q._http is None, timeout=30)
    assert "clawd-speech stop heartbeat" in remote.guard_out()
    assert not alive(server)
    assert await q.synth("好。") == SAY_PCM
    await q.close()


@needs_afconvert
async def test_heartbeat_survives_blocked_event_loop(tmp_path, remotes):
    q, remote = await engine(tmp_path, remotes, qwen_link_timeout_s=2.0)
    await q.start()
    await ready(q, remote)
    time.sleep(4.0)  # blocks the loop for two link timeouts
    assert q._ssh.poll() is None and all(alive(pid) for pid in remote.servers())
    assert "stop" not in remote.guard_out()
    await q.close()


@needs_afconvert
async def test_link_drop_falls_back_then_reconnects(tmp_path, remotes, caplog):
    caplog.set_level("INFO", logger="claude_voice.qwen_tts")
    q, remote = await engine(tmp_path, remotes)
    await q.start()
    await ready(q, remote)
    (remote.dir / "drop").touch()
    await until(lambda: q._http is None, timeout=30)
    assert await q.synth("一。") == SAY_PCM
    await ready(q, remote, n=2)
    assert await q.synth("二。") != SAY_PCM
    assert q._fallback.texts == ["一。"]
    warnings = [r.getMessage() for r in caplog.records if r.levelname == "WARNING"]
    assert len(warnings) == 1 and "exited with 255" in warnings[0], warnings
    assert len([r for r in caplog.records if "ready, warm-up" in r.getMessage()]) == 2
    await q.close()


async def test_unreachable_host_backs_off_and_logs_once(tmp_path, remotes, caplog):
    caplog.set_level("INFO", logger="claude_voice.qwen_tts")
    q, remote = await engine(
        tmp_path, remotes, plan=["unreachable"] * 20, qwen_restart_min_s=0.01, qwen_restart_max_s=30.0
    )
    await q.start()
    await until(lambda: len(remote.runs()) >= 5, timeout=30)
    assert await q.synth("好。") == SAY_PCM
    await q.close()
    delays = [r.args[1] for r in caplog.records if r.getMessage().startswith("retrying qwen3")]
    assert delays[:4] == [0.01, 0.02, 0.04, 0.08]
    warnings = [r.getMessage() for r in caplog.records if r.levelname == "WARNING"]
    assert len(warnings) == 1 and "exited with 255" in warnings[0], warnings


@needs_afconvert
async def test_server_crash_restarts_with_backoff(tmp_path, remotes, caplog):
    caplog.set_level("INFO", logger="claude_voice.qwen_tts")
    q, remote = await engine(tmp_path, remotes)
    await q.start()
    await ready(q, remote)
    os.kill(remote.servers()[0], signal.SIGKILL)
    await ready(q, remote, n=2)
    warnings = [r.getMessage() for r in caplog.records if r.levelname == "WARNING"]
    assert len(warnings) == 1 and "exited with 137" in warnings[0], warnings
    assert len(remote.servers()) == 2
    await q.close()


async def test_busy_remote_port_fails_the_attempt(tmp_path, remotes, caplog):
    holder = socket.socket()
    holder.bind(("127.0.0.1", 0))
    holder.listen()
    try:
        q, remote = await engine(tmp_path, remotes, qwen_remote_port=holder.getsockname()[1],
                                 qwen_restart_min_s=30.0, qwen_restart_max_s=30.0)
        await q.start()
        await until(lambda: any(r.levelname == "WARNING" for r in caplog.records), timeout=30)
        assert "is in use" in caplog.text and remote.servers() == []
        await q.close()
    finally:
        holder.close()


async def test_shutdown_during_startup_keeps_ownership(tmp_path, remotes, monkeypatch):
    q, remote = await engine(tmp_path, remotes, hold=True)
    await q.start()
    await until(lambda: remote.servers() and q._ssh is not None, timeout=30)
    entered, never = asyncio.Event(), asyncio.Event()
    original = qwen_mod.reaped

    async def blocking_reaped(proc, timeout_s=None):
        if timeout_s is not None and not entered.is_set():
            entered.set()
            await never.wait()
        return await original(proc, timeout_s)

    monkeypatch.setattr(qwen_mod, "reaped", blocking_reaped)
    sup = q._supervisor
    sup.cancel()
    await entered.wait()  # the supervisor's own stop is waiting for ssh
    sup.cancel()
    with contextlib.suppress(asyncio.CancelledError):
        await sup
    assert q._ssh is not None
    monkeypatch.setattr(qwen_mod, "reaped", original)
    await q.close()
    assert q._ssh is None and not any(alive(pid) for pid in remote.pids()), remote.log()


@needs_afconvert
async def test_close_after_ssh_already_died(tmp_path, remotes):
    q, remote = await engine(tmp_path, remotes, qwen_restart_min_s=30.0, qwen_restart_max_s=30.0)
    await q.start()
    await ready(q, remote)
    ssh, threads, server = q._ssh, list(q._threads), remote.servers()[0]
    os.kill(ssh.pid, signal.SIGKILL)
    await until(lambda: ssh.poll() is not None, timeout=30)
    await q.close()
    assert q._ssh is None
    await until(lambda: not any(t.is_alive() for t in threads))
    await until(lambda: not alive(server), timeout=30)
