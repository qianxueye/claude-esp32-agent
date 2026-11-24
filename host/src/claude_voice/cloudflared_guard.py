"""Runs cloudflared as its child and stops it when stdin reaches EOF, so it never outlives the relay."""

from __future__ import annotations

import os
import select
import signal
import subprocess
import sys

POLL_S = 0.1


def stop(child: subprocess.Popen, grace_s: float) -> int:
    """SIGTERM, then SIGKILL after `grace_s`; returns the shell-style exit status."""
    if child.poll() is None:
        child.terminate()
        try:
            child.wait(grace_s)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
    code = child.returncode
    return 128 - code if code < 0 else code


def main(argv: list[str]) -> int:
    grace_s, cmd = float(argv[0]), argv[1:]
    try:
        child = subprocess.Popen(cmd, stdin=subprocess.DEVNULL)
    except OSError as e:
        print(f"cannot start {cmd[0]}: {e}", file=sys.stderr, flush=True)
        return 127
    stopping: list[int] = []
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, lambda s, _f: stopping.append(s))
    while not stopping and child.poll() is None:
        ready, _, _ = select.select([0], [], [], POLL_S)
        if ready and not os.read(0, 4096):
            break
    return stop(child, grace_s)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
