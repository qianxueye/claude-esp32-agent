"""PSK storage: macOS Keychain generic passwords (service = config, account = PSK identity or an earlier-key account)."""

from __future__ import annotations

import dataclasses
import hashlib
import re
import subprocess
from typing import Protocol

from .protocol import PSK_IDENTITY_PREFIX

SECURITY = "/usr/bin/security"
_IDENTITY = re.compile(re.escape(PSK_IDENTITY_PREFIX) + r"[0-9A-Fa-f]{12}")
_SERIAL = re.compile(r"[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}")
_SERVICE = re.compile(r"[A-Za-z0-9._-]{1,64}")
_KID = re.compile(r"[0-9A-F]{8}")
EARLIER = "-earlier-"  # account of a key the board may still hold: identity, EARLIER, key id
# whole attribute lines only, so a label or comment quoting `"acct"<blob>=` is not read as one
_ACCOUNT_LINE = re.compile(r'^\s*"acct"<blob>="([^"]*)"$', re.MULTILINE)
_SERVICE_LINE = re.compile(r'^\s*"svce"<blob>="([^"]*)"$', re.MULTILINE)
_DUMP_ITEM = re.compile(r"^keychain: ", re.MULTILINE)
_NOT_FOUND = 44
DELETE_MAX = 16  # deletes of one identity, one per keychain in the search list


class KeychainError(RuntimeError):
    pass


class KeychainValueError(KeychainError):
    """The item exists but its value is not a hex key."""


def valid_identity(identity: str | None) -> bool:
    return bool(identity) and _IDENTITY.fullmatch(identity) is not None


def key_id(psk: bytes) -> str:
    """The PAIR `key_id`: the first 8 hex digits of SHA-256 of the PSK, in uppercase."""
    return hashlib.sha256(psk).hexdigest()[:8].upper()


def wire_identity(identity: str, kid: str) -> str:
    """The PSK identity the board sends in the TLS handshake (docs/protocol.md, Wi-Fi)."""
    return f"{identity}-{kid}"


def earlier_account(identity: str, kid: str) -> str:
    """The Keychain account of an earlier key of one board; ValueError unless both parts are well formed."""
    if not valid_identity(identity) or not _KID.fullmatch(kid):
        raise ValueError(f"invalid earlier-key account: {identity!r} {kid!r}")
    return identity + EARLIER + kid


def _earlier_kid(identity: str, account: str) -> str | None:
    prefix = identity + EARLIER
    kid = account[len(prefix):] if account.startswith(prefix) else ""
    return kid if _KID.fullmatch(kid) else None


def account_board(account: str) -> str | None:
    """The board identity an account belongs to: the account itself, or the identity of an earlier-key account."""
    if valid_identity(account):
        return account
    base = account.split(EARLIER, 1)[0]
    return base if valid_identity(base) and _earlier_kid(base, account) else None


def _boards(accounts: list[str]) -> list[str]:
    return list(dict.fromkeys(b for a in accounts if (b := account_board(a))))


def psk_identity(serial: str) -> str:
    """`clawd-` followed by the USB serial number without colons, in uppercase."""
    if not _SERIAL.fullmatch(serial):
        raise ValueError(f"not a USB serial number of the form AA:BB:CC:DD:EE:FF: {serial!r}")
    return PSK_IDENTITY_PREFIX + serial.replace(":", "").upper()


class PskStore(Protocol):
    """The current key of a board under its identity, plus earlier keys it may still hold (docs/network.md, Keys)."""

    def get(self, identity: str) -> bytes | None: ...
    def set(self, identity: str, key: bytes) -> None: ...
    def delete(self, identity: str) -> bool: ...
    def find_any(self) -> str | None: ...
    def identities(self) -> list[str]: ...
    def earlier(self, identity: str) -> list[str]: ...
    def get_earlier(self, identity: str, kid: str) -> bytes | None: ...
    def set_earlier(self, identity: str, kid: str, key: bytes) -> None: ...
    def delete_earlier(self, identity: str, kid: str) -> bool: ...


@dataclasses.dataclass
class Deleted:
    current: bool  # a current key was found and deleted
    earlier: int  # earlier keys found and deleted
    listed: bool  # every earlier key was listed and deleted


def delete_board(store: PskStore, identity: str) -> Deleted:
    """Delete the current key, which ends the board's service, then every earlier key of the board.
    A failed current delete propagates; a failed listing or earlier delete only clears `listed`."""
    done = Deleted(store.delete(identity), 0, True)
    try:
        kids = store.earlier(identity)
    except KeychainError:
        return dataclasses.replace(done, listed=False)
    for kid in kids:
        try:
            done.earlier += int(store.delete_earlier(identity, kid))
        except KeychainError:
            done.listed = False
    return done


