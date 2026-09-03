"""A minimal NDJSON client for the ledger engine's JSON port.

Deliberately hand-written rather than generated from the C++ definitions: if the
test client shared code with the server, a mistake in that shared code would
cancel itself out and the test would pass anyway. The same reasoning is why the
C++ integration tests hand-roll their own blocking socket client.

Every integer crosses this boundary as a *string*. See `assert_integers_quoted`
and the note in README.md — JSON numbers are IEEE-754 doubles in most languages,
so an int64 amount sent unquoted can come back silently changed.
"""

from __future__ import annotations

import json
import socket
from typing import Any


class ProtocolError(RuntimeError):
    """The server closed the connection or sent something unparseable."""


class LedgerClient:
    """One connection to the NDJSON port. One line out, one line back."""

    def __init__(self, port: int, host: str = "127.0.0.1", timeout: float = 10.0) -> None:
        self._sock = socket.create_connection((host, port), timeout=timeout)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self._inbox = b""

    # --- lifetime ---------------------------------------------------------

    def close(self) -> None:
        try:
            self._sock.close()
        except OSError:
            pass

    def __enter__(self) -> "LedgerClient":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    # --- raw line level ---------------------------------------------------

    def send_line(self, line: str) -> None:
        self._sock.sendall(line.encode() + b"\n")

    def recv_line(self) -> str:
        """Read one newline-delimited line, or raise if the peer hung up."""
        while b"\n" not in self._inbox:
            chunk = self._sock.recv(65536)
            if not chunk:
                raise ProtocolError("connection closed while waiting for a line")
            self._inbox += chunk
        line, _, self._inbox = self._inbox.partition(b"\n")
        return line.decode()

    def at_eof(self) -> bool:
        """True if the server has closed the connection (a framing error should)."""
        try:
            while True:
                chunk = self._sock.recv(65536)
                if not chunk:
                    return True
                self._inbox += chunk
        except (socket.timeout, TimeoutError):
            return False
        except OSError:
            return False

    # --- message level ----------------------------------------------------

    def request(self, **fields: Any) -> dict[str, Any]:
        """Send one request object and return the decoded response object."""
        self.send_line(json.dumps(fields))
        return json.loads(self.recv_line())

    def ping(self, req_id: str = "1") -> dict[str, Any]:
        return self.request(id=req_id, type="ping")

    def get_account(self, account_id: int | str, req_id: str = "1") -> dict[str, Any]:
        return self.request(id=req_id, type="get_account", account_id=str(account_id))

    def transfer(
        self,
        idem_key: str,
        src: int | str,
        dst: int | str,
        amount: int | str,
        ccy: str = "USD",
        req_id: str = "1",
    ) -> dict[str, Any]:
        # `from` is a Python keyword, so it cannot be a kwarg — build the dict.
        return self.request(
            id=req_id,
            type="transfer",
            idem_key=idem_key,
            **{"from": str(src)},
            to=str(dst),
            amount=str(amount),
            ccy=ccy,
        )


    def get_stats(self, req_id: str = "1") -> dict[str, Any]:
        return self.request(id=req_id, type="get_stats")


def balance_of(client: LedgerClient, account_id: int) -> int:
    """Convenience: read one balance as an int. Raises if the account is missing."""
    resp = client.get_account(account_id)
    if resp["type"] != "account":
        raise ProtocolError(f"expected an account response, got {resp}")
    return int(resp["balance"])
