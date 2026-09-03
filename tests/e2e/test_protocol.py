"""End-to-end protocol behaviour over a real socket.

Every test is Arrange-Act-Assert. Here "Arrange" is usually the `client` fixture
(a running engine plus a connected socket), so many tests start straight at Act.
See instruction.md for the convention.
"""

from __future__ import annotations

import json

import pytest

from client import LedgerClient, ProtocolError

# The demo accounts the engine seeds at startup. Documented in README.md and
# produced by seedDemoAccounts() in src/core/LedgerRequestHandler.cpp.
ALICE_USD = 1001
BOB_USD = 2002
ALICE_JPY = 1003

ALICE_START = 115_000  # $1,150.00
BOB_START = 47_000  # $470.00
ALICE_JPY_START = 5_000  # ¥5,000 — exponent 0, not ¥50.00


class TestHandshake:
    def test_ping_gets_a_pong_with_the_same_id(self, client: LedgerClient) -> None:
        # Act
        resp = client.ping(req_id="7")

        # Assert
        assert resp["type"] == "pong"
        assert resp["id"] == "7", "the request id must be echoed so clients can match replies"
        assert resp["v"] == 1

    def test_version_may_be_omitted_by_hand_typed_clients(self, client: LedgerClient) -> None:
        # Arrange —— no "v" field at all, exactly what someone types into nc.
        line = json.dumps({"id": "1", "type": "ping"})

        # Act
        client.send_line(line)
        resp = json.loads(client.recv_line())

        # Assert
        assert resp["type"] == "pong"

    def test_blank_lines_do_not_disconnect(self, client: LedgerClient) -> None:
        # Act —— someone pressed Enter a few extra times.
        client.send_line("")
        client.send_line("")
        resp = client.ping(req_id="3")

        # Assert
        assert resp["type"] == "pong"
        assert resp["id"] == "3"


class TestAccountLookup:
    def test_returns_the_seeded_balance(self, client: LedgerClient) -> None:
        # Act
        resp = client.get_account(ALICE_USD)

        # Assert
        assert resp["type"] == "account"
        assert resp["id"] == str(ALICE_USD)
        assert resp["balance"] == str(ALICE_START)
        assert resp["ccy"] == "USD"
        assert resp["status"] == "ACTIVE"

    def test_unknown_account_is_an_error_not_a_zero_balance(self, client: LedgerClient) -> None:
        # Act
        resp = client.get_account(424_242)

        # Assert —— inventing an empty account would be far worse than saying no.
        assert resp["type"] == "error"
        assert resp["code"] == "ACCOUNT_NOT_FOUND"


class TestTransfer:
    def test_moves_money_and_reports_both_new_balances(self, client: LedgerClient) -> None:
        # Arrange
        amount = 2_500  # $25.00

        # Act
        resp = client.transfer("t1", ALICE_USD, BOB_USD, amount, req_id="9")

        # Assert
        assert resp["type"] == "transfer_ok", resp
        assert resp["id"] == "9"
        assert resp["from_balance"] == str(ALICE_START - amount)
        assert resp["to_balance"] == str(BOB_START + amount)
        assert int(resp["tx_id"]) > 0

    def test_the_reported_balance_matches_a_later_lookup(self, client: LedgerClient) -> None:
        # Arrange
        transferred = client.transfer("t2", ALICE_USD, BOB_USD, 1_000)
        assert transferred["type"] == "transfer_ok"

        # Act —— ask again, through a separate request.
        looked_up = client.get_account(ALICE_USD)

        # Assert —— the transfer response must not be able to disagree with the ledger.
        assert looked_up["balance"] == transferred["from_balance"]

    def test_money_is_conserved_across_the_pair(self, client: LedgerClient) -> None:
        # Arrange
        before = int(client.get_account(ALICE_USD)["balance"]) + int(
            client.get_account(BOB_USD)["balance"]
        )

        # Act
        client.transfer("t3", ALICE_USD, BOB_USD, 4_321)

        # Assert
        after = int(client.get_account(ALICE_USD)["balance"]) + int(
            client.get_account(BOB_USD)["balance"]
        )
        assert after == before, "a transfer must not change the total, only where it sits"


