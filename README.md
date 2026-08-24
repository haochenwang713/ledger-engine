# Transactional Ledger & Settlement Engine

A high-concurrency, ACID-compliant double-entry ledger written in C++20. Handles
multi-currency account transfers over a hand-rolled epoll event loop, prevents
double-spending under concurrent load, and guarantees that every transfer is
recorded as a balanced pair of immutable journal entries.

> **Status:** Stage 2 of 9 — schema is live and enforces double-entry at the
> database level. See [Roadmap](#roadmap) for what is done and what is next.

---

## Why this project

Money systems fail in ways ordinary CRUD apps do not. A lost update is not a
stale cache — it is money that no longer exists. This engine is built around
that constraint:

- **Double-entry is enforced, not documented.** Every transfer produces exactly
  two journal entries whose amounts sum to zero. Account balances are a
  *derived snapshot* that can always be recomputed from the immutable entry log,
  which makes lost updates detectable rather than silent.
- **Deadlock is prevented by construction.** Concurrent `A → B` and `B → A`
  transfers acquire per-account locks in a strict global order, so the wait-for
  graph is provably acyclic. Not "we tested it and it seemed fine."
- **Durability is the acknowledgment boundary.** A client is told a transfer
  succeeded only after PostgreSQL has committed it. In-memory state never runs
  ahead of the database.

## Architecture at a glance

```
Client ──TCP──▶ IO Thread ──▶ BlockingQueue ──▶ Worker Pool ──▶ PostgreSQL 16
                (epoll ET)      (bounded)         (20 threads)     (WAL/fsync)
                    ▲                                   │
                    └────── eventfd wakeup ─────────────┘
                       (only the IO thread writes to sockets)
```

| Concern | Approach |
|---|---|
| Networking | POSIX sockets + `epoll` (edge-triggered, non-blocking), hand-written event loop |
| Concurrency | One IO thread, 20+ worker threads, bounded MPMC task queue with backpressure |
| Locking | Per-account `std::shared_mutex`, ordered acquisition by account id |
| Atomics | `std::atomic` for counters, ID generation, and shutdown flags only — never balances |
| Persistence | PostgreSQL 16 + libpqxx, `SELECT … FOR UPDATE` in matching lock order |
| Money | `int64_t` minor units with a per-currency exponent. No floating point, anywhere |
| Idempotency | Client-supplied key, guarded by an in-memory shard cache and a DB unique index |
| Testing | GoogleTest units, ThreadSanitizer / AddressSanitizer, Locust load tests |

The full design — module boundaries, thread model, the deadlock proof, the
schema, and the three persistence strategies that were evaluated — is in
**[`docs/stage-0-architecture.md`](docs/stage-0-architecture.md)** (written in
Traditional Chinese).

## Supported currencies

`USD` · `EUR` · `JPY` · `GBP` · `CNY` · `TWD`

Amounts are stored as `int64_t` in each currency's minor unit. `JPY` has an
exponent of **0** while the rest have **2**, so the same stored value of `5000`
means `$50.00` in USD but `¥5,000` in JPY. Every format and parse path consults
the currency table; nothing divides by 100.

Both legs of a transfer must share a currency. FX is out of scope for v1 and
would be modeled as two same-currency transactions through an FX position
account rather than a single mixed-currency entry pair.

## Requirements

**`epoll` is Linux-only.** There is no `<sys/epoll.h>` on macOS or Windows, and
the build fails at compile time on those platforms by design. Use the provided
Docker environment.

- Docker + Docker Compose (the only hard requirement)
- Or, natively on Linux: GCC 11+ / Clang 14+, CMake 3.22+, libpqxx, GoogleTest

## Quick start

```bash
git clone https://github.com/<your-username>/ledger-engine.git
cd ledger-engine

# Bring up PostgreSQL 16 and the Ubuntu 22.04 build container
make up

# Drop into the build container
make shell

# Inside the container:
make test    # configure, build, and run the test suite
make run     # run the engine binary
```

Expected output from `make test`:

```
100% tests passed, 0 tests failed out of 3
```

`make help` lists every available target.

## The schema enforces the ledger, not just stores it

Most of the double-entry rules live in the database rather than in application
code, because application code can be bypassed — by a stray `psql` session, a
data-fixing script, or a new code path that forgot to check.

| Rule | How it is enforced |
|---|---|
| A transaction has exactly two entries summing to zero | `DEFERRABLE INITIALLY DEFERRED` constraint trigger, checked at `COMMIT` |
| Both legs share a currency | Composite FK to `accounts (id, currency)` — a cross-currency row has no parent to reference |
| An entry's currency matches its account | Same composite FK, from `entries` |
| The audit trail is immutable | `BEFORE UPDATE`/`BEFORE DELETE` triggers reject every mutation on `entries` |
| A resent request cannot double-charge | `UNIQUE (idempotency_key)` on `transactions` |
| A user balance cannot go negative | `CHECK (allow_negative OR balance >= 0)` |
| An account cannot transfer to itself | `CHECK (debit_account_id <> credit_account_id)` |

The deferred trigger is the interesting one. `CHECK` constraints see a single
row, but "these two rows sum to zero" spans rows — and the two entries arrive in
separate `INSERT`s, so an immediate check would fire while the transaction is
legitimately half-written. Deferring it to commit time is what makes the rule
expressible at all.

```bash
make db-all      # reset schema, load seed, run 22 constraint tests, check invariants
make db-check    # invariant health check on whatever data is currently there
make psql        # interactive session
```

`make db-test` writes deliberately invalid data 22 different ways and asserts
the database rejects every one. `make db-check` asks the opposite question — is
the data that is already there still consistent? Constraints only catch the
error shapes they know about; a lost update writes rows that are individually
valid and only wrong in relation to each other, which is why **I2** compares
each balance snapshot against the sum of its entries.

## Development

```bash
make build        # configure + compile
make test         # compile + ctest
make tsan         # ThreadSanitizer build + tests
make asan         # AddressSanitizer + UBSan build + tests
make fmt          # clang-format in place
make fmt-check    # verify formatting without modifying (used by CI)
make clean        # remove build directories
```

PostgreSQL is exposed on host port **5433** to avoid colliding with a local
install on 5432:

```bash
psql postgresql://ledger:ledger_dev_password@localhost:5433/ledger
```

Credentials in `docker-compose.yml` are development-only and are not used
anywhere else.

## Roadmap

| Stage | Deliverable | Status |
|---|---|---|
| 0 | Architecture design, module split, thread model | ✅ Done |
| 1 | Project skeleton, CMake, Docker Compose | ✅ Done |
| 2 | DB schema: `currencies` / `accounts` / `transactions` / `entries` | ✅ Done |
| 3 | Ledger core — in-memory, `shared_mutex`, no DB yet | ⬜ Next |
| 4 | epoll TCP server (echo first) | ⬜ |
| 5 | Wire protocol framing + thread pool | ⬜ |
| 6 | PostgreSQL integration with `SELECT … FOR UPDATE` | ⬜ |
| 7 | ThreadSanitizer / AddressSanitizer verification | ⬜ |
| 8 | Locust load tests, tuning, TPS and p95 numbers | ⬜ |
| 9 | Architecture diagrams, final documentation | ⬜ |

## Invariants under test

These are asserted by the test suite, not just described here:

| | Invariant |
|---|---|
| **I1** | Every transaction's entries sum to zero |
| **I2** | Every account balance equals the sum of its entries — catches lost updates |
| **I3** | Total money per currency is conserved across 32 threads and 100k concurrent transfers |
| **I4** | No user account balance goes negative under concurrent load — no double-spending |
| **I5** | The same idempotency key always yields the same transaction, applied once |

I3 is the load-bearing one: it is the assertion that a race condition cannot
hide behind.

## License

MIT — see [LICENSE](LICENSE).
