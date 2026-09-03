"""Concurrency, seen from the outside.

The C++ suite already proves the ledger core cannot lose money under 32 threads.
What it cannot prove is that the *server* — event loop, codecs, bounded queue,
worker pool, response routing — preserves that guarantee once real sockets are in
the way. Responses could be delivered to the wrong connection, or dropped, and
the core would still be perfectly correct.

So these tests use many concurrent connections and then assert properties that
hold no matter how the threads interleave.
"""

from __future__ import annotations

import concurrent.futures
import threading

import pytest

from client import LedgerClient, balance_of

ALICE_USD = 1001
BOB_USD = 2002


@pytest.mark.slow
def test_opposing_transfers_preserve_total_money(engine: dict[str, int]) -> None:
    """The pattern that deadlocks a naive lock-the-source-first implementation.

    Half the clients send 1001 -> 2002, half send 2002 -> 1001, all at once. A
    deadlock shows up as this test hanging until pytest's own patience runs out;
    a lost update shows up as the total changing.
    """
    # Arrange
    clients_count = 8
    per_client = 25
    amount = 100

    with LedgerClient(engine["json"]) as reader:
        before = balance_of(reader, ALICE_USD) + balance_of(reader, BOB_USD)

    answered = threading.Semaphore(0)

    def run_one(index: int) -> int:
        forward = index % 2 == 0
        src, dst = (ALICE_USD, BOB_USD) if forward else (BOB_USD, ALICE_USD)
        succeeded = 0
        with LedgerClient(engine["json"]) as client:
            for i in range(per_client):
                resp = client.transfer(f"c{index}-{i}", src, dst, amount, req_id=str(i))
                answered.release()
                # Every request must come back on its own connection, with its id.
                assert resp["id"] == str(i), "a response was routed to the wrong connection"
                if resp["type"] == "transfer_ok":
                    succeeded += 1
        return succeeded

    # Act
    with concurrent.futures.ThreadPoolExecutor(max_workers=clients_count) as pool:
        results = list(pool.map(run_one, range(clients_count)))

    # Assert
    assert sum(results) > 0, "no transfer succeeded — this run proved nothing"

    with LedgerClient(engine["json"]) as reader:
        after = balance_of(reader, ALICE_USD) + balance_of(reader, BOB_USD)
    assert after == before, (
        "the two balances no longer sum to what they did — money was lost or created"
    )


@pytest.mark.slow
def test_concurrent_withdrawals_cannot_overdraw(engine: dict[str, int]) -> None:
    """Everyone races for the same limited balance; the total taken cannot exceed it."""
    # Arrange —— drain Alice down to a known small amount first.
    slice_amount = 10_000
    with LedgerClient(engine["json"]) as setup:
        start = balance_of(setup, ALICE_USD)
        # Leave exactly 3 slices behind.
        leftover = slice_amount * 3
        setup.transfer("drain", ALICE_USD, BOB_USD, start - leftover)
        assert balance_of(setup, ALICE_USD) == leftover

    def withdraw(index: int) -> bool:
        with LedgerClient(engine["json"]) as client:
            resp = client.transfer(f"w{index}", ALICE_USD, BOB_USD, slice_amount)
            return resp["type"] == "transfer_ok"

    # Act —— twelve racers, three slices available.
    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
        outcomes = list(pool.map(withdraw, range(12)))

    # Assert —— exactly three may win. Four would mean money created from nothing.
    assert sum(outcomes) == 3, f"{sum(outcomes)} withdrawals succeeded, expected exactly 3"
    with LedgerClient(engine["json"]) as reader:
        assert balance_of(reader, ALICE_USD) == 0
        assert balance_of(reader, ALICE_USD) >= 0, "the account went negative — overdraft"


def test_many_connections_each_get_their_own_answers(engine: dict[str, int]) -> None:
    """Thirty open connections at once; no response may land on the wrong socket."""
    # Arrange
    count = 30
    clients = [LedgerClient(engine["json"]) for _ in range(count)]

    try:
        # Act —— every client sends a ping carrying its own id, then all read back.
        for index, client in enumerate(clients):
            client.send_line(f'{{"id":"{index}","type":"ping"}}')

        # Assert
        for index, client in enumerate(clients):
            import json

            resp = json.loads(client.recv_line())
            assert resp["type"] == "pong"
            assert resp["id"] == str(index), "a response was delivered to the wrong connection"
    finally:
        for client in clients:
            client.close()


def test_a_client_disconnecting_mid_flight_does_not_kill_the_server(
    engine: dict[str, int]
) -> None:
    """Fire and hang up. The worker's result is discarded, not written to a dead socket."""
    # Act
    for _ in range(50):
        client = LedgerClient(engine["json"])
        client.send_line('{"id":"1","type":"ping"}')
        client.close()  # never read the reply

    # Assert —— the server is still there and still correct.
    with LedgerClient(engine["json"]) as survivor:
        resp = survivor.ping(req_id="99")
        assert resp["type"] == "pong"
        assert resp["id"] == "99"