class TestRefusals:
    """Every rejection must leave the ledger exactly as it was."""

    @pytest.mark.parametrize(
        ("case", "kwargs", "expected_code"),
        [
            ("overdraft", {"src": BOB_USD, "dst": ALICE_USD, "amount": 999_999}, "INSUFFICIENT_FUNDS"),
            ("self transfer", {"src": ALICE_USD, "dst": ALICE_USD, "amount": 100}, "SELF_TRANSFER"),
            ("zero amount", {"src": ALICE_USD, "dst": BOB_USD, "amount": 0}, "INVALID_AMOUNT"),
            ("negative amount", {"src": ALICE_USD, "dst": BOB_USD, "amount": -100}, "INVALID_AMOUNT"),
            ("unknown account", {"src": ALICE_USD, "dst": 999_999, "amount": 100}, "ACCOUNT_NOT_FOUND"),
            (
                "cross currency",
                {"src": ALICE_USD, "dst": ALICE_JPY, "amount": 100},
                "CURRENCY_MISMATCH",
            ),
        ],
    )
    def test_invalid_transfers_are_refused_without_side_effects(
        self,
        client: LedgerClient,
        case: str,
        kwargs: dict[str, int],
        expected_code: str,
    ) -> None:
        # Arrange
        before = int(client.get_account(ALICE_USD)["balance"])

        # Act
        resp = client.transfer(f"bad-{case}", ccy="USD", **kwargs)

        # Assert
        assert resp["type"] == "error", f"{case} should have been refused, got {resp}"
        assert resp["code"] == expected_code
        assert int(client.get_account(ALICE_USD)["balance"]) == before, (
            f"{case} was refused but still moved money"
        )

    def test_unknown_currency_is_refused(self, client: LedgerClient) -> None:
        # Act
        resp = client.transfer("bad-ccy", ALICE_USD, BOB_USD, 100, ccy="XYZ")

        # Assert
        assert resp["type"] == "error"
        assert resp["code"] == "UNKNOWN_CURRENCY"

    def test_missing_field_is_named_as_such(self, client: LedgerClient) -> None:
        # Act —— no "amount".
        resp = client.request(
            id="1",
            type="transfer",
            idem_key="k",
            **{"from": "1001"},
            to="2002",
            ccy="USD",
        )

        # Assert
        assert resp["type"] == "error"
        assert resp["code"] == "MISSING_FIELD"

    def test_unknown_message_type_is_refused(self, client: LedgerClient) -> None:
        # Act
        resp = client.request(id="1", type="withdraw")

        # Assert
        assert resp["type"] == "error"
        assert resp["code"] == "UNKNOWN_MESSAGE_TYPE"


class TestFailureModes:
    """Two kinds of protocol failure, two different consequences."""

    def test_garbage_gets_an_error_but_keeps_the_connection(self, client: LedgerClient) -> None:
        # Act —— a decode error: the line boundary is known, the contents are not.
        client.send_line("this is not json at all")
        error = json.loads(client.recv_line())

        # Assert —— an error, then business as usual on the same socket.
        assert error["type"] == "error"
        # Note the numeric id: the envelope's "id" is a u32 carried as a string,
        # so a non-numeric one is itself a protocol error.
        assert client.ping(req_id="99")["type"] == "pong", (
            "a decode error must not close the connection"
        )

    def test_unsupported_version_is_rejected(self, client: LedgerClient) -> None:
        # Act
        client.send_line(json.dumps({"v": 2, "id": "1", "type": "ping"}))
        resp = json.loads(client.recv_line())

        # Assert
        assert resp["type"] == "error"
        assert resp["code"] == "UNSUPPORTED_VERSION"

    def test_an_endless_line_is_capped_and_closes_the_connection(
        self, engine: dict[str, int]
    ) -> None:
        # Arrange —— NDJSON has no length prefix, so a client that never sends a
        # newline could otherwise grow the server's buffer without limit.
        with LedgerClient(engine["json"], timeout=5.0) as client:
            # Act —— well past kMaxFrameSize (64 KB), no newline anywhere.
            try:
                client._sock.sendall(b"x" * (128 * 1024))
            except OSError:
                pass  # the server may have closed on us mid-write; that is the point

            # Assert —— a framing error is fatal: reply, then FIN.
            try:
                resp = json.loads(client.recv_line())
                assert resp["code"] == "FRAME_TOO_LARGE"
            except (ProtocolError, ValueError):
                pass  # closed without a readable reply is also acceptable
            assert client.at_eof(), "a framing error must close the connection"
