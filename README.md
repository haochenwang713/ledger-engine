# Transactional Ledger & Settlement Engine

A high-concurrency, ACID-compliant double-entry ledger written in C++20. Handles
multi-currency account transfers over a hand-rolled epoll event loop, prevents
double-spending under concurrent load, and guarantees that every transfer is
recorded as a balanced pair of immutable journal entries.

> **Status:** Stage 5 of 9 — the server is live. Two ports, twenty-four worker
> threads, an in-memory ledger, and `nc` is a working client. PostgreSQL is
> next. See [Roadmap](#roadmap) for the rest.

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
100% tests passed, 0 tests failed out of 56
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

## The two rules that make edge-triggered epoll work

`epoll` in edge-triggered mode reports a fd only when its state *changes*.
That makes it cheaper than level-triggered, and it makes two mistakes fatal in
a way that small tests never reveal.

**Drain every readable fd until `EAGAIN`.** If you stop early, the remaining
bytes sit in the kernel buffer, the state never changes again, and epoll never
notifies you. That connection goes permanently silent: the client waits for a
response, the server believes no request arrived. The same rule applies to
`accept()` — stop early and a connection sits in the backlog forever, its
handshake already complete.

**Handle short writes.** When the kernel send buffer fills, `write()` returns
`EAGAIN`. The rest of the response has to stay buffered while you register
`EPOLLOUT` and wait to be told the socket is writable again — and you must
deregister it once drained, or a permanently-writable socket spins the loop at
100% CPU.

Both bugs are invisible below the read-chunk size and under fast clients. The
tests therefore push 1 MB through a single read event and make a client stop
reading mid-response.

Writes are funnelled through the loop thread for a separate reason: two threads
calling `write()` on one socket interleave their bytes, the length prefix stops
lining up, and the protocol desynchronises. That is not a data race — `write()`
is thread-safe — so ThreadSanitizer will never flag it. `Connection::send()` is
callable from any thread and hands the actual write to the loop via an
`eventfd` wakeup.

## What Stage 4's tests prove

| Test | What it would catch |
|---|---|
| `EdgeTriggeredReadDrainsTheEntireSocket` | 1 MB through one read event. A missing drain loop delivers ~131 KB and then hangs |
| `HandlesPartialWritesWhenClientStopsReading` | Client stops reading mid-response. Without `EPOLLOUT` the tail is silently dropped |
| `ServesManyConcurrentClients` | 50 clients × 20 round-trips on one loop thread |
| `SurvivesAbruptDisconnects` | 200 connect-then-immediately-close cycles. Catches fd leaks |
| `ReadsDataSentImmediatelyBeforeClose` | Data sent just before `FIN` must not be dropped — read has to be handled before hangup |
| `RunInLoopHandlesManyCrossThreadTasks` | 2000 tasks from 8 threads, none lost |

Checked against broken builds, as in earlier stages:

- Reading once instead of draining: 131,018 of 1,048,576 bytes arrive, then the
  connection stalls until the test times out.
- Skipping the `EPOLLOUT` registration: 3,994,597 of 4,194,304 bytes arrive.

The second one is worth a note. The first version of that test passed *even
with the bug present*, because the client was still streaming data and every
read event happened to re-drive the write path. The test was being rescued by
traffic it did not control. Adding a 300 ms pause after the client stops sending
is what made it actually test the thing it claimed to test.

ThreadSanitizer also caught three real races in this stage that reading the code
had not: `EventLoop::threadId_`, `Acceptor::accepted_`, and reading
`connections_.size()` from outside the loop thread. All three are now atomic.

## One field declaration, two encodings

The server speaks two protocols: a length-prefixed binary frame on port 9000,
and newline-delimited JSON on port 9001 so that `nc` is a usable debugging
client. Supporting both by hand would mean writing every message field four
times — binary encode, binary decode, JSON encode, JSON decode. Adding a field
and forgetting one of the four is a silent failure: it compiles, that
encoding's tests pass, and only the *other* encoding is quietly missing a field.

Instead each message declares its fields exactly once:

```cpp
static constexpr auto fields() {
  return std::tuple{
      Field{"idem_key", &TransferReq::idemKey},
      Field{"from",     &TransferReq::from},
      Field{"amount",   &TransferReq::amount},
      Field{"ccy",      &TransferReq::ccy},
  };
}
```

Both codecs walk that tuple and dispatch on the field type, so each one only
implements five primitives rather than one function per message. A field type
with no dispatch arm is a `static_assert`, not a silently skipped field.

**Every integer crosses the JSON boundary as a string.** JavaScript numbers are
IEEE-754 doubles, so `JSON.parse('{"amount":9007199254740993}')` returns
9007199254740992 — no error, no warning, one cent gone. Bare numbers in integer
fields are rejected outright rather than accepted while the values happen to be
small. Protobuf's canonical JSON mapping made the same call for the same reason.

The trade-off is that reordering `fields()` silently changes the binary layout
while leaving JSON untouched, and round-trip tests cannot see it — encoder and
decoder change together and stay consistent with each other. That is what the
byte-level golden table in `test_codec.cpp` is for; it doubles as the protocol
specification.

## Backpressure, and two different ways to stop

Work reaches the workers through a bounded queue. Bounded is the point: an
unbounded queue responds to a slow database by growing without limit and by
queueing requests whose clients timed out long ago. Both are worse than saying
no. The IO thread therefore only ever calls `tryPush`, and turns a refusal into
a `SERVER_BUSY` response — an event loop that blocks stalls *every* connection,
not just the one that was unlucky.

Shutdown needs two distinct meanings, so it gets two mechanisms:

| | Effect |
|---|---|
| `close()` | Refuse new work, but finish what is already queued — those clients are still waiting for an answer |
| `stop_token` | Abandon the queue immediately |

Workers are `std::jthread` blocked in
`condition_variable_any::wait(lock, stop_token, pred)`, so `request_stop()`
wakes them with nobody having to remember to notify, and the destructor cannot
leave a thread behind.

Handlers are an abstract interface owned by `unique_ptr`, not `std::function`.
Stage 6 handlers will hold a `pqxx::connection`, which is move-only, and
`std::function` requires its target to be copyable. The factory runs *on* the
worker thread, so each database connection is created, used, and destroyed on
one thread and needs no lock at all.

## What Stage 5's tests prove

| Test | What it would catch |
|---|---|
| `CodecGolden.*` | Byte-level layout of every message. Reordering `fields()` breaks binary compatibility silently; round-trip tests cannot see it |
| `CodecInt64.ValuesBeyondDoublePrecisionSurvive` | 2⁵³+1 through both codecs |
| `CodecInt64.JsonRejectsBareNumbers` | An unquoted integer is an error, not a lucky small value |
| `FrameSplitter*.OversizedLengthIsRejectedImmediately` | A client sending `len = 0xFFFFFFFF` and then going quiet must not make us wait forever |
| `BlockingQueue.TryPushNeverBlocksWhenFull` | 10,000 rejected pushes must take microseconds, not block |
| `BlockingQueue.CloseWakesEveryBlockedConsumer` | `notify_one` instead of `notify_all` leaves threads asleep and `join()` never returns |
| `ThreadPool.DiscardsResultWhenSinkIsGone` | A client that disconnects while its request is queued |
| `ThreadPool.EachWorkerGetsItsOwnHandler` | The precondition for one database connection per worker |

Checked against broken builds, as in earlier stages:

- `notify_all` → `notify_one` in `close()`: the process hangs forever with no
  output at all and is killed by the ctest timeout.
- `tryPush` → blocking `push` in `submit()`: 200 submissions take **9,780 ms**
  instead of under 1 ms.

The first one is why `gtest_discover_tests` sets a timeout. A missed
notification and a lock-order deadlock both manifest as *never returning*, not
as a failed assertion — the assertion never gets a chance to run, so the
timeout is the only thing that can judge them.

## Talking to it

The JSON port exists so that the server can be driven by hand. Start it and
point `nc` at port 9001:

```
$ ./build/src/ledger_engine
ledger engine up
  binary  port 9000  (length-prefixed)
  json    port 9001  (NDJSON, one object per line)
  workers 20 binary / 4 json (independent queues)
  accounts 5 loaded

$ nc localhost 9001
{"id":"1","type":"get_account","account_id":"1001"}
{"balance":"115000","ccy":"USD","id":"1001","status":"ACTIVE","type":"account","v":1}

{"id":"2","type":"transfer","idem_key":"k1","from":"1001","to":"2002","amount":"2500","ccy":"USD"}
{"from_balance":"112500","id":"2","to_balance":"49500","tx_id":"900004","type":"transfer_ok","v":1}

{"id":"3","type":"transfer","idem_key":"k2","from":"2002","to":"1001","amount":"99999999","ccy":"USD"}
{"code":"INSUFFICIENT_FUNDS","id":"3","message":"INSUFFICIENT_FUNDS","type":"error","v":1}
```

Ctrl-C prints what the run did and re-checks the ledger:

```
stopped.
  json     6 handled, 0 refused (queue full), 0 dropped (client gone)
  transfers 5 committed / 1 rejected
  invariants passed — the ledger balances
```

The demo accounts are the ones from the design document and from
`db/seeds/dev_seed.sql`: Alice at 115000 USD, Bob at 47000, and a JPY account
holding 5000 — which is ¥5,000, not ¥50.00, because JPY has exponent 0. They
are created by transferring out of a system account rather than by setting
balances directly, so I2 holds from the very first row. Stage 6 replaces that
function with `SELECT id, balance FROM accounts` and the same commands should
produce the same answers.

## Two kinds of protocol failure

Turning a byte stream into requests means deciding what to do when the bytes
are wrong, and there are two different answers:

| | What it means | What to do |
|---|---|---|
| **Decode error** | The frame boundary is known, the contents are not understood | Reply with an error, skip that one message, keep the connection |
| **Framing error** | The length field is nonsense, or a line never ends — the stream can no longer be aligned | Reply, then close the connection |

Treating a framing error as recoverable puts the connection into a loop:
garbage in, error out, more garbage in. There is no way to find where the next
message starts, so continuing to read only produces more noise.

Connection lifetime has a matching pair of rules, and both directions have to
be weak:

```
LedgerServer::connections_ ──shared──▶ Connection
                                          │  the read callback captures shared
                                          ▼
                                   ConnectionContext
                                          │  weak  ←── must not be shared
                                          ▼
                                     Connection
```

A `shared_ptr` back to the `Connection` would be a reference cycle: the count
never reaches zero and every closed connection leaks its fd and its buffers.
That failure never crashes — the process just grows, and only a long load test
notices. In the other direction each queued `Task` holds the context weakly, so
a client that disconnects while its transfer is still queued gets its result
discarded rather than written to a dead socket.

## What Stage 5c's tests prove

Eighteen of these use real sockets against a real server with real worker
threads. The most useful one is `ConcurrentClientsPreserveTotalMoney`: eight
clients, half transferring 1001→2002 and half 2002→1001 — precisely the pattern
that deadlocks a naive lock-the-source-first implementation. It exercises the
event loop, both codecs, the queue, the ordered locking, and the response
routing at once, and then asserts that the two balances still sum to what they
did before. Every fixture teardown re-checks the invariants.

`SurvivesClientsDisconnectingMidFlight` fires fifty requests and hangs up
without reading any of them. `FramingErrorClosesTheConnection` sends
`len = 0xFFFFFFFF` and expects a FIN. `GarbageLineGetsAnErrorButKeepsTheConnection`
sends nonsense and then a valid ping on the same socket.

None of `net/` changed in this stage. The event loop, the acceptor, the
connection, and the buffer are byte-for-byte what Stage 4 delivered — which was
the point of building the echo server without a protocol in the first place.

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

### Building on both platforms is a test in itself

Everything except `net/` builds and runs natively on macOS. `net/` needs
`epoll`, `eventfd`, and `accept4`, so it is compiled only on Linux and the
server has to run in the container. The core, the money types, the protocol
layer, and the thread pool are all platform-independent on purpose — they are
the parts that need the fastest edit-compile-test loop.

Keeping that second platform alive costs a little and has already paid for
itself. `main.cpp` parsed `--port` and then only read it inside the
`LEDGER_HAS_EPOLL` branch, so on macOS the variable was written and never read.
AppleClang's `-Wunused-but-set-variable` plus `-Werror` rejected the build. The
Linux CI could never have seen it: on Linux the variable *is* read.

Two compilers disagreeing is information. A warning that only one of them
raises is usually pointing at a code path the other one never compiles.

## Roadmap

| Stage | Deliverable | Status |
|---|---|---|
| 0 | Architecture design, module split, thread model | ✅ Done |
| 1 | Project skeleton, CMake, Docker Compose | ✅ Done |
| 2 | DB schema: `currencies` / `accounts` / `transactions` / `entries` | ✅ Done |
| 3 | Ledger core — in-memory, `shared_mutex`, no DB yet | ✅ Done |
| 4 | epoll TCP server (echo first) | ✅ Done |
| 5 | Wire protocol framing + thread pool | ✅ Done |
| 6 | PostgreSQL integration with `SELECT … FOR UPDATE` | ⬜ Next |
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
