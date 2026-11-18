import importlib.util
from pathlib import Path

SPEC = importlib.util.spec_from_file_location("devctl", Path(__file__).parents[2] / "firmware" / "tools" / "devctl.py")
devctl = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(devctl)


def line(first):
    triples = ";".join("%d,%d,%d" % (first + k, -(first + k), 4096) for k in range(5))
    return b"I (1) imu: imu_dump i=%d %s" % (first, triples.encode())


def test_contiguous_trace_is_accepted():
    rows, gaps = devctl.parse_imu_dump([line(0), line(5), line(10), b"I (2) imu: imu_dump done n=15"])
    assert gaps == 0
    assert [r[0] for r in rows] == list(range(15))
    assert rows[7] == (7, 7, -7, 4096)


def test_missing_line_is_a_gap():
    _, gaps = devctl.parse_imu_dump([line(0), line(10), b"imu_dump done n=15"])
    assert gaps == 1


def test_lost_tail_is_a_gap():
    _, gaps = devctl.parse_imu_dump([line(0), line(5), b"imu_dump done n=30"])
    assert gaps == 1


def test_partial_final_batch_is_not_a_gap():
    _, gaps = devctl.parse_imu_dump([line(0), line(5), b"imu_dump done n=13"])
    assert gaps == 0
