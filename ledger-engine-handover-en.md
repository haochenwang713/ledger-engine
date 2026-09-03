# Ledger Engine — Project Handover

> **Purpose of this document**: someone who has never touched this project should be able to read this one file and understand what the system does, how far it has been built, the reasoning behind every significant decision, and how to build and verify it — and then pick up Stage 6 directly.
>
> **Last updated**: 2026-09-02
> **Repository**: `github.com/haochenwang713/ledger-engine`
> **Local path**: `/Users/haochensmacbookpro/Downloads/ledger-engine`
> **Status**: Stages 0–5 complete, CI green. **Next up = Stage 6 (PostgreSQL integration)**

---

## Table of contents

1. [What this project is](#1-what-this-project-is)
2. [Technical constraints (not negotiable)](#2-technical-constraints-not-negotiable)
3. [Progress overview](#3-progress-overview)
4. [System architecture](#4-system-architecture)
5. [Stage-by-stage record](#5-stage-by-stage-record)
6. [Current directory layout](#6-current-directory-layout)
7. [Invariants — the referee for correctness](#7-invariants--the-referee-for-correctness)
8. [Lock hierarchy and the deadlock argument](#8-lock-hierarchy-and-the-deadlock-argument)
9. [Build and verification: full procedure](#9-build-and-verification-full-procedure)
10. [Environment pitfalls](#10-environment-pitfalls)
11. [What is not done yet](#11-what-is-not-done-yet)
12. [Stage 6 handover notes](#12-stage-6-handover-notes)
13. [Open questions](#13-open-questions)
14. [Self-check questions for whoever takes over](#14-self-check-questions-for-whoever-takes-over)
15. [Provenance and confidence](#15-provenance-and-confidence)

---

## 1. What this project is

**Transactional Ledger & Settlement Engine** — a high-concurrency double-entry bookkeeping engine written in C++20.

Three things it has to get right:

| Goal | What it concretely means |
|---|---|
| High-frequency transfers | Many threads process transfer requests simultaneously, not one at a time in a queue |
| No double-spending | An account holding 1000 with ten threads each withdrawing 100 must yield **exactly** ten successes; the eleventh must fail |
| Atomic double-entry | Every transaction produces two entries summing to zero, and "two entries + two balance updates" either all happen or none do |

It is also a **portfolio project**, and that shapes some trade-offs. In several places the design deliberately favours *explainability* over *convenience*, because it has to be defensible in three sentences during an interview (ordered locking vs. `std::scoped_lock` is the clearest example — see §8).

### The core mental model

> **An entry is an immutable fact. A balance is a mutable snapshot.**

Facts are never modified (the `entries` table is append-only); the snapshot can be recomputed from the facts at any time. That relationship *is* the system's self-check: any time `balance != SUM(entries)`, a lost update has occurred. This is invariant I2, and it is the primary weapon this project uses to catch race conditions.

---

## 2. Technical constraints (not negotiable)

| Area | Choice |
|---|---|
| Language / build | C++20, CMake, Linux (Ubuntu 22.04) |
| Concurrency | `std::shared_mutex`, `std::atomic`, hand-written thread pool, 20+ workers |
| Networking | POSIX sockets + epoll (**edge-triggered, non-blocking**), hand-written event loop |
| Database | PostgreSQL 16 + libpqxx, row-level pessimistic locking (`SELECT ... FOR UPDATE`) |
| Testing | GoogleTest (unit / concurrency), Locust (load testing, custom TCP client) |
| Tooling | Docker Compose, clang-format, ThreadSanitizer / AddressSanitizer |

### Three rules that are never broken

1. Amounts are always `int64_t` in the **minor unit**. Never `double`.
2. Both legs of a transfer must share a currency. Mismatches return `CURRENCY_MISMATCH` (no FX in v1).
3. The `entries` table is append-only. Never UPDATE, never DELETE. Reversals are done with an opposing transaction.

### Currencies and the JPY trap

| ISO 4217 | exponent | What `int64 amount = 5000` means |
|---|---|---|
| USD / EUR / GBP / CNY / TWD | 2 | $50.00 |
| JPY | **0** | **¥5,000** |

The same integer differs by a factor of one hundred. **Every display and parse path must look up `currency.exponent`. Hard-coding `/100` is forbidden.** This trap was left in deliberately: it forces there to be exactly one formatting implementation (`Money::toString()`).

---

## 3. Progress overview

| Stage | Content | Status | Deliverable |
|---|---|---|---|
| 0 | Architecture, module boundaries, threading model | ✅ Done | `docs/stage-0-architecture.md` |
| 1 | Project skeleton, CMake, Docker Compose | ✅ Done | Buildable shell, 19 tracked files |
| 2 | DB schema: accounts / entries / transactions | ✅ Done | 5 migrations + 22 constraint tests |
| 3 | Ledger core (in-memory, `shared_mutex`) | ✅ Done | `stage-3.patch`, 37/37 tests |
| 4 | epoll TCP server (echo) | ✅ Done | `stage-4.patch`, 56/56 tests |
| 5a | Wire protocol + dual codecs (binary / JSON) | ✅ Done | See §5.6 |
| 5b | Thread pool (`jthread` + `stop_token`) | ✅ Done | See §5.6 |
| 5c | Full server wiring | ✅ Done | CI green |
| **6** | **PostgreSQL + `SELECT FOR UPDATE`** | 🔜 **Next** | Durable transfers |
| 7 | TSan / ASan validation + concurrency tests | ⬜ Not started | Clean race report |
| 8 | Locust load tests + tuning | ⬜ Not started | TPS / p95 numbers |
| 9 | README, architecture diagrams, résumé evidence | ⬜ Not started | Finished portfolio piece |

---

## 4. System architecture

### 4.1 The partitioning principle

> **The net layer does not know what a balance is. The core layer does not know what a socket is.**

That sentence is the test for every module boundary. It paid off in Stage 5: replacing the echo handler with full protocol handling plus a thread pool required **zero changes to the networking layer**.

### 4.2 Module responsibilities

| Module | Single responsibility | Landed in |
|---|---|---|
| `Currency` | ISO 4217 ↔ enum, exponent lookup, formatting. Stateless, lock-free | Stage 2/3 |
| `Money` | Value type `{int64 units, Currency ccy}`; checks overflow and currency on arithmetic. Immutable | Stage 3 |
| `Account` | `id/owner/currency/balance/version` **plus its own `shared_mutex`**. Address-stable | Stage 3 |
| `AccountRegistry` | Only `id → Account*` lookup and insertion. One map-level `shared_mutex` that **protects the container structure, not account contents** | Stage 3 |
| `LedgerCore` | **The only place that knows how a transfer works.** Ordered locking, validation, application, entry generation | Stage 3 (extended in 6) |
| `Journal` | In-memory append-only sequence of transactions and entries; lets tests recompute balances | Stage 3 |
| `Buffer` | Growable byte buffer with two cursors. Accumulates partial frames under ET. **No syscalls** | Stage 4 |
| `Socket` | RAII fd, non-blocking / NoDelay / ReuseAddr | Stage 4 |
| `Connection` | Complete state of one fd. Held by `shared_ptr` | Stage 4 |
| `Acceptor` | `accept4(NONBLOCK)` in a loop until `EAGAIN` | Stage 4 |
| `EventLoop` | `epoll_wait` main loop, `wakeupFd`, pending tasks. **The only place allowed to `write()` a socket** | Stage 4 |
| `Codec` | Length-prefix framing and serialisation; binary + JSON implementations | Stage 5a |
| `BlockingQueue<T>` | Bounded MPMC queue: `push` / `try_push` / `pop` / `close` | Stage 5b |
| `ThreadPool` | Starts N workers; each will later own one DB connection | Stage 5b |
| `RequestHandler` | Abstract interface held by `unique_ptr` | Stage 5c |
| `PgPool` | Owns N `pqxx::connection` objects, held long-term by workers | **Stage 6** |
| `LedgerRepository` | Translates a transfer into one Postgres transaction. **The only place that writes SQL** | **Stage 6** |

**Three boundaries that matter most:**
- `Money` / `Currency` can be unit-tested to 100% with no threads involved.
- `LedgerCore` is the only file containing lock logic — chasing a deadlock means reading one file.
- `LedgerRepository` is the only file containing SQL — changing the persistence strategy touches one layer.

### 4.3 Threading model: why the split is mandatory

Assume one transfer costs roughly **0.05 ms** of pure CPU and roughly **1.5 ms** waiting on Postgres (including the COMMIT fsync).

| Approach | Event loop occupancy per transfer | Theoretical TPS ceiling |
|---|---|---|
| Everything on the event loop | 1.55 ms | ≈ **645** |
| IO thread + 20 workers | 0.05 ms | ≈ **12,900** |

A 20× difference — and the first approach also lets one slow transaction stall every connection.

> ### 🔒 Hard rule
> **The event loop thread never does anything that can block.**

### 4.4 Worker count: why 20+

```
N_threads ≈ N_cores × (1 + wait/compute) = 4 × (1 + 1.5/0.05) = 124   ← theoretical
```

The theoretical number cannot be used directly:

1. Postgres defaults to `max_connections = 100`, and each connection is a separate process (5–10 MB).
2. 124 threads rotating on 4 cores burn the gain in context switches and cache pollution.

The practical landing zone is **20–32 workers, each holding one DB connection for its lifetime** (not borrow-and-return per request). Stage 8 will make the worker count a tunable parameter and plot TPS-vs-workers to find the knee.

### 4.5 Queue design

**Bounded, mutex + condition_variable, capacity 8192.**

- **Why bounded**: backpressure. An unbounded queue grows without limit when the DB slows down, and queueing delay explodes — the client has long since timed out while a worker is still processing the request. 8192 is roughly 0.6 s of backlog; beyond that the work is pointless.
- **Why not a lock-free MPMC queue**:
  1. Optimising the wrong thing — a push/pop is ~100 ns against ~1,500,000 ns of DB wait. Four orders of magnitude.
  2. A new source of bugs — ABA and memory reclamation (hazard pointers / epochs) become your problem, and TSan is unreliable on hand-rolled atomic algorithms.
  3. Contention is not the issue — the critical section pushes one `Task` and holds the lock for ~50 ns.
- **When to revisit**: in Stage 8, measure with `perf`. If futex waiting exceeds 5% of CPU, the correct next step is **one queue per worker with round-robin dispatch** (then work stealing), not a lock-free queue. The single shared queue's whole virtue is that it load-balances for free.

> ⚠️ **The IO thread must never block waiting on a full queue.** Use `try_push()` only; on failure, return `SERVER_BUSY` immediately. Blocking here stalls the entire event loop.

### 4.6 Two invariants on the return path

| Tag | Content | Why it is hard to catch |
|---|---|---|
| **W1** | **One fd has exactly one writer.** Only the IO thread may `write()` a socket | Two workers writing the same socket interleave bytes, the length prefix stops matching, and the protocol collapses. But this is **not a data race** (`write()` itself is thread-safe), so **TSan will never find it** |
| **W2** | **The connection may be gone by the time a worker finishes.** `Task` holds a `std::weak_ptr`; on completion it `lock()`s, and discards the result if that fails | Otherwise it is a use-after-free that only shows up when a client disconnects mid-flight |

**Return mechanism:**
```
worker → conn->queueWrite(resp)        (small mutex)
       → loop->addPendingWrite(conn)
       → eventfd_write(wakeupFd, 1)
       → IO thread wakes and performs all write() calls
```

### 4.7 Concurrency mechanism table

| Data | Mechanism | Rationale |
|---|---|---|
| `AccountRegistry` map | One map-level `shared_mutex` | Lookups vastly outnumber account creation. **Protects container structure only, not account contents** |
| `Account.balance/status` | One `shared_mutex` per account | Queries take `shared_lock`, transfers take `unique_lock`. Granularity = a single account |
| `IdempotencyCache` | 64 shards, one `shared_mutex` each | `hash(key)%64` spreads contention to 1/64 |
| Global audit | One global `shared_mutex` | **Inverted usage**: transfers take shared (so they never block each other), audit takes unique (to get a consistent snapshot) |
| Metrics counters | `atomic<uint64_t>`, relaxed | Only the final count matters; no ordering relationship with other data is needed |
| Shutdown flag | `atomic<bool>`, release/acquire | Requires happens-before. **Relaxed would be wrong here** |
| `Connection` outbound buffer | Plain `std::mutex` | Every accessor is a writer; `shared_mutex` gains nothing and costs more |
| `BlockingQueue` | `mutex` + 2 `condition_variable`s | "Wait" semantics are required; atomics can only spin |

### 4.8 Why `balance` cannot be a plain `atomic<int64_t>`

This is the most commonly misunderstood point in the design, so it is written out in full.

A holds 5000. Two concurrent A→B transfers of 3000 each arrive. With a CAS loop:

```
T1: load A → 5000, check 5000 >= 3000 ✓, CAS(5000→2000) succeeds
T2: CAS fails → retry → load 2000, check 2000 >= 3000 ✗ → rejected   ← this part is correct
```

That is not where the problem is. T1's complete operation is four things:

```
① A.balance -= 3000     ← the atomic covers this
② B.balance += 3000     ← a different atomic; no atomicity between ① and ②
③ INSERT entry(A, -3000)
④ INSERT entry(B, +3000)

In the instant after ① completes and before ② begins, a third thread runs an audit
→ it observes the system total short by 3000
→ the central invariant of double-entry bookkeeping is observably broken
```

**Conclusion**: an atomic protects **one variable**. What is required here is **four things happening together**, which is the semantics of a mutual-exclusion region.
Atomics suit things that do not need to change in step with anything else (counters, flags, IDs). `shared_mutex` suits things that must change together and are read far more than written (balances, the account table).

---

## 5. Stage-by-stage record

### 5.1 Stage 0 — Architecture

**Output**: `docs/stage-0-architecture.md` (written in Chinese; includes data-flow, deadlock-comparison, and three-option timeline diagrams).

**Three settled decisions:**

| Decision | Outcome | Architectural consequence |
|---|---|---|
| Currencies | Multi-currency | Account identity includes currency; invariants become per-currency; decimal places must be looked up |
| Idempotency | Included | Protocol gains `idempotency_key`; `transactions` gains a UNIQUE index; memory gains a sharded cache |
| Legs per transaction | Fixed at two | One transaction is exactly two entries; only two accounts ever need ordering |

**Two layers of idempotency protection** (to be implemented in Stage 6):

| Layer | Mechanism | Responsible for |
|---|---|---|
| In-memory `IdempotencyCache` | 64-shard hash map, 24 h TTL | **Performance.** High-frequency retries return the cached result. Losing it on restart is fine — it is not the source of correctness |
| DB `UNIQUE(idempotency_key)` | Unique index; conflict returns SQLSTATE `23505` | **Correctness.** The only real guarantee. Catch 23505 → look up the original transaction → return the original result |

> With only the memory layer, a restart causes double charges. With only the DB layer, every retry wastes a round trip.
> **The memory layer is a cache. The DB layer is the truth.**

**The one thing Stage 0 left undecided**: section 5, "memory vs. DB relationship", options A/B/C. See §13.

---

### 5.2 Stage 1 — Project skeleton

**Verification:**

```
$ cmake -S . -B build -DLEDGER_WERROR=ON && cmake --build build
(zero warnings, zero errors)

$ ctest
100% tests passed, 0 tests failed out of 3

$ ./build/src/ledger_engine --version
ledger_engine 0.1.0 | RelWithDebInfo | gcc 13.3.0 | sanitizer: none

$ make tsan
100% tests passed, 0 tests failed out of 3
```

All of the above was run against a **fresh clone**, confirming the repo is self-contained.

**Five deliberate decisions:**

1. **`src/main.cpp` contains `#if !defined(__linux__) #error`.** The dev machine is macOS (Apple Silicon), where `<sys/epoll.h>` does not exist. Rather than discovering this in Stage 4, the build fails at compile time and points at `make dev`. **The development workflow must go through Docker/colima.**
2. **Postgres is exposed on 5433, not 5432**, to avoid colliding with a pre-existing local Postgres.
3. **The build directory is a named volume**, so macOS and Linux-container CMake caches cannot corrupt each other.
4. **Tests do not link `ledger_warnings`.** GoogleTest macros expand into code that trips `-Wconversion`, and that is not our bug.
5. **`db/migrations/` is mounted as `/docker-entrypoint-initdb.d`**, so the container runs the SQL in filename order on first initialisation.

---

### 5.3 Stage 2 — Database schema

**Verification:**

```
$ make db-all
✓ schema rebuilt
 passed | failed | total
     22 |      0 |    22
✓ all invariants pass — the ledger balances
```

**Three techniques for pushing rules down into the database** (the most valuable part of Stage 2):

#### ① A composite foreign key enforces currency consistency

`accounts` carries a seemingly redundant `UNIQUE (id, currency)`, which lets `transactions` and `entries` declare `FOREIGN KEY (account_id, currency)`.

The effect: a cross-currency transfer is **structurally unwritable**. The pair `(1001, 'JPY')` does not exist in `accounts`, so the foreign key finds no parent row. No application check, no trigger.

#### ② A deferred constraint trigger enforces "the two legs sum to zero"

`CHECK` sees only one row and cannot express a cross-row rule. The two entries are inserted separately, so an immediate check would fire when only one exists — but that is a legitimate intermediate state.

The fix: `CREATE CONSTRAINT TRIGGER ... DEFERRABLE INITIALLY DEFERRED`, deferring the check to COMMIT. **The database refuses to commit an unbalanced transaction.**

#### ③ Triggers enforce append-only

`BEFORE UPDATE` and `BEFORE DELETE` raise exceptions. An audit trail that can be edited is not an audit trail.

**One correction to the Stage 0 design:**

Stage 0 said global transaction IDs would come from an `atomic fetch_add`. That is correct for the in-memory `Journal` sequence, but **it cannot serve as the persisted `transactions.id`** — an in-process counter resets on restart and collides across instances. Changed to `GENERATED BY DEFAULT AS IDENTITY` (a Postgres sequence). The atomic counter is retained for the Journal and metrics.

**Both verification scripts are necessary:**

| Script | Question it asks | What it catches |
|---|---|---|
| `constraint_tests.sql` | Can bad data get in? | Whether the schema itself is actually in force |
| `invariants.sql` | Is the data currently correct? | Cross-table inconsistencies such as lost updates |

Why both: every row written by a lost update **is individually legal** — the balance is positive, the entries balance, the currency is right. What is broken is the relationship between the balance snapshot and the entry facts, a cross-table property that only I2 actively compares.

**I2's detection power has been measured**: after manually injecting a lost update (a balance change with no matching entry), I2 immediately reported drift = −3000 and I3 simultaneously reported a non-zero total for that currency.

**Seed data design intent**: money cannot appear from nowhere. For Alice to hold $1,200, some other account must be $1,200 poorer. The seed therefore transfers *from a system account* rather than doing `UPDATE balance`, so I1 holds from the very first row.

---

### 5.4 Stage 3 — In-memory ledger core

**Verification:**

```
Normal build (-DLEDGER_WERROR=ON)  37/37 passed, 0 warnings
ThreadSanitizer                    37/37 passed, 0 data races
AddressSanitizer + UBSan           37/37 passed, 0 errors
```

**Three design decisions:**

#### ① Account is neither copyable nor movable

`AccountRegistry::find()` returns a raw `Account*`, and the caller takes the account lock **after releasing the registry lock**. That pointer must remain valid forever, so the object must never move. Deleting the copy and move constructors pins this precondition down in the type system instead of writing it in a comment and hoping someone reads it.

Three conditions must all hold:
1. `unordered_map` is node-based — rehashing does not relocate nodes.
2. The map stores `unique_ptr`, so `Account` addresses never change.
3. **Accounts are only ever inserted, never erased** (closing an account sets a `closed` flag).

> ⚠️ **If anyone later adds `erase()`, `find()` starts returning dangling pointers** — and that use-after-free only manifests sporadically under high concurrency, making it extremely hard to reproduce.

#### ② Ordered locking

```cpp
if (req.from == req.to) return ErrorCode::SelfTransfer;  // must precede locking

Account* lo = from;
Account* hi = to;
if (lo->id() > hi->id()) std::swap(lo, hi);

std::unique_lock loGuard(lo->mutex);
std::unique_lock hiGuard(hi->mutex);
```

Without the self-transfer guard, `lo` and `hi` are the same `Account*` and a non-recursive mutex gets locked twice — UB, in practice a thread deadlocking against itself, presenting as a random hang.

#### ③ The inverted use of `auditMutex_`

```
transfer()  takes shared_lock   ← the operation that WRITES data
audit()     takes unique_lock   ← the operation that READS data
```

Because this lock does not protect a variable; it protects "whether any transfer is currently in flight". Transfers exclude each other via account locks, so they all take shared at zero cost; only the rare audit needs the whole system to stand still.

`transfer()`'s `auditGuard` is function-scoped and held all the way through `journal_.append()`, so by the time `verifyInvariants()` acquires the unique lock, no transfer can be stuck in the window between "balance updated" and "entry written".

**Two real bugs found during implementation:**

**① Auto-generated IDs collided with explicit ones.** `createWithId(1001, ...)` did not advance `nextId_`, so `create()` eventually produced 1001 and hit an existing account. Fixed with a CAS loop that raises the watermark to `id + 1`. **This is the same problem as the `setval()` call in `dev_seed.sql`** — after inserting an explicit primary key, the sequence must be advanced to match. One pitfall, stepped in once in memory and once in the database.

**② `shared_mutex` writers are extremely sensitive to reader density.**

| Worker thread behaviour | Audit thread completions in the same window |
|---|---|
| Tight loop, no yield | **0** (killed by a 50-second timeout) |
| One `yield()` between transfers | ~**6000** |

The difference is a single line. libstdc++'s `shared_mutex` sits on pthread rwlock, which is reader-preferring by default, and writers genuinely starve. The concern recorded as "Limitation 2" in Stage 0 was confirmed experimentally. Stage 8 must quantify this and state it honestly in the README.

**Reverse validation: proving the tests actually catch things**

A test that cannot fail is worthless. Every critical test was verified against a deliberately broken build:

| Sabotage | Result |
|---|---|
| Remove `std::swap` (lock source account first) | Deadlock test hangs, killed by 30 s timeout ✅ |
| Change account locks to `shared_lock` | Conservation test reports 19,991,919 ≠ 20,000,000; I2 simultaneously reports balance/entry mismatch ✅ |

The second one is worth remembering: **96,000 transfers all "succeeded" with no error message anywhere, and 8,081 units of money were simply gone.** That is what a lost update looks like — it does not raise an error, it just makes money disappear. Without the I3 assertion, this bug reaches production.

**The five concurrency tests:**

| Test | What it catches |
|---|---|
| `TotalMoneyIsConserved` | 32 threads, 96,000 transfers, 20 accounts. The total must match **to the unit** |
| `ConcurrentWithdrawalsCannotOverdraw` | Account holds 1000, each thread withdraws 100. **Exactly** 10 succeed |
| `OppositeDirectionTransfersDoNotDeadlock` | Two accounts, 640,000 opposing transfers. Deadlock presents as a hang, judged by the ctest timeout |
| `AuditSeesConsistentSnapshotDuringTraffic` | Every audit must observe the correct total |
| `RegistryGrowthDoesNotInvalidatePointers` | 20,000 accounts created during live transfers; rehashing must not invalidate `Account*` |

---

### 5.5 Stage 4 — epoll TCP server

**Verification:**

```
Normal build (WERROR)  56/56 passed, 0 warnings, format check clean
ThreadSanitizer        56/56 passed, 0 data races
AddressSanitizer       56/56 passed, 0 errors
Manual nc test         passed, clean shutdown on Ctrl-C
```

> ⚠️ **From Stage 4 onward, Linux is required.** `epoll`, `eventfd`, and `accept4` are Linux-only. CMake handles this conditionally: `make test` still runs on macOS (minus the 10 networking tests), but the server itself will not build.

**Two hard rules of edge-triggered I/O, with measurements**

**① Read until `EAGAIN`.** Changing `handleRead` to read once:
```
received only 131018 / 1048576 bytes — ET mode did not drain to EAGAIN, the connection went silent
```

**② Register `EPOLLOUT` when a write is incomplete.** Removing that registration:
```
received only 3994597 / 4194304 bytes — partial write unhandled, the remainder vanished
```

Both bugs appear only when a single burst exceeds the read chunk, or when the client reads slowly. Poking at it with `nc` looks fine forever; the first load test blows up.

**★ A lesson about test quality**

**The first version of the partial-write test was fake** — it still passed against deliberately broken code.

The reason: the client was still sending at that point, so every read event called `flushOutput()` again and incidentally finished the previous incomplete write. **The test was rescued by traffic it did not control**, and tested nothing at all.

The fix: a 300 ms pause after the client finishes sending. With no read events available to drive the write, the only remaining path is `EPOLLOUT`. Only then does the test catch the bug.

> **"The test passes" ≠ "the test is testing something."** This is only discoverable by deliberately breaking the code.

**TSan found three races that code review would not have:**

| Location | Why it is a race |
|---|---|
| `EventLoop::threadId_` | Written **once** by the loop thread inside `run()`, read by every `runInLoop()` caller |
| `Acceptor::accepted_` | Incremented by the loop thread, read by test/monitoring threads |
| `EchoServer::connections_.size()` | The loop thread mutates the map while an external thread reads size |

All changed to `std::atomic`. The first is especially worth noting: **the write happens exactly once**, looks entirely harmless, and is essentially undetectable by reading the code. That is the argument for TSan in CI.

**Five details that are easy to miss:**

- **`SIGPIPE` must be ignored.** Writing to a closed socket otherwise **kills the whole process**.
- **`EPOLLOUT` must be deregistered when finished**, or the event loop spins at 100% CPU.
- **Call `handleRead()` before handling hangup.** Reversed, the final batch from a "send then immediately close" client is lost.
- **`ECONNABORTED` must `continue`.** A client vanishing after the handshake is normal and must not stop the accept loop.
- **Tests bind port 0.** Let the OS pick, so CI jobs do not fight over ports.

---

### 5.6 Stage 5 — Protocol + thread pool (5a / 5b / 5c)

> 📌 **This stage has no corresponding design document.** Stages 0–4 each have one; Stage 5 does not yet. What follows comes from development records. Verify against the actual code in the repo, and consider writing `docs/stage-5-protocol-threadpool.md`.

#### 5a — Wire protocol and dual codecs

**Core design: a field-descriptor table**

`fields()` returns a `std::tuple` of `Field` structs. Every message field is **declared exactly once**, and both `BinaryCodec` and `JsonCodec` walk that same table to produce their own encoding.

**Why**: two hand-written codecs will inevitably drift — someone adds a field, updates one side, and binary works while JSON silently drops data. The descriptor table makes "forgot to update the other one" structurally impossible.

**All int64 values are encoded as JSON strings.** JavaScript's `Number` is an IEEE-754 double and loses precision beyond 2⁵³. Encoding ledger amounts as JSON numbers is a landmine; they are encoded as strings and the parser converts back to int64.

**A pitfall hit here: `Result<ErrorCode>` is semantically contradictory.** When `T` is itself `ErrorCode`, the success and error types coincide and the implicit constructors become ambiguous. Resolved with a `static_assert` blocking that instantiation, plus switching the JSON codec's field parser to an output-parameter pattern.

#### 5b — Thread pool

**Lifecycle uses `std::jthread` + `std::stop_token` + `condition_variable_any`.**

- `close()`: **drains** remaining work before finishing (graceful drain)
- `abort()`: stops immediately

`condition_variable_any` rather than `condition_variable`, because only the former accepts the `stop_token` predicate overload, letting a stop request wake waiting workers directly.

#### 5c — Full server wiring

**`RequestHandler` is an abstract interface held by `unique_ptr`, not a `std::function`.**

The reason is concrete: **Stage 6 handlers will own a move-only `pqxx::connection`**. `std::function` requires a copyable callable, and move-only state cannot live inside it. This decision exists specifically to make Stage 6 possible.

**Two independent thread pools:**

| Port | Protocol | Workers | Queue capacity |
|---|---|---|---|
| 9000 | binary | 20 | 8192 |
| 9001 | JSON | 4 | 1024 |

They are separate because the JSON port exists for human debugging and Locust load testing, and should not compete with production traffic for workers.

**Lifetime management:**
- `ConnectionContext` holds `Connection` by **`weak_ptr`** to avoid a reference cycle.
- `Task` holds `ResponseSink` by **`weak_ptr`**, implementing invariant W2 (client disconnects mid-flight → result discarded).

---

## 6. Current directory layout

```
ledger-engine/
├── CMakeLists.txt              # C++20, strict warnings (incl. -Wconversion)
│                               # LEDGER_ENABLE_TSAN / ASAN options
├── docker-compose.yml          # postgres:16 (host port 5433) + engine
├── Dockerfile.dev              # Ubuntu 22.04 + libpqxx + gtest + gdb + clang-format
├── Makefile                    # up/shell/build/test/tsan/asan/fmt
│                               # db-status/db-reset/db-seed/db-check/db-test/db-all/psql
├── .clang-format               # Google style, 100 cols, grouped include sorting
├── .github/workflows/ci.yml    # build + test + TSan/ASan matrix + format + schema job
│
├── db/
│   ├── migrations/
│   │   ├── 000_schema_migrations.sql   # migration version table
│   │   ├── 001_currencies.sql          # six currencies + exponent
│   │   ├── 002_accounts.sql            # includes the crucial UNIQUE (id, currency)
│   │   ├── 003_transactions.sql        # idempotency key UNIQUE + composite currency FK
│   │   └── 004_entries.sql             # deferred balance trigger + append-only trigger
│   ├── seeds/dev_seed.sql              # dev_transfer() + 8 accounts + 6 transactions
│   └── checks/
│       ├── constraint_tests.sql        # 22 deliberate-violation tests
│       └── invariants.sql              # I1–I5 health check
│
├── include/ledger/
│   ├── common/     Types.h  Result.h  Config.h  Logger.h  Metrics.h
│   ├── money/      Currency.h  Money.h
│   ├── core/       Account.h  AccountRegistry.h  LedgerCore.h
│   │               Journal.h  IdempotencyCache.h
│   ├── concurrent/ BlockingQueue.h  ThreadPool.h  Task.h
│   ├── net/        EventLoop.h  Acceptor.h  Connection.h  Buffer.h  Socket.h
│   ├── proto/      Frame.h  Messages.h  Codec.h
│   └── db/         PgPool.h  LedgerRepository.h      ← filled in Stage 6
│
├── src/                        # matching .cpp files + main.cpp (with __linux__ guard)
│
├── tests/
│   ├── test_version.cpp        # Stage 1, 3 tests, validates the toolchain
│   ├── test_money.cpp          # Stage 3, incl. JPY exponent=0 and overflow edges
│   ├── test_ledger_core.cpp    # Stage 3, single-threaded behaviour and rejection paths
│   ├── test_concurrency.cpp    # Stage 3, 5 concurrency tests — the TSan battleground
│   ├── test_buffer.cpp         # Stage 4, 9 tests, platform-independent
│   ├── test_echo_server.cpp    # Stage 4, 10 tests, real sockets, Linux only
│   └── test_codec.cpp          # Stage 5
│
├── bench/                      # filled in Stage 8
│   ├── tcp_client.py           # custom TCP client for Locust
│   └── locustfile.py
│
├── docs/
│   ├── stage-0-architecture.md
│   ├── stage-1-skeleton.md
│   ├── stage-2-schema.md
│   ├── stage-3-ledger-core.md
│   ├── stage-4-epoll-server.md
│   └── (stage-5 document missing)
│
└── README.md                   # English (the portfolio front door)
```

---

## 7. Invariants — the referee for correctness

These five are the sole standard for deciding whether the system is broken. The SQL versions live in `db/checks/invariants.sql`; the in-memory versions live in `test_concurrency.cpp`.

| Tag | Statement | What it catches |
|---|---|---|
| **I1** | Every transaction's two legs sum to zero | Double-entry itself |
| **I2** | Balance snapshot == sum of that account's entries | **The killer for lost updates** |
| **I3** | Per-currency totals are conserved | **The single most important test in the project**: after 32 threads and 100,000 random transfers, totals must match exactly |
| **I4** | User account balances never go negative (including under concurrent load) | Double-spending |
| **I5** | The same `idempotency_key` yields the same `transaction_id` | Idempotency |

```sql
-- I1: passes only if this returns the empty set
SELECT transaction_id FROM entries
GROUP BY transaction_id HAVING SUM(amount) <> 0;

-- I2: catches lost updates
SELECT a.id, a.balance, SUM(e.amount) AS recomputed
FROM accounts a JOIN entries e ON e.account_id = a.id
GROUP BY a.id, a.balance
HAVING a.balance <> SUM(e.amount);

-- I3: per-currency conservation
SELECT currency, SUM(balance) FROM accounts GROUP BY currency;
```

**Why signed integers instead of a direction enum**: the invariants become one-line SQL. The cost is that it does not match traditional accounting DR/CR presentation, solved with a generated `direction` column. **Signed for correctness, direction for presentation.**

---

## 8. Lock hierarchy and the deadlock argument

### Global lock hierarchy (acquire low to high, never the reverse)

```
L0 = registry (map-level shared_mutex)
 ↓
L1 = account locks (ascending account_id)
 ↓
L2 = journal
```

Plus one orthogonal `auditMutex_` (see §5.4 ③).

### The formal deadlock guarantee

Coffman's four conditions: mutual exclusion, hold-and-wait, no preemption, and **circular wait**. The first three cannot be avoided here, so the fourth is broken.

**Naïve approach (lock the source first)**: T1 holds 1001 and waits for 2002; T2 holds 2002 and waits for 1001 → cycle → permanent deadlock.

**Ordered approach**: `auto [lo, hi] = std::minmax(fromId, toId);` — always lock `lo` first. Both T1 and T2 take 1001 then 2002. When T2 blocks on 1001 it **holds no locks at all**, so it can never obstruct anyone else. No cycle is possible.

> **Proof**: define a total order on accounts (ascending `account_id`) and require every lock acquisition sequence to be an increasing subsequence. If a cycle T₁→T₂→…→Tₙ→T₁ existed, walking it once would require the id to strictly increase and yet return to its starting value — a contradiction.
>
> **This is a proof, not a rule of thumb.**

### Three edge cases that must be handled

1. **`from == to`** — otherwise `loId == hiId` and a non-recursive mutex is locked twice. UB, presenting as a random hang.
2. **The DB must use the same order** — `SELECT ... FOR UPDATE` needs `ORDER BY id`, or Postgres row-lock deadlocks follow (SQLSTATE `40P01`). **This matters especially in Stage 6.**
3. **The lock hierarchy is global** — low to high only.

### Why not `std::scoped_lock(m1, m2)`

1. Under contention it repeatedly tries, releases, and retries; tail latency becomes unpredictable and livelock is possible.
2. It cannot directly handle `shared_mutex` in shared mode.
3. Explicit ordering is O(1), predictable, and explainable in three sentences.

**The choice here is explainability, not convenience.**

### Two known limitations (to be stated honestly in the README)

**Limitation 1: there is no globally consistent cross-account snapshot.** A transfer locks only two accounts, so a third thread locking A, reading, releasing, then locking B may see A's new value alongside B's old one and falsely report an imbalance.
**Solution (already implemented in Stage 3)**: the global `auditMutex_` — transfers take shared, audits take unique.

**Limitation 2: `shared_lock` is not free.** `lock_shared()` increments a reader count, which is a write; on a multi-core machine reading the same account frequently, that cache line ping-pongs between cores.
**Response**: Stage 8 runs a dedicated "pure balance query" pass; only if it is genuinely the bottleneck should seqlock or RCU be considered. **Measure first, then decide.**

---

## 9. Build and verification: full procedure

### Step 1 — Bring up the container runtime

Docker Desktop has previously failed on this machine with `Docker Desktop is unable to start`. colima is the lighter alternative:

```bash
brew install colima docker docker-compose
colima start --cpu 4 --memory 4
```

**Expected**: colima reports `done`, and `docker ps` no longer complains about a missing daemon.

### Step 2 — Start Postgres and the dev container

```bash
cd /Users/haochensmacbookpro/Downloads/ledger-engine
make up
make shell
```

**Expected**: a shell prompt inside the container. Postgres is on host port **5433**, not 5432.

### Step 3 — Build (strict mode)

```bash
cmake -S . -B build -DLEDGER_WERROR=ON
cmake --build build -j
```

**Expected**: zero warnings, zero errors. Any warning becomes an error under `-Werror`.

### Step 4 — Run unit and concurrency tests

```bash
cd build && ctest --output-on-failure
```

**Expected**: everything passes. It was 56/56 at the end of Stage 4; the count is higher after Stage 5 — trust the actual output.

### Step 5 — Sanitizer runs

```bash
make tsan
make asan
```

**Expected**: both pass completely — `ThreadSanitizer: 0 data races`, and no ASan/UBSan errors.

### Step 6 — Full database flow

```bash
make db-all
```

**Expected:**
```
✓ schema rebuilt
 passed | failed | total
     22 |      0 |    22
✓ all invariants pass — the ledger balances
```

Individual targets: `make db-status` / `db-reset` / `db-seed` / `db-check` / `db-test` / `psql`

### Step 7 — Format check

```bash
make fmt
```

**Expected**: no diff. CI's format job uses the same `.clang-format`.

### Step 8 — Manual connection test

```bash
# binary port
nc localhost 9000
# JSON port (human-readable, for debugging)
nc localhost 9001
```

**Expected**: a valid frame produces the corresponding response; Ctrl-C shuts the server down cleanly with no leaked fds.

---

## 10. Environment pitfalls

Every one of these cost real time:

| Pitfall | Symptom | Fix |
|---|---|---|
| **macOS-only build failures are invisible to Linux CI** | e.g. AppleClang's `-Wunused-but-set-variable` with `-Werror` breaks locally while CI stays green | Cross-platform validation needs explicit attention; CI alone is not sufficient |
| **Patch files containing prior-stage formatting hunks** | Patch mismatch on apply | Diff only against the files actually changed in that stage |
| **`pip install clang-format==14.0.6` on Apple Silicon** | Binary fails to run | That pip package ships an x86_64 binary and requires Rosetta 2 |
| **`~/.zshrc` export missing `:$PATH`** | Every shell command suddenly not found | Always append when modifying PATH: `export PATH=/new/path:$PATH` |
| **Docker Desktop will not start** | `Docker Desktop is unable to start` | Use colima instead (see §9 Step 1) |
| **Postgres port collision** | Connects to a pre-existing local Postgres | The project deliberately uses **5433** |

> 📌 These findings should become a "Cross-platform build notes" section in the README. This is on the to-do list and must land before Stage 9.

---

## 11. What is not done yet

| Item | Notes |
|---|---|
| **Stage 6: PostgreSQL integration** | See §12 |
| Stage 7: full TSan / ASan concurrency validation | Each stage runs them, but there is no whole-system test spanning core + net + pool + DB |
| Stage 8: Locust load testing | `bench/tcp_client.py` and `locustfile.py` are unwritten |
| Stage 9: README / diagrams / résumé evidence | The README is still the Stage 1 draft |
| `IdempotencyCache` implementation | Designed in Stage 0 (64 shards), not yet written. Stage 6 needs it |
| `docs/stage-5-*.md` | Stage 5 has no design document |
| README cross-platform build notes | Promised, not yet written |
| Formal sign-off on Stage 0 §5 options A/B/C | See §13 |

---

## 12. Stage 6 handover notes

### Goal

Connect the Stage 3 in-memory `LedgerCore` to PostgreSQL to produce genuinely durable transfers.

### Groundwork already laid

| Stage 5 decision | What it enables in Stage 6 |
|---|---|
| `RequestHandler` as an abstract interface held by `unique_ptr` | Handlers can own a move-only `pqxx::connection` |
| `makeLedgerHandlerFactory` is a factory lambda | **Each worker thread opens its own `pqxx::connection` inside the factory** |
| Every ThreadPool worker gets its own handler instance | Connections are held long-term, not borrowed per request |
| `LedgerRepository` designated as the only place writing SQL | Changing persistence strategy touches one layer |

### Concrete to-do list

1. Implement `db/PgPool.h` (or let the factory manage connections directly, depending on the final design)
2. Implement `db/LedgerRepository`: translate one transfer into one Postgres transaction
3. Add the DB round trip inside `LedgerCore::transfer()`'s critical section (option B)
4. Implement `IdempotencyCache` (64 shards) and wire up the SQLSTATE `23505` lookup path
5. Add integration tests that hit a real Postgres and then verify I1–I5 via `invariants.sql`

### The SQL skeleton (from Stage 0 §6)

```sql
BEGIN;

SELECT id, balance FROM accounts
  WHERE id IN (1001, 2002)
  ORDER BY id FOR UPDATE;   -- ⚠️ order must match the in-memory lock order, or 40P01

INSERT INTO transactions(id, idempotency_key, ...)
  VALUES (DEFAULT, 'req-a3f9-01', ...);
  -- on UNIQUE violation → SQLSTATE 23505 → idempotency layer 2 kicks in
  -- roll back, look up the original transaction, return the original result

INSERT INTO entries VALUES
  (900001, 1001, 'USD', -5000, 115000),
  (900001, 2002, 'USD', +5000,  47000);

UPDATE accounts SET balance = balance - 5000, version = version + 1 WHERE id = 1001;
UPDATE accounts SET balance = balance + 5000, version = version + 1 WHERE id = 2002;

COMMIT;   -- WAL fsync. The transaction only truly exists at this moment
```

### Option B's ordering (critical)

```
take in-memory locks
  → validate balance
  → complete the Postgres transaction WHILE HOLDING THE LOCKS
  → apply the in-memory balance ONLY after COMMIT succeeds    ← never reorder these
  → release locks
  → respond
```

**If COMMIT fails, memory is left completely untouched and `DB_ERROR` is returned.**

This ordering buys a crash semantics that fits in one sentence: **memory only changes after a successful DB COMMIT, so memory can never run ahead of the database.** On restart, `SELECT id, balance FROM accounts` rebuilds everything in one pass. No replay logic required.

### Option B's weakness (state it honestly in the README — this is the bonus point)

The critical section contains a ~1.5 ms DB round trip, so a **single account has a theoretical ceiling of 1 ÷ 1.5 ms ≈ 660 TPS**, no matter how many workers exist.

Stage 8 must test two distributions:

| Traffic distribution | Expected TPS |
|---|---|
| Uniform (1000 accounts, random pairs) | ≈ **9,000**, determined by worker count |
| Hotspot (everyone transfers to one merchant account) | ≈ **660**, independent of worker count |

Putting that comparison chart in the README is far more convincing than reporting a single "9,000 TPS" figure, because it demonstrates knowing **when the system breaks**.

---

## 13. Open questions

### 🔴 Question 1: Stage 0 §5's A/B/C decision was never formally made

The Stage 0 document still says: "**Undecided** — options A / B / C in section 5 must be chosen before Stage 1."

In practice, everything built in Stages 1–5 (a full lock design in the in-memory `LedgerCore`, one DB connection per worker, `RequestHandler` needing to hold a `pqxx::connection`) **only makes sense under option B**. The project has been on B's path all along.

| Option | Mutual exclusion | Response timing | Estimated TPS | Crash semantics |
|---|---|---|---|---|
| A: DB is the sole source of truth | Postgres row locks | After COMMIT | 3,000–8,000 | Cleanest |
| **B: in-memory decision + synchronous write-through** ★ | In-memory `shared_mutex` | After COMMIT | 3,500–9,000 writes / hundreds of thousands for pure reads | Memory never runs ahead of the DB |
| C: in-memory authority + batched write-behind | In-memory `shared_mutex` (very short critical section) | After batch flush / immediately | 30,000–80,000 | Complex; requires a bespoke WAL and replay |

**Four reasons B is recommended:**
1. It is the only option consistent with the project's goals. Option A turns `shared_mutex`, the event loop, and the 20 workers into decoration — Postgres does the real mutual exclusion. Asked "what problem does your lock design solve?", option A has no answer.
2. Option C's hard part — rolling back already-applied in-memory state when a batch flush fails — is a distributed-systems-grade problem, and getting it wrong produces an unbalanced ledger. For a portfolio piece whose selling point is ACID bookkeeping, that is the most embarrassing possible failure.
3. B's crash semantics fit in one sentence.
4. B preserves the upgrade path to C. If Stage 8 needs more TPS, replacing COMMIT with group commit is C's safe variant and **requires no change to the lock design at all**.

**→ Recommended action**: before starting Stage 6, change the status line in `stage-0-architecture.md` from "undecided" to "**Decided: option B**" and add a note. Otherwise the document actively misleads whoever picks this up.

### 🟡 Question 2: Stage 0 §7's "three things to confirm"

| Item | Default | Actual status |
|---|---|---|
| Currency list | USD / EUR / JPY / GBP / CNY / TWD | ✅ All six kept, landed in Stage 2 |
| Protocol format | Length prefix + binary | ✅ Stage 5 chose "binary primary + JSON debug port", two ports |
| Account ID source | Server-generated (BIGINT ascending) | ✅ Stage 2 uses Postgres IDENTITY; the in-memory version has `createWithId()` |

All three are in fact resolved; the Stage 0 document can be updated to match.

---

## 14. Self-check questions for whoever takes over

Being able to answer these without the document means the system is genuinely understood.

**Architecture**
- **Q1** How much of a transfer does the event loop thread spend on? Why can the DB call not live there?
- **Q2** Why can a worker not `write()` the socket itself after finishing? What would the client see? Why can't TSan catch it?

**Concurrency**
- **Q3** A→B and B→A happen simultaneously. Walk through both threads' lock acquisition order and explain why the wait-for graph cannot form a cycle.
- **Q4** Why can `balance` not be a `std::atomic<int64_t>`? Give a concrete numeric example.
- **Q5** Why is it safe to release the registry's `shared_lock` after obtaining an `Account*`? What are the three preconditions, and what happens if any one is violated?
- **Q6** Why does a transfer take `auditMutex_` in shared mode while an audit takes it in unique mode? What does that lock actually protect?

**Data**
- **Q7** The DB COMMIT succeeds but the process crashes before the response is sent. What happens when the client retries? Which layer stops the double charge?
- **Q8** What is `int64 amount = 5000` worth in USD versus JPY? Where in the code is that difference looked up?
- **Q9** Which invariant catches data written by a lost update? Why do the others miss it?

**Networking**
- **Q10** What happens if ET mode does not drain to `EAGAIN`? Why does testing with `nc` never reveal it?
- **Q11** Why must `EPOLLOUT` always be deregistered once writing completes?

---

## 15. Provenance and confidence

| Content | Source | Confidence |
|---|---|---|
| Stage 0 architecture, module table, lock design, three options | `docs/stage-0-architecture.md` | ✅ Verbatim from the document |
| Stage 1 skeleton, verification output, five decisions | `docs/stage-1-skeleton.md` | ✅ Verbatim |
| Stage 2 schema, 22 tests, three DB techniques | `docs/stage-2-schema.md` | ✅ Verbatim |
| Stage 3 ledger core, five concurrency tests, reverse validation | `docs/stage-3-ledger-core.md` | ✅ Verbatim |
| Stage 4 epoll, ET rules, three TSan races | `docs/stage-4-epoll-server.md` | ✅ Verbatim |
| **All Stage 5a/5b/5c content** | **Development records; no matching `.md`** | ⚠️ **Verify against repo code and write the missing document** |
| Environment pitfalls (§10) | Development records | ⚠️ Each one genuinely encountered, but not yet in the README |
| Stage 6 to-do list (§12) | Derived from the Stage 0 design + Stage 5 decisions | ⚠️ A plan, not yet implemented |

---

## Suggested reading order

1. §1–§4 of this document (what the system does, how modules are split)
2. `docs/stage-0-architecture.md` in full — the only complete design document, especially section 6, "the full path of one transfer"
3. §7 (invariants) and §8 (lock hierarchy) of this document — these two are the standard for judging whether a change breaks correctness
4. Run the eight steps in §9 to confirm the environment is alive
5. `src/core/LedgerCore.cpp` — the only file containing lock logic
6. `tests/test_concurrency.cpp` — see what those five tests actually assert
7. §12 of this document, then start Stage 6

---

*End of document.*
