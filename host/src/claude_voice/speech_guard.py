"""Runs the speech server on the synthesis Mac and stops it when the relay's ssh session goes away.
Sent over ssh as base64 and run by the system python, so it parses on 3.9 and imports only the stdlib."""

import base64
import json
import os
import select
import shutil
import signal
import socket
import subprocess
import sys
import time

TAG = "clawd-speech"
POLL_S = 0.1
LSOF_TIMEOUT_S = 5.0


class Gone(Exception):
    """The relay no longer reads stdout."""


def status(text):
    try:
        print(TAG, text, flush=True)
    except BrokenPipeError:
        os.dup2(os.open(os.devnull, os.O_WRONLY), sys.stdout.fileno())  # no second error at exit
        raise Gone()


def port_free(port):
    s = socket.socket()
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def listening(port):
    try:
        socket.create_connection(("127.0.0.1", port), timeout=POLL_S).close()
        return True
    except OSError:
        return False


def listener_groups(lsof, port):
    """Process groups of the processes listening on `port`, or None when lsof fails."""
    try:
        out = subprocess.run(
            [lsof, "-nP", "-iTCP:%d" % port, "-sTCP:LISTEN", "-t"],
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=LSOF_TIMEOUT_S,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    groups = set()
    for pid in out.split():
        try:
            groups.add(os.getpgid(int(pid)))
        except (OSError, ValueError):  # gone since lsof listed it
            pass
    return groups


def kill_group(pgid, sig):
    try:
        os.killpg(pgid, sig)
    except (ProcessLookupError, PermissionError):
        pass


def group_alive(pgid):
    try:
        os.killpg(pgid, 0)
        return True
    except OSError:  # EPERM here means only zombies are left, or nothing this user may signal
        return False


def stop(child, grace_s):
    """SIGTERM the server's group, SIGKILL it after `grace_s`; returns the shell-style exit status."""
    if child.poll() is None:
        kill_group(child.pid, signal.SIGTERM)
        deadline = time.monotonic() + grace_s
        while time.monotonic() < deadline:
            child.poll()  # a zombie leader would keep the group alive
            if not group_alive(child.pid):
                break
            time.sleep(POLL_S / 5)
    kill_group(child.pid, signal.SIGKILL)  # workers left by a server that exited still hold the port
    child.wait()
    code = child.returncode
    return 128 - code if code < 0 else code


def watch(child, cfg, lsof, stopping):
    """Returns why the server must stop, or None once it has exited by itself."""
    port, deadline_s = cfg["port"], cfg["deadline_s"]
    last_beat = time.monotonic()
    ready = False
    while not stopping:
        if child.poll() is not None:
            return None
        if not ready and listening(port):
            groups = listener_groups(lsof, port)  # lsof runs only once something answers on the port
            if groups and groups != {child.pid}:
                status("busy %d" % port)
                return "busy"
            if groups and child.poll() is None:
                ready = True
                status("listening")
        readable, _, _ = select.select([0], [], [], POLL_S)
        if readable:
            if not os.read(0, 4096):
                return "eof"
            last_beat = time.monotonic()
        if time.monotonic() - last_beat > deadline_s:
            return "heartbeat"
    return "signal"


def main(payload):
    cfg = json.loads(base64.b64decode(payload))
    stopping = []
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, lambda s, _f: stopping.append(s))
    lsof = "/usr/sbin/lsof" if os.path.exists("/usr/sbin/lsof") else shutil.which("lsof")
    try:
        if lsof is None:
            status("nolsof")
            return 1
        if not port_free(cfg["port"]):
            status("busy %d" % cfg["port"])
            return 1
        try:
            child = subprocess.Popen(
                cfg["cmd"], stdin=subprocess.DEVNULL, stdout=2, stderr=2, start_new_session=True
            )
        except OSError as e:
            print("cannot start %s: %s" % (cfg["cmd"][0], e), file=sys.stderr, flush=True)
            status("exit 127")
            return 127
    except Gone:
        return 1
    reason = None
    try:
        reason = watch(child, cfg, lsof, stopping)
    except Gone:
        reason = "stdout"
    finally:
        code = stop(child, cfg["grace_s"])
    try:
        status("exit %d" % code if reason is None else "stop " + reason)
    except Gone:
        pass
    return code


if __name__ == "__main__":
    sys.exit(main(sys.argv[-1]))
