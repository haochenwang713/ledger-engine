# Transactional Ledger & Settlement Engine

A high-concurrency, ACID-compliant double-entry ledger written in C++20. Handles
multi-currency account transfers over a hand-rolled epoll event loop, prevents
double-spending under concurrent load, and guarantees that every transfer is
recorded as a balanced pair of immutable journal entries.

> **Status:** Stage 3 of 9 — the in-memory ledger core is complete and verified
> clean under ThreadSanitizer. See [Roadmap](#roadmap) for what is next.

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

Requirements grow with the stages, so you do not need everything on day one:

| What you want to run | What you need |
|---|---|
| Ledger core and its tests (stages 1–3) | A C++20 compiler and CMake 3.22+. Any platform — macOS included |
| Schema checks (stage 2) | Also a PostgreSQL 16, native or containerized |
| The TCP server (stages 4+) | Linux, because `epoll` has no macOS equivalent |

`epoll` is Linux-only, but that constraint belongs to the networking layer
alone. The ledger core — accounts, ordered lock acquisition, `shared_mutex` —
is plain standard C++20 and builds and runs natively on macOS, sanitizers
included. Only the socket layer needs a container.

```bash
make doctor      # reports what is installed and what each stage needs
```

## Quick start

```bash
git clone https://github.com/<your-username>/ledger-engine.git
cd ledger-engine

make test        # configure, build, run the test suite — no Docker needed
```

Expected output:

```
100% tests passed, 0 tests failed out of 37
```

For the schema checks, pick whichever PostgreSQL is easier for you. The
Makefile detects Docker and falls back to a native install, so the commands are
the same either way.

```bash
# Option A — Docker (also gives you the Linux container for stages 4+)
make up

# Option B — native PostgreSQL, no Docker
brew install postgresql@16 && brew services start postgresql@16
make db-native-setup

# Either way:
make db-all
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

## How the locking works

A transfer touches two accounts, so it takes two locks. Which order it takes
them in is the whole design.

```cpp
if (req.from == req.to) return ErrorCode::SelfTransfer;  // before any lock

Account* lo = from;
Account* hi = to;
if (lo->id() > hi->id()) std::swap(lo, hi);   // always ascending account id

std::unique_lock loGuard(lo->mutex);
std::unique_lock hiGuard(hi->mutex);
// check balance and apply it, both inside this one critical section
```

Locking in the request's own direction deadlocks: a thread moving 1001 → 2002
holds 1001 and waits for 2002 while a thread moving 2002 → 1001 holds 2002 and
waits for 1001. Sorting by account id means both threads take 1001 first, so
the second one blocks while **holding nothing** — it cannot become anyone's
blocker, and the wait-for graph stays acyclic. That is a proof, not a heuristic:
along any cycle the ids would have to increase strictly and still return to the
start.

The self-transfer guard has to come first. Without it `lo` and `hi` are the same
account, the same non-recursive mutex gets locked twice, and the thread wedges
itself — undefined behavior that presents as a random hang.

One inversion is deliberate. `auditMutex_` is taken **shared by transfers**
(which write) and **exclusive by audits** (which read). It does not protect a
variable; it protects the property "no transfer is in flight". Transfers already
exclude each other through the account locks, so they pay nothing, while an
audit gets a genuinely consistent cross-account snapshot instead of account A's
new balance beside account B's old one.

## What the concurrency tests actually prove

Concurrency bugs are the ones that usually don't happen. So the tests generate
real contention and then check properties that hold no matter how the threads
interleave.

| Test | What it would catch |
|---|---|
| `TotalMoneyIsConserved` | 32 threads, 96k transfers over 20 accounts. Total per currency must be **exactly** unchanged. A lost update shows up as money appearing or vanishing |
| `ConcurrentWithdrawalsCannotOverdraw` | One account holds 1000, everyone withdraws 100. Exactly 10 succeed — 11 would mean money created from nothing |
| `OppositeDirectionTransfersDoNotDeadlock` | 32 threads, two accounts, 640k opposing transfers. Deadlock presents as a hang, caught by the ctest timeout |
| `AuditSeesConsistentSnapshotDuringTraffic` | Every audit taken mid-traffic must see the correct total |
| `RegistryGrowthDoesNotInvalidatePointers` | 20k accounts created while transfers run. Rehashing must not invalidate a live `Account*` |

These were checked against deliberately broken builds, because a test that
cannot fail proves nothing:

- Removing the `std::swap` deadlocks within 30 seconds.
- Downgrading the account locks to `shared_lock` loses 8081 units out of
  20,000,000 and trips both the conservation check and the I2 recompute.

Both sanitizer builds are clean: no data races under ThreadSanitizer, no
use-after-free or undefined behavior under AddressSanitizer/UBSan.

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
| 3 | Ledger core — in-memory, `shared_mutex`, no DB yet | ✅ Done |
| 4 | epoll TCP server (echo first) | ⬜ Next |
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
