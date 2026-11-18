import pytest
import serial


@pytest.fixture(autouse=True)
def no_real_serial_ports(monkeypatch):
    """Tests never open a real serial port: another process may own the board."""

    def refuse(self):
        raise AssertionError(f"test tried to open a real serial port: {self.port}")

    monkeypatch.setattr(serial.Serial, "open", refuse)
    monkeypatch.setattr("serial.tools.list_ports.comports", lambda: [])
