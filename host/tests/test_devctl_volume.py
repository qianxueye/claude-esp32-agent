import importlib.util
from pathlib import Path
from types import SimpleNamespace

SPEC = importlib.util.spec_from_file_location("devctl", Path(__file__).parents[2] / "firmware" / "tools" / "devctl.py")
devctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(devctl)


class StubLink:
    def __init__(self):
        self.sent = []

    def send(self, ftype, payload=b""):
        self.sent.append((ftype, payload))


def test_devctl_volume_is_transient_unless_save():
    link = StubLink()
    devctl.cmd_volume(link, SimpleNamespace(value=35, save=False))
    devctl.cmd_volume(link, SimpleNamespace(value=35, save=True))
    assert link.sent == [(devctl.T_SET_VOLUME, bytes([35])), (devctl.T_SET_VOLUME, bytes([35, 1]))]
    assert devctl.PROTO == 3 and devctl.T_VOLUME == 0x22
