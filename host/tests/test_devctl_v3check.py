import importlib.util
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location("devctl", Path(__file__).parents[2] / "firmware" / "tools" / "devctl.py")
devctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(devctl)


class StubLink:
    def __init__(self):
        self.sent = []

    def send(self, ftype, payload=b""):
        self.sent.append(ftype)

    def wait_for(self, ftype, timeout, show=True):
        return b'{"ok":true}' if ftype == devctl.T_FORGET_RESULT and devctl.T_FORGET in self.sent else None


@pytest.mark.parametrize(
    "hello,forgets",
    [
        ({"paired": False, "store": "ok"}, True),
        ({"paired": False, "store": "error"}, False),
        ({"paired": False}, False),
        ({"paired": True, "store": "ok"}, False),
        ({"paired": True, "store": "error"}, False),
    ],
    ids=["empty-store", "unreadable-store", "no-store-field", "paired", "paired-unreadable"],
)
def test_v3check_sends_forget_only_to_a_board_with_a_known_empty_store(hello, forgets, capsys):
    link = StubLink()
    assert devctl.forget_check(link, hello)
    assert (devctl.T_FORGET in link.sent) is forgets
    out = capsys.readouterr().out
    assert ("PASS FORGET while unpaired" in out) is forgets and ("SKIP FORGET" in out) is not forgets
