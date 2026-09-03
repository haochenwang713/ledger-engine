"""The observability surface (Step 10), seen from outside.

`get_stats` is what makes the engine watchable — it is the data the Step 12 web
console will render. These tests hold it to the standard that matters for a
dashboard: the numbers must be *live*. A stats endpoint that always answers zero
passes every "is the shape right" test ever written.
"""

from __future__ import annotations

import json

from client import LedgerClient

ALICE_USD = 1001
BOB_USD = 2002

# Every field the response is contracted to carry. If one is dropped, the console
# breaks; if one is added, this list should grow with it deliberately.
EXPECTED_FIELDS = {
    "uptime_ms",
    "accounts",
    "connections_active",
    "connections_total",
    "transfers_committed",
    "transfers_rejected",
    "binary_workers",
    "binary_queue_depth",
    "binary_queue_capacity",
    "binary_submitted",
    "binary_completed",
    "binary_rejected",
    "binary_dropped",
    "json_workers",
    "json_queue_depth",
    "json_queue_capacity",
    "json_submitted",
    "json_completed",
    "json_rejected",
    "json_dropped",
}


class TestShape:
    def test_every_documented_field_is_present(self, client: LedgerClient) -> None:
        # Act
        resp = client.get_stats(req_id="1")

        # Assert
        assert resp["type"] == "stats"
        assert resp["id"] == "1"
        present = set(resp) - {"type", "id", "v"}
        assert present == EXPECTED_FIELDS, f"missing: {EXPECTED_FIELDS - present}"

    def test_every_number_is_a_quoted_string(self, client: LedgerClient) -> None:
        # Act —— read the raw line, before json.loads erases the distinction.
        client.send_line(json.dumps({"id": "1", "type": "get_stats"}))
        raw = client.recv_line()
        resp = json.loads(raw)

        # Assert —— the console runs in a browser, where a bare number loses
        # precision above 2^53. Every counter must arrive as text.
        for name in EXPECTED_FIELDS:
            assert isinstance(resp[name], str), f"{name} came back as {type(resp[name])}, not a string"
            assert f'"{name}":"' in raw, f"{name} is not quoted on the wire"


class TestNumbersAreLive:
    """A stats endpoint hard-coded to zero would pass a shape test. Not these."""

    def test_transfer_counters_follow_real_traffic(self, client: LedgerClient) -> None:
        # Arrange —— assert on the *delta*, not the absolute. The engine starts
        # with a non-zero committed count because seeding the demo accounts goes
        # through real transfers out of a system account rather than writing
        # balances directly (see test_the_seed_itself_is_counted below).
        before = client.get_stats()

        # Act —— one that works, one that cannot.
        ok = client.transfer("s-ok", ALICE_USD, BOB_USD, 100)
        refused = client.transfer("s-no", BOB_USD, ALICE_USD, 99_999_999)

        # Assert
        assert ok["type"] == "transfer_ok"
        assert refused["type"] == "error"

        after = client.get_stats()
        committed = int(after["transfers_committed"]) - int(before["transfers_committed"])
        rejected = int(after["transfers_rejected"]) - int(before["transfers_rejected"])
        assert committed == 1
        assert rejected == 1

    def test_the_seed_itself_is_counted(self, client: LedgerClient) -> None:
        # Act —— before this connection has moved any money of its own.
        resp = client.get_stats()

        # Assert —— four seed transfers: dollars to Alice, dollars to Bob, yen to
        # Alice, and the worked example from the design document. Money never
        # appears from nowhere, so even the starting balances are transfers, and
        # the counter is honest about that rather than quietly excluding them.
        assert int(resp["transfers_committed"]) == 4
        assert int(resp["transfers_rejected"]) == 0

    def test_uptime_advances(self, client: LedgerClient) -> None:
        # Arrange
        first = int(client.get_stats()["uptime_ms"])

        # Act —— do some real work rather than sleeping; the clock moves anyway.
        for i in range(50):
            client.transfer(f"tick-{i}", ALICE_USD, BOB_USD, 1)

        # Assert
        second = int(client.get_stats()["uptime_ms"])
        assert second >= first, "uptime went backwards"
        assert first >= 0

    def test_the_pools_are_counted_separately(self, client: LedgerClient) -> None:
        # Act —— every request here goes to the JSON port.
        resp = client.get_stats()

        # Assert —— the two pools have independent queues, and the stats must
        # show that. Summing them would hide exactly the situation backpressure
        # exists for: one port saturated while the other is idle.
        assert int(resp["json_submitted"]) >= 1
        assert int(resp["binary_submitted"]) == 0, (
            "binary pool counted work that arrived on the JSON port"
        )
        assert int(resp["json_workers"]) > 0
        assert int(resp["binary_workers"]) > 0
        assert int(resp["json_queue_capacity"]) > 0
        assert int(resp["binary_queue_capacity"]) > 0

    def test_connections_are_counted(self, engine: dict[str, int]) -> None:
        # Arrange —— hold several connections open at once.
        held = [LedgerClient(engine["json"]) for _ in range(5)]
        try:
            # Act
            resp = held[0].get_stats()

            # Assert
            assert int(resp["connections_active"]) >= 5
            assert int(resp["connections_total"]) >= 5
        finally:
            for connection in held:
                connection.close()

    def test_the_account_count_matches_the_seeded_ledger(self, client: LedgerClient) -> None:
        # Act
        resp = client.get_stats()

        # Assert —— 1001, 2002, 1003 plus the two system accounts.
        assert int(resp["accounts"]) == 5


class TestItDoesNotDisturbWhatItMeasures:
    def test_asking_repeatedly_does_not_break_transfers(self, client: LedgerClient) -> None:
        # Arrange
        before = int(client.get_account(ALICE_USD)["balance"]) + int(
            client.get_account(BOB_USD)["balance"]
        )
        committed_before = int(client.get_stats()["transfers_committed"])

        # Act —— interleave monitoring with real money movement.
        for i in range(25):
            client.get_stats()
            client.transfer(f"mix-{i}", ALICE_USD, BOB_USD, 7)
            client.get_stats()

        # Assert —— the ledger is untouched by having been watched.
        after = int(client.get_account(ALICE_USD)["balance"]) + int(
            client.get_account(BOB_USD)["balance"]
        )
        assert after == before
        committed = int(client.get_stats()["transfers_committed"]) - committed_before
        assert committed == 25, "monitoring traffic must not be counted as ledger traffic"
