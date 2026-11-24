import importlib.util
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("devctl", Path(__file__).parents[2] / "firmware" / "tools" / "devctl.py")
devctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(devctl)


def test_parse_batt_reads_every_field_of_the_log_line():
    line = b"I (5000) app: batt pct=73 present=1 vbus=1 dir=1 chg=2 mv=3912 bars=3 look=charging"
    assert devctl.parse_batt(line) == dict(pct=73, present=1, vbus=1, dir=1, chg=2, mv=3912, bars=3, look="charging")


def test_parse_batt_keeps_a_missing_voltage_as_minus_one():
    line = b"I (9) app: batt pct=0 present=0 vbus=0 dir=0 chg=0 mv=-1 bars=0 look=hidden"
    assert devctl.parse_batt(line)["mv"] == -1


def test_parse_batt_ignores_other_lines():
    assert devctl.parse_batt(b"I (5000) app: bright lvl=49 eff=49 panel=49 set=1") is None


def test_parse_batt_reports_a_stale_line():
    assert devctl.parse_batt(b"W (123) app: batt: stale") == {"look": "stale"}
    assert devctl.parse_batt(b"\x1b[0;33mW (123) app: batt: stale\x1b[0m") == {"look": "stale"}


def test_parse_batt_ignores_stale_text_in_other_logs():
    assert devctl.parse_batt(b"I (5) net: joining batt: stale") is None
