"""PSK storage: macOS Keychain generic passwords (service = config, account = PSK identity)."""

from __future__ import annotations

import re
import subprocess
from typing import Protocol

from .protocol import PSK_IDENTITY_PREFIX

SECURITY = "/usr/bin/security"
_IDENTITY = re.compile(re.escape(PSK_IDENTITY_PREFIX) + r"[0-9A-Fa-f]{12}")
_SERIAL = re.compile(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}")
_SERVICE = re.compile(r"[A-Za-z0-9._-]{1,64}")
_ACCOUNT_LINE = re.compile(r'"acct"<blob>="([^"]*)"')
_SERVICE_LINE = re.compile(r'"svce"<blob>="([^"]*)"')
_DUMP_ITEM = re.compile(r"^keychain: ", re.MULTILINE)
_NOT_FOUND = 44


class KeychainError(RuntimeError):
    pass


def valid_identity(identity: str | None) -> bool:
    return bool(identity) and _IDENTITY.fullmatch(identity) is not None


def psk_identity(serial: str) -> str:
    """`clawd-` followed by the USB serial number without colons."""
    if not _SERIAL.fullmatch(serial):
        raise ValueError(f"not a USB serial number of the form AA:BB:CC:DD:EE:FF: {serial!r}")
    return PSK_IDENTITY_PREFIX + serial.replace(":", "")


class PskStore(Protocol):
    def get(self, identity: str) -> bytes | None: ...
    def set(self, identity: str, key: bytes) -> None: ...
    def delete(self, identity: str) -> bool: ...
    def find_any(self) -> str | None: ...
    def identities(self) -> list[str]: ...


class MemoryStore:
    """In-process store for tests."""

    def __init__(self) -> None:
        self.items: dict[str, bytes] = {}

    def get(self, identity: str) -> bytes | None:
        return self.items.get(identity)

    def set(self, identity: str, key: bytes) -> None:
        self.items[identity] = bytes(key)

    def delete(self, identity: str) -> bool:
        return self.items.pop(identity, None) is not None

    def find_any(self) -> str | None:
        return next(iter(self.items), None)

    def identities(self) -> list[str]:
        return list(self.items)


class KeychainStore:
    """Keys go to `security` on stdin so they never appear in a process argument list."""

    def __init__(self, service: str, timeout_s: float, security: str = SECURITY) -> None:
        if not _SERVICE.fullmatch(service):
            raise ValueError(f"invalid Keychain service name: {service!r}")
        self.service = service
        self._security = security
        self._timeout = timeout_s

    def _run(self, args: list[str], stdin: str | None = None) -> subprocess.CompletedProcess:
        try:
            return subprocess.run(
                [self._security, *args], input=stdin, capture_output=True, text=True, timeout=self._timeout, check=False
            )
        except subprocess.TimeoutExpired:
            raise KeychainError(f"security {args[0]} timed out") from None

    @staticmethod
    def _check(identity: str) -> None:
        if not valid_identity(identity):
            raise ValueError(f"invalid PSK identity: {identity!r}")

    def get(self, identity: str) -> bytes | None:
        self._check(identity)
        r = self._run(["find-generic-password", "-s", self.service, "-a", identity, "-w"])
        if r.returncode == _NOT_FOUND:
            return None
        if r.returncode != 0:
            raise KeychainError(f"security find-generic-password exited with {r.returncode}")
        try:
            key = bytes.fromhex(r.stdout.strip())
        except ValueError:
            raise KeychainError(f"Keychain item {self.service}/{identity} is not a hex key") from None
        return key or None

    def set(self, identity: str, key: bytes) -> None:
        self._check(identity)
        line = f"add-generic-password -U -s {self.service} -a {identity} -w {key.hex()}\n"
        r = self._run(["-i"], stdin=line)
        if r.returncode != 0:
            raise KeychainError(f"security add-generic-password exited with {r.returncode}")

    def delete(self, identity: str) -> bool:
        self._check(identity)
        r = self._run(["delete-generic-password", "-s", self.service, "-a", identity])
        if r.returncode == _NOT_FOUND:
            return False
        if r.returncode != 0:
            raise KeychainError(f"security delete-generic-password exited with {r.returncode}")
        return True

    def find_any(self) -> str | None:
        r = self._run(["find-generic-password", "-s", self.service])
        if r.returncode != 0:
            return None
        m = _ACCOUNT_LINE.search(r.stdout)
        return m.group(1) if m and valid_identity(m.group(1)) else None

    def identities(self) -> list[str]:
        """Every valid PSK identity under this service, from item attributes only (no passwords are read)."""
        r = self._run(["dump-keychain"])
        if r.returncode != 0:
            raise KeychainError(f"security dump-keychain exited with {r.returncode}")
        found: list[str] = []
        for item in _DUMP_ITEM.split(r.stdout):
            if 'class: "genp"' not in item:
                continue
            service, account = _SERVICE_LINE.search(item), _ACCOUNT_LINE.search(item)
            if service and account and service.group(1) == self.service and valid_identity(account.group(1)):
                if account.group(1) not in found:
                    found.append(account.group(1))
        return found
