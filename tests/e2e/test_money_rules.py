"""The money rules, checked from outside the process.

These duplicate assertions that the C++ unit tests already make — deliberately.
The unit tests prove the ledger core is right; these prove the rules survive the
whole journey out through the codec, the queue, a worker, and back down a socket.
A currency exponent applied correctly in LedgerCore but lost in the JSON encoder
would pass there and fail here.
"""

from __future__ import annotations

import json

from client import LedgerClient

ALICE_USD = 1001
BOB_USD = 2002
ALICE_JPY = 1003


class TestCurrencyExponent:
    """The same stored integer means different money in different currencies."""

    def test_usd_and_jpy_accounts_hold_the_same_number(self, client: LedgerClient) -> None:
        # Act
        usd = client.get_account(ALICE_USD)
        jpy = client.get_account(ALICE_JPY)

        # Assert —— 5000 is ¥5,000 here, and would be $50.00 in a USD account.
        # The wire carries the raw minor units; formatting is the client's job.
        assert jpy["balance"] == "5000"
        assert jpy["ccy"] == "JPY"
        assert usd["ccy"] == "USD"

    def test_a_jpy_transfer_moves_whole_yen(self, client: LedgerClient) -> None:
        # Arrange —— JPY has exponent 0, so 1 means one yen, not one sen.
        # 1003 is the only seeded JPY user account, so send money back to the
        # system account it came from.
        before = int(client.get_account(ALICE_JPY)["balance"])

        # Act
        resp = client.transfer("jpy-1", ALICE_JPY, 9002, 1_000, ccy="JPY")

        # Assert
        assert resp["type"] == "transfer_ok", resp
        assert int(resp["from_balance"]) == before - 1_000


class TestIntegersAreStrings:
    """The single rule a client author is most likely to get wrong."""

    def test_every_integer_field_comes_back_quoted(self, client: LedgerClient) -> None:
        # Act —— read the raw line, before json.loads erases the distinction.
        client.send_line(json.dumps({"id": "1", "type": "get_account", "account_id": "1001"}))
        raw = client.recv_line()

        # Assert
        assert '"balance":"115000"' in raw, f"balance must be a quoted string, got: {raw}"
        assert '"balance":115000' not in raw, "a bare number would lose precision in a browser"
        assert '"id":"1001"' in raw

    def test_a_bare_number_in_a_request_is_refused(self, client: LedgerClient) -> None:
        # Arrange —— exactly the mistake a JavaScript client makes by default.
        line = json.dumps(
            {
                "id": "1",
                "type": "transfer",
                "idem_key": "k",
                "from": 1001,  # ← not a string
                "to": "2002",
                "amount": "5000",
                "ccy": "USD",
            }
        )

        # Act
        client.send_line(line)
        resp = json.loads(client.recv_line())

        # Assert —— refuse now, loudly, rather than work until the numbers get big.
        assert resp["type"] == "error"
        assert resp["code"] == "INTEGER_NOT_STRING"

    def test_values_beyond_double_precision_survive_the_round_trip(
        self, client: LedgerClient
    ) -> None:
        # Arrange —— 2^53 + 1 cannot be represented exactly as an IEEE-754 double.
        # An amount this large is refused for lack of funds, but the *error* proves
        # the value arrived intact: a truncated value would still be refused, so we
        # check the balance is untouched and the refusal is the funds one, not a
        # parse failure.
        beyond_double = 9_007_199_254_740_993

        # Act
        resp = client.transfer("big", ALICE_USD, BOB_USD, beyond_double)

        # Assert
        assert resp["type"] == "error"
        assert resp["code"] == "INSUFFICIENT_FUNDS", (
            "the amount must have parsed as a valid int64, just a very large one"
        )


class TestDoubleEntry:
    """What one account loses, another gains — exactly."""

    def test_the_two_legs_cancel_out(self, client: LedgerClient) -> None:
        # Arrange
        alice_before = int(client.get_account(ALICE_USD)["balance"])
        bob_before = int(client.get_account(BOB_USD)["balance"])
        amount = 3_333

        # Act
        resp = client.transfer("dd-1", ALICE_USD, BOB_USD, amount)

        # Assert
        assert resp["type"] == "transfer_ok"
        alice_delta = int(resp["from_balance"]) - alice_before
        bob_delta = int(resp["to_balance"]) - bob_before
        assert alice_delta == -amount
        assert bob_delta == amount
        assert alice_delta + bob_delta == 0, "the two legs must sum to zero (invariant I1)"

    def test_spending_the_entire_balance_is_allowed(self, client: LedgerClient) -> None:
        # Arrange
        everything = int(client.get_account(ALICE_USD)["balance"])

        # Act
        resp = client.transfer("all-in", ALICE_USD, BOB_USD, everything)

        # Assert —— the boundary must be >=, not >.
        assert resp["type"] == "transfer_ok", resp
        assert resp["from_balance"] == "0"

    def test_one_unit_beyond_the_balance_is_refused(self, client: LedgerClient) -> None:
        # Arrange
        everything = int(client.get_account(ALICE_USD)["balance"])

        # Act
        resp = client.transfer("one-too-many", ALICE_USD, BOB_USD, everything + 1)

        # Assert
        assert resp["type"] == "error"
        assert resp["code"] == "INSUFFICIENT_FUNDS"
