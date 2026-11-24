import os
import subprocess

import pytest
import serial

from claude_voice.config import Config


@pytest.fixture(autouse=True)
def no_real_serial_ports(monkeypatch):
    """Tests never open a real serial port: another process may own the board."""

    def refuse(self):
        raise AssertionError(f"test tried to open a real serial port: {self.port}")

    monkeypatch.setattr(serial.Serial, "open", refuse)
    monkeypatch.setattr("serial.tools.list_ports.comports", lambda: [])


@pytest.fixture(autouse=True)
def no_real_ssh(monkeypatch):
    """Tests never start the real ssh: it would reach another machine."""
    real = subprocess.Popen
    ssh = Config().qwen_ssh_bin

    class Refusing(real):
        def __init__(self, args, *a, **kw):
            argv0 = args if isinstance(args, (str, bytes, os.PathLike)) else args[0]
            if os.fsdecode(kw.get("executable") or argv0) == ssh:
                raise AssertionError(f"test tried to start the real ssh: {args!r}")
            super().__init__(args, *a, **kw)

    monkeypatch.setattr(subprocess, "Popen", Refusing)
