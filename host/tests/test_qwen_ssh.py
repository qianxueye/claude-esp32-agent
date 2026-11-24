import ast
import base64
import json
import os
import select
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

from claude_voice import config as config_mod
from claude_voice.config import Config, load
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

    def until_line(self, text, timeout=10.0):
        deadline = time.monotonic() + timeout
        while text not in self.lines:
            left = deadline - time.monotonic()
            if left <= 0:
                raise AssertionError(f"no {text!r} in {self.lines}")
            ready, _, _ = select.select([self.proc.stdout], [], [], left)
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
    g.until_line("clawd-speech listening")
    server = pid_in(g.dir / "server.pid")
    if reason == "eof":
        g.proc.stdin.close()
    elif reason == "signal":
        g.proc.send_signal(signal.SIGTERM)
    g.until_line(f"clawd-speech stop {reason}")
    assert g.proc.wait(10) == 128 + signal.SIGKILL
    assert wait_dead(server)


def test_guard_keeps_the_server_while_beats_arrive(tmp_path, guards):
    g = Guard(tmp_path, free_port(), deadline_s=0.5)
    guards.append(g)
    (g.dir / "go").touch()
    for _ in range(15):
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


def test_guard_decodes_server_argv_with_spaces_and_quotes(tmp_path, guards):
    out = tmp_path / "argv.json"
    script = f"import json,sys;open({str(out)!r},'w').write(json.dumps(sys.argv[1:]))"
    odd = ["a b", "it's", 'say "hi"', "$HOME", "`x`;y"]
    g = Guard(tmp_path, free_port(), cmd=[remote_python(tmp_path), "-c", script, *odd])
    guards.append(g)
    g.until_line("clawd-speech exit 0")
    assert json.loads(out.read_text())[:5] == odd