class MemoryStore:
    """In-process store for tests; it refuses the accounts KeychainStore refuses."""

    def __init__(self) -> None:
        self.items: dict[str, bytes] = {}

    @staticmethod
    def _check(identity: str) -> str:
        if not valid_identity(identity):
            raise ValueError(f"invalid PSK identity: {identity!r}")
        return identity

    def get(self, identity: str) -> bytes | None:
        return self.items.get(self._check(identity))

    def set(self, identity: str, key: bytes) -> None:
        self.items[self._check(identity)] = bytes(key)

    def delete(self, identity: str) -> bool:
        return self.items.pop(self._check(identity), None) is not None

    def find_any(self) -> str | None:
        return next((b for a in self.items if (b := account_board(a))), None)

    def identities(self) -> list[str]:
        return _boards(list(self.items))

    def earlier(self, identity: str) -> list[str]:
        self._check(identity)
        return [kid for a in self.items if (kid := _earlier_kid(identity, a))]

    def get_earlier(self, identity: str, kid: str) -> bytes | None:
        return self.items.get(earlier_account(identity, kid))

    def set_earlier(self, identity: str, kid: str, key: bytes) -> None:
        self.items[earlier_account(identity, kid)] = bytes(key)

    def delete_earlier(self, identity: str, kid: str) -> bool:
        return self.items.pop(earlier_account(identity, kid), None) is not None


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

    def _get(self, account: str) -> bytes | None:
        r = self._run(["find-generic-password", "-s", self.service, "-a", account, "-w"])
        if r.returncode == _NOT_FOUND:
            return None
        if r.returncode != 0:
            raise KeychainError(f"security find-generic-password exited with {r.returncode}")
        try:
            key = bytes.fromhex(r.stdout.strip())
        except ValueError:
            raise KeychainValueError(f"Keychain item {self.service}/{account} is not a hex key") from None
        return key or None

    def _set(self, account: str, key: bytes) -> None:
        line = f"add-generic-password -U -s {self.service} -a {account} -w {key.hex()}\n"
        r = self._run(["-i"], stdin=line)
        if r.returncode != 0:
            raise KeychainError(f"security add-generic-password exited with {r.returncode}")

    def _delete(self, account: str) -> bool:
        """Delete every item of `account`; `security` removes only the first match in the search list per call."""
        deleted = False
        for _ in range(DELETE_MAX):
            r = self._run(["delete-generic-password", "-s", self.service, "-a", account])
            if r.returncode == _NOT_FOUND:
                return deleted
            if r.returncode != 0:
                raise KeychainError(f"security delete-generic-password exited with {r.returncode}")
            deleted = True
        raise KeychainError(f"Keychain item {self.service}/{account} still found after {DELETE_MAX} deletes")

    def get(self, identity: str) -> bytes | None:
        self._check(identity)
        return self._get(identity)

    def set(self, identity: str, key: bytes) -> None:
        self._check(identity)
        self._set(identity, key)

    def delete(self, identity: str) -> bool:
        self._check(identity)
        return self._delete(identity)

    def get_earlier(self, identity: str, kid: str) -> bytes | None:
        return self._get(earlier_account(identity, kid))

    def set_earlier(self, identity: str, kid: str, key: bytes) -> None:
        self._set(earlier_account(identity, kid), key)

    def delete_earlier(self, identity: str, kid: str) -> bool:
        return self._delete(earlier_account(identity, kid))

    def find_any(self) -> str | None:
        """The board of the first item in the service, current or earlier key."""
        r = self._run(["find-generic-password", "-s", self.service])
        if r.returncode != 0:
            return None
        m = _ACCOUNT_LINE.search(r.stdout)
        return account_board(m.group(1)) if m else None

    def _accounts(self) -> list[str]:
        """Every account under this service, from item attributes only (no passwords are read)."""
        r = self._run(["dump-keychain"])
        if r.returncode != 0:
            raise KeychainError(f"security dump-keychain exited with {r.returncode}")
        found: list[str] = []
        for item in _DUMP_ITEM.split(r.stdout):
            if 'class: "genp"' not in item:
                continue
            service, account = _SERVICE_LINE.search(item), _ACCOUNT_LINE.search(item)
            if service and account and service.group(1) == self.service and account.group(1) not in found:
                found.append(account.group(1))
        return found

    def identities(self) -> list[str]:
        """Every board identity under this service with a current or an earlier key."""
        return _boards(self._accounts())

    def earlier(self, identity: str) -> list[str]:
        """Key ids of the earlier-key items of one board."""
        self._check(identity)
        return [kid for a in self._accounts() if (kid := _earlier_kid(identity, a))]
