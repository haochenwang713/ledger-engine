# Ledger Engine — Project Progress

A C++20 transactional ledger and settlement engine: double-entry accounting,
multi-currency, deadlock-free concurrent transfers, served over a hand-written
epoll event loop.

**This file is the complete engineering record.** Every step from start to
finish: what it was supposed to deliver, how it was actually delivered, where the
code lives, what the tests prove, and which deliberately broken builds were used
to confirm those tests can fail. [Where we are right now](#where-we-are-right-now)
is at the bottom.

**[README.md](README.md)** is the other half, written for a reader with no
software background: what the problem is, what the engine promises, and how to run
it, with every term explained. That one answers "what is this and how do I use it";
this one answers "how was it built, and why that way". The full wire specification
is here too, in [Appendix D](#appendix-d--protocol-reference).

**Contents:** [the step map](#the-step-map) · [commit timeline](#commit-timeline) ·
Steps [0](#step-0--architecture-and-the-decisions-that-constrain-everything-else)
[1](#step-1--build-skeleton-cmake-docker-ci-two-platform-builds)
[2](#step-2--database-schema-the-ledger-rules-live-in-postgres)
[3](#step-3--in-memory-ledger-core-money-accounts-ordered-locking)
[4](#step-4--epoll-tcp-server-proven-with-an-echo-protocol)
[5](#step-5--wire-protocol-thread-pool-and-the-full-request-path)
[6](#step-6--postgresql-durability-select--for-update-and-real-commits)
[7](#step-7--sanitizer-and-stress-verification-of-the-db-backed-path)
[8](#step-8--load-tests-tuning-tps-and-p95)
[9](#step-9--final-architecture-documentation-and-diagrams)
[10](#step-10--observability-surface-the-engine-reports-on-itself)
[11](#step-11--gateway-a-websockethttp-bridge-to-the-browser)
[12](#step-12--web-console-see-the-ledger-and-the-concurrency-work) ·
[A: invariants](#appendix-a--the-invariants) ·
[B: how correctness was verified](#appendix-b--how-correctness-was-verified) ·
[C: decisions](#appendix-c--decisions-worth-remembering) ·
[D: protocol reference](#appendix-d--protocol-reference) ·
[**where we are right now**](#where-we-are-right-now)

---

## The step map

Thirteen steps. Steps 0–9 are the backend track and keep the same numbering as
the README roadmap and the `Stage N` comments inside the source, so nothing
drifts. Steps 10–12 are the **visibility track** — the frontend that makes the
engine watchable instead of only testable.

| Step | Topic | Status |
|---|---|---|
| 0 | Architecture and the decisions that constrain everything else | ✅ Done |
| 1 | Build skeleton: CMake, Docker, CI, two-platform builds | ✅ Done |
| 2 | Database schema — the ledger rules live in Postgres | ✅ Done |
| 3 | In-memory ledger core: money, accounts, ordered locking | ✅ Done |
| 4 | epoll TCP server, proven with an echo protocol | ✅ Done |
| 5 | Wire protocol, thread pool, and the full request path | ✅ Done |
| 6 | PostgreSQL durability — `SELECT … FOR UPDATE`, real commits | ⬜ Not started |
| 7 | Sanitizer and stress verification of the DB-backed path | ⬜ Not started |
| 8 | Load tests, tuning, TPS and p95 numbers | ⬜ Not started |
| 9 | Final architecture docs and diagrams | ⬜ Not started |
| **10** | **Observability surface — the engine reports on itself** | ⬜ **Next** |
| **11** | **Gateway — WebSocket/HTTP bridge so a browser can talk to it** | ⬜ Next |
| **12** | **Web console — see accounts, transfers, and concurrency live** | ⬜ Next |

**Execution order is not step order.** The numbers above are the canonical stage
ids. The order of work from here is:

```
10 → 11 → 12 → 6 → 7 → 8 → 9
```

The visibility track comes first on purpose. Right now the only proof the engine
works is a green ctest line and a `nc` session; the frontend turns that into
something observable. Building it *before* Postgres also means the console is
already watching when the storage layer is swapped underneath — Step 6 becomes a
change you can see happen rather than a change you have to trust.

### Commit timeline

| Date | Commit | What landed |
|---|---|---|
| 2026-08-24 | `97d885e` | Stage 0–1: architecture design and buildable project skeleton |
| 2026-08-24 | `c702b3a` | Stage 2: database schema with double-entry enforced by the database |
| 2026-08-24 | `e2e1419` | Let the ledger core build on macOS; epoll is only needed by the network layer |
| 2026-08-25 | `28cb402` | Stage 3: in-memory ledger core with ordered per-account locking |
| 2026-08-25 | `c759dbf` | Stage 4: epoll event loop and echo server |
| 2026-08-30 | `6e8b695` | Stage 5a: wire protocol messages, framing, and dual codecs |
| 2026-08-30 | `b439ff7` | Stage 5b: bounded work queue and jthread-based worker pool |
| 2026-08-30 | `f43a9fb` | Stage 5c: wire the protocol and the pools into the event loop |
| 2026-08-30 | `0d86ae4` | Comments to English: `proto/` and `concurrent/` headers |

Stage 5 was split into three commits (a/b/c) on purpose: the protocol, the
threading, and the wiring are independently testable, and landing them together
would have meant debugging a codec bug and a queue bug at the same time.

---

# Step 0 — Architecture and the decisions that constrain everything else
`[Architecture]` `[Documentation]` — ✅ Done

### What was expected to be performed after this stage
- Decide the data model: double-entry, multi-currency, integer money.
- Decide the concurrency model and prove the deadlock strategy on paper.
- Decide the persistence strategy and the acknowledgment boundary.
- Fix a roadmap and the boundaries of v1.

### How it was done
The whole design was written down before any code: module split, thread model,
the deadlock proof, the schema, and the three persistence strategies that were
evaluated and rejected. Three decisions from this document constrain every later
step:

- **Balances are a derived snapshot, not the truth.** The immutable entry log is
  the truth, which is what makes a lost update *detectable* (invariant I2) rather
  than silent.
- **Locks are taken in ascending account id**, never in the request's direction —
  that is what makes the wait-for graph provably acyclic (see Step 3).
- **Durability is the acknowledgment boundary** — a client hears "ok" only after
  Postgres commits. In-memory state is never allowed to run ahead of the DB.

📁 [docs/stage-0-architecture.md](docs/stage-0-architecture.md) (Traditional
Chinese) · [README.md](README.md) · [ledger-engine-handover-en.md](ledger-engine-handover-en.md)

### Outcome
A blueprint specific enough that later stages were implementation, not
re-litigation.

---

# Step 1 — Build skeleton: CMake, Docker, CI, two-platform builds
`[Build]` `[Infrastructure]` — ✅ Done

### What was expected to be performed after this stage
- A C++20 build that compiles and runs.
- A container environment for the Linux-only parts.
- Continuous integration so regressions are caught automatically.

### How it was done
CMake sets C++20 as a hard requirement at
[CMakeLists.txt:11](CMakeLists.txt#L11), with warnings-as-errors available via
`-DLEDGER_WERROR=ON` and sanitizer presets wired in. `make doctor` reports which
tools each stage needs, so nobody has to install everything on day one.

A deliberate choice here paid off later: **everything except `net/` builds
natively on macOS**, and only the epoll layer is Linux-only. `main.cpp` guards it
with `LEDGER_HAS_EPOLL` ([src/main.cpp:15](src/main.cpp#L15)) instead of an
`#error`, so the ledger core, money types, protocol, and thread pool all keep a
fast native edit-compile-test loop. Two compilers disagreeing is information —
AppleClang's `-Wunused-but-set-variable` caught a `--port` bug that the Linux CI
structurally could not see.

📁 [CMakeLists.txt](CMakeLists.txt) · [src/CMakeLists.txt](src/CMakeLists.txt) ·
[Makefile](Makefile) · [docker-compose.yml](docker-compose.yml) ·
[Dockerfile.dev](Dockerfile.dev) · [.github/workflows/ci.yml](.github/workflows/ci.yml)

### Outcome
`make test` builds and runs the whole suite with no Docker. CI runs the same
build plus ThreadSanitizer and AddressSanitizer jobs on every push.

---

# Step 2 — Database schema: the ledger rules live in Postgres
`[Database]` — ✅ Done

### What was expected to be performed after this stage
- Tables for `currencies`, `accounts`, `transactions`, `entries`.
- The double-entry rules enforced *by the database*, not only by app code.
- Tests that prove invalid data is actually rejected.

### How it was done
The rules are constraints and triggers, because application code can be bypassed
by a stray `psql` session or a data-fixing script:

| Rule | Enforcement |
|---|---|
| A transaction has exactly two entries summing to zero | `DEFERRABLE INITIALLY DEFERRED` constraint trigger, checked at `COMMIT` — [db/migrations/004_entries.sql:129](db/migrations/004_entries.sql#L129) |
| Both legs share a currency | Composite FK to `accounts (id, currency)` — a cross-currency row has no parent row to point at |
| The audit trail is immutable | `BEFORE UPDATE` / `BEFORE DELETE` triggers reject every mutation — [db/migrations/004_entries.sql:159](db/migrations/004_entries.sql#L159) |
| A resent request cannot double-charge | `UNIQUE (idempotency_key)` on `transactions` |
| A user balance cannot go negative | `CHECK (allow_negative OR balance >= 0)` |

The deferred trigger is the interesting one: a `CHECK` sees one row, but "these
two rows sum to zero" spans rows that arrive in separate `INSERT`s. An immediate
check would fire while the transaction is legitimately half-written, so deferring
to commit time is what makes the rule expressible at all — the reasoning is in
the comment at [db/migrations/004_entries.sql:88](db/migrations/004_entries.sql#L88).

📁 [db/migrations/](db/migrations/) ·
[db/checks/constraint_tests.sql](db/checks/constraint_tests.sql) (22 deliberately
invalid writes, all must be rejected) ·
[db/checks/invariants.sql](db/checks/invariants.sql) (is the data that is
*already there* still consistent?) · `make db-all`

### Outcome
The database is the referee. `make db-test` proves it rejects 22 error shapes;
`make db-check` recomputes every balance from its entries (**I2**), which is the
one check that catches a lost update — those rows are individually valid and only
wrong in relation to each other.

---

# Step 3 — In-memory ledger core: money, accounts, ordered locking
`[Core]` `[Concurrency]` — ✅ Done

### What was expected to be performed after this stage
- Model money without floating point, across currencies with different exponents.
- Apply a transfer as an atomic, balanced pair of entries.
- Survive concurrent transfers with no lost updates, no overdraft, no deadlock.

### How it was done
**Money** is `int64_t` minor units plus a per-currency exponent table
([src/money/Currency.cpp:20](src/money/Currency.cpp#L20)). `JPY` has exponent 0
while the rest have 2, so a stored `5000` means `$50.00` in USD but `¥5,000` in
JPY. Every format and parse path consults the table; nothing divides by 100.

**The locking is the design.** A transfer takes two locks, and the order it takes
them in is everything — [src/core/LedgerCore.cpp:61](src/core/LedgerCore.cpp#L61):

```cpp
if (req.from == req.to) return ErrorCode::SelfTransfer;  // before any lock
Account* lo = from; Account* hi = to;
if (lo->id() > hi->id()) std::swap(lo, hi);              // always ascending id
std::unique_lock loGuard(lo->mutex);
std::unique_lock hiGuard(hi->mutex);
```

Locking in the request's own direction deadlocks: `1001→2002` holds 1001 and
waits for 2002 while `2002→1001` does the mirror image. Sorting by id means both
threads take 1001 first, so the loser blocks while **holding nothing** and can
never become anyone's blocker. Along any cycle the ids would have to increase
strictly and still return to the start — a proof, not a heuristic. The
self-transfer guard has to come first, or `lo` and `hi` are the same
non-recursive mutex locked twice and the thread wedges itself.

**One inversion is deliberate.** `auditMutex_` is taken *shared by transfers*
(which write) and *exclusive by audits* (which read) —
[include/ledger/core/LedgerCore.h:73](include/ledger/core/LedgerCore.h#L73). It
does not protect a variable; it protects the property "no transfer is in flight".
Transfers already exclude each other through account locks, so they pay nothing,
while an audit gets a genuinely consistent cross-account snapshot instead of
account A's new balance beside account B's old one.

**Account pointers must stay stable** while the registry grows under traffic —
`unordered_map` is node-based, holds `unique_ptr`, and never erases
([include/ledger/core/AccountRegistry.h:71](include/ledger/core/AccountRegistry.h#L71)).

📁 [include/ledger/core/](include/ledger/core/) · [src/core/](src/core/) ·
[include/ledger/money/](include/ledger/money/) · [src/money/](src/money/)

### Outcome — what the tests actually prove
[tests/test_concurrency.cpp](tests/test_concurrency.cpp) generates real
contention and asserts properties that hold under any interleaving:

| Test | What it would catch |
|---|---|
| `TotalMoneyIsConserved` | 32 threads, 96k transfers, 20 accounts. A lost update shows up as money appearing or vanishing |
| `ConcurrentWithdrawalsCannotOverdraw` | 1000 in the account, everyone withdraws 100 — exactly 10 may succeed |
| `OppositeDirectionTransfersDoNotDeadlock` | 640k opposing transfers; deadlock presents as a hang, caught by the ctest timeout |
| `AuditSeesConsistentSnapshotDuringTraffic` | Audits taken mid-traffic must still total correctly |
| `RegistryGrowthDoesNotInvalidatePointers` | 20k accounts created while transfers run |

Verified against deliberately broken builds, because a test that cannot fail
proves nothing: removing the `std::swap` deadlocks within 30 seconds, and
downgrading the account locks to `shared_lock` loses 8081 units out of 20,000,000.

---

# Step 4 — epoll TCP server, proven with an echo protocol
`[Networking]` `[Linux]` — ✅ Done

### What was expected to be performed after this stage
- A non-blocking, edge-triggered epoll event loop written by hand.
- Correct accept, read, write, and connection-lifetime handling.
- Proven on an echo protocol *before* any ledger semantics are involved.

### How it was done
Edge-triggered epoll reports a fd only when its state *changes*, which makes two
mistakes fatal in a way small tests never reveal:

- **Drain every readable fd until `EAGAIN`** —
  [src/net/Connection.cpp:62](src/net/Connection.cpp#L62), and the same rule for
  `accept4` at [src/net/Acceptor.cpp:40](src/net/Acceptor.cpp#L40). Stop early and
  the remaining bytes sit in the kernel buffer, the state never changes again, and
  epoll never notifies you. That connection goes permanently silent.
- **Handle short writes.** When the send buffer fills, `write()` returns `EAGAIN`;
  the tail has to stay buffered while `EPOLLOUT` is registered — and *deregistered*
  once drained, or a permanently-writable socket spins the loop at 100% CPU
  ([src/net/Connection.cpp:143](src/net/Connection.cpp#L143)).

Writes are funnelled through the loop thread for a different reason: two threads
calling `write()` on one socket interleave their bytes and the length prefix stops
lining up. That is not a data race — `write()` is thread-safe — so ThreadSanitizer
would never flag it. `Connection::send()` is callable from any thread and hands
the real write to the loop via an `eventfd` wakeup
([src/net/EventLoop.cpp:44](src/net/EventLoop.cpp#L44)). The same eventfd is why
`SIGINT` handling is safe: `EventLoop::stop()` does one atomic store and one
`write()`, both async-signal-safe ([src/main.cpp:66](src/main.cpp#L66)).

📁 [include/ledger/net/](include/ledger/net/) · [src/net/](src/net/) ·
[src/net/EventLoop.cpp](src/net/EventLoop.cpp) ·
[src/net/Acceptor.cpp](src/net/Acceptor.cpp) ·
[src/net/Connection.cpp](src/net/Connection.cpp) ·
[src/net/EchoServer.cpp](src/net/EchoServer.cpp)

### Outcome — what these tests prove
[tests/test_echo_server.cpp](tests/test_echo_server.cpp):

| Test | What it would catch |
|---|---|
| `EdgeTriggeredReadDrainsTheEntireSocket` | 1 MB through one read event. A missing drain loop delivers ~131 KB and then hangs |
| `HandlesPartialWritesWhenClientStopsReading` | Client stops reading mid-response. Without `EPOLLOUT` the tail is silently dropped |
| `ServesManyConcurrentClients` | 50 clients × 20 round-trips on one loop thread |
| `SurvivesAbruptDisconnects` | 200 connect-then-immediately-close cycles. Catches fd leaks |
| `ReadsDataSentImmediatelyBeforeClose` | Data sent just before `FIN` must not be dropped — read has to be handled before hangup |
| `RunInLoopHandlesManyCrossThreadTasks` | 2000 tasks from 8 threads, none lost |

Checked against deliberately broken builds:

- Reading once instead of draining: **131,018 of 1,048,576 bytes** arrive, then the
  connection stalls until the test times out.
- Skipping the `EPOLLOUT` registration: **3,994,597 of 4,194,304 bytes** arrive.

The second one is worth a note, because it is a lesson about tests rather than
about epoll. The first version of that test *passed with the bug present*: the
client was still streaming data, and every read event happened to re-drive the
write path. The test was being rescued by traffic it did not control. Adding a
300 ms pause after the client stops sending is what made it actually test the
thing it claimed to test.

ThreadSanitizer caught three real races here that code review had not —
`EventLoop::threadId_`, `Acceptor::accepted_`, and reading `connections_.size()`
off-thread. All three are now atomic.

---

# Step 5 — Wire protocol, thread pool, and the full request path
`[Networking]` `[Concurrency]` `[Protocol]` — ✅ Done

### What was expected to be performed after this stage
- A real message protocol in two encodings: binary and human-readable.
- Work moved off the IO thread onto workers, with backpressure.
- The whole path wired end to end: socket → frame → decode → queue → worker →
  ledger → response.

### How it was done

**One field declaration, two encodings.** The server speaks two protocols: a
length-prefixed binary frame on port 9000, and newline-delimited JSON on port 9001
so that `nc` is a usable debugging client. Supporting both by hand would mean
writing every message field four times — binary encode, binary decode, JSON
encode, JSON decode. Forgetting one of the four is a silent failure: it compiles,
that encoding's tests pass, and only the *other* encoding is quietly missing a
field.

Instead each message declares its fields exactly once
([include/ledger/proto/Messages.h:126](include/ledger/proto/Messages.h#L126)):

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

Both codecs walk that tuple and dispatch on the field type, so each one implements
five primitives rather than one function per message. A field type with no
dispatch arm is a `static_assert`, not a silently skipped field.

The trade-off is real and worth naming: reordering `fields()` silently changes the
binary layout while leaving JSON untouched, and round-trip tests cannot see it —
encoder and decoder change together and stay consistent with each other. That is
what the byte-level golden table in [tests/test_codec.cpp](tests/test_codec.cpp)
is for; it doubles as the protocol specification.

`Currency` goes on the wire as three ASCII bytes (`"USD"`) rather than a one-byte
enum, for the same class of reason: the enum's ordering is tied to
[db/migrations/001_currencies.sql](db/migrations/001_currencies.sql), so
reordering that SQL file would silently change the wire format. Three letters are
self-describing, visible in a hexdump, and match how the database spells it. Two
extra bytes to delete a whole category of silent breakage is a good trade
([include/ledger/proto/BinaryCodec.h](include/ledger/proto/BinaryCodec.h)).

**Every integer crosses the JSON boundary as a string**, and bare numbers are
rejected outright ([src/proto/JsonCodec.cpp:45](src/proto/JsonCodec.cpp#L45)).
JavaScript numbers are IEEE-754 doubles, so `JSON.parse('{"amount":9007199254740993}')`
silently returns …992 — no error, one cent gone. **This directly constrains Step 12:
the web console must send and receive amounts as strings.**

**Two kinds of protocol failure, two different responses**
([src/proto/Session.cpp](src/proto/Session.cpp)): a *decode* error means the frame
boundary is known but the contents are not — reply with an error and keep the
connection. A *framing* error means the stream can no longer be aligned — reply,
then close, because continuing to read only produces more noise.

**The queue is bounded on purpose**
([include/ledger/concurrent/BlockingQueue.h:92](include/ledger/concurrent/BlockingQueue.h#L92)).
An unbounded queue answers a slow database by growing without limit and by
queueing requests whose clients timed out long ago. The IO thread therefore only
ever calls `tryPush`, and turns a refusal into `SERVER_BUSY`
([src/net/LedgerServer.cpp:64](src/net/LedgerServer.cpp#L64)) — an event loop that
blocks stalls *every* connection, not just the unlucky one.

Workers are `std::jthread` blocked on `condition_variable_any::wait(lock, stop_token, pred)`
([src/concurrent/ThreadPool.cpp:16](src/concurrent/ThreadPool.cpp#L16)), so
shutdown has two distinct meanings: `close()` refuses new work but finishes what
is queued (those clients are still waiting), while `stop_token` abandons the queue
immediately.

**Handlers are an abstract interface owned by `unique_ptr`, not `std::function`.**
This looks like over-engineering until Step 6: those handlers will hold a
`pqxx::connection`, which is move-only, and `std::function` requires its target to
be copyable. The factory runs *on* the worker thread, so each database connection
is created, used, and destroyed on one thread and needs no lock at all.

**Connection lifetime is weak in both directions**
([include/ledger/net/LedgerServer.h:45](include/ledger/net/LedgerServer.h#L45)):

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
never reaches zero and every closed connection leaks its fd and its buffers. That
failure never crashes — the process just grows, and only a long load test notices.
In the other direction each queued `Task` holds its context weakly, so a client
that disconnects while its transfer is still queued gets its result discarded
rather than written to a dead socket.

**None of `net/` changed in this stage.** The event loop, the acceptor, the
connection, and the buffer are byte-for-byte what Step 4 delivered — which was the
entire point of building the echo server without a protocol first.

📁 [include/ledger/proto/](include/ledger/proto/) · [src/proto/](src/proto/) ·
[include/ledger/concurrent/](include/ledger/concurrent/) ·
[src/concurrent/](src/concurrent/) ·
[src/core/LedgerRequestHandler.cpp:27](src/core/LedgerRequestHandler.cpp#L27)
(request dispatch via `std::visit`, so a new message type is a compile error until
handled) · [src/net/LedgerServer.cpp](src/net/LedgerServer.cpp)

### Outcome — the engine is a live server
Port 9000 speaks length-prefixed binary; port 9001 speaks NDJSON so that `nc` is
a working client:

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
```

Demo accounts are created by transferring out of a system account rather than by
setting balances directly, so I2 holds from the very first row
([src/core/LedgerRequestHandler.cpp:101](src/core/LedgerRequestHandler.cpp#L101)).
Ctrl-C prints what the run did and re-checks the invariants
([src/main.cpp:149](src/main.cpp#L149)).

#### What the protocol and pool tests prove (5a / 5b)

| Test | What it would catch |
|---|---|
| `CodecGolden.*` | Byte-level layout of every message. Reordering `fields()` breaks binary compatibility silently; round-trip tests cannot see it |
| `CodecInt64.ValuesBeyondDoublePrecisionSurvive` | 2⁵³+1 through both codecs |
| `CodecInt64.JsonRejectsBareNumbers` | An unquoted integer is an error, not a lucky small value |
| `FrameSplitter*.OversizedLengthIsRejectedImmediately` | A client sending `len = 0xFFFFFFFF` and then going quiet must not make us wait forever |
| `BlockingQueue.TryPushNeverBlocksWhenFull` | 10,000 rejected pushes must take microseconds, not block |
| `BlockingQueue.CloseWakesEveryBlockedConsumer` | `notify_one` instead of `notify_all` leaves threads asleep and `join()` never returns |
| `ThreadPool.DiscardsResultWhenSinkIsGone` | A client that disconnects while its request is queued |
| `ThreadPool.EachWorkerGetsItsOwnHandler` | The precondition for one database connection per worker in Step 6 |

Checked against deliberately broken builds:

- `notify_all` → `notify_one` in `close()`: the process **hangs forever with no
  output at all** and is killed by the ctest timeout.
- `tryPush` → blocking `push` in `submit()`: 200 submissions take **9,780 ms**
  instead of under 1 ms.

The first is why `gtest_discover_tests` sets a timeout. A missed notification and
a lock-order deadlock both manifest as *never returning*, not as a failed
assertion — the assertion never gets a chance to run, so the timeout is the only
thing that can judge them.

#### What the end-to-end tests prove (5c)

Eighteen tests in [tests/test_ledger_server.cpp](tests/test_ledger_server.cpp)
drive real sockets against a real server with real worker threads:

| Test | What it would catch |
|---|---|
| `ConcurrentClientsPreserveTotalMoney` | Eight clients, half going 1001→2002 and half 2002→1001 — exactly the pattern that deadlocks a naive lock-the-source-first implementation. Exercises the event loop, both codecs, the queue, the ordered locking, and response routing at once, then asserts the two balances still sum to what they did before |
| `SurvivesClientsDisconnectingMidFlight` | Fifty requests fired, then the client hangs up without reading any of them |
| `FramingErrorClosesTheConnection` | `len = 0xFFFFFFFF` must produce a reply and then a FIN |
| `GarbageLineGetsAnErrorButKeepsTheConnection` | Nonsense followed by a valid ping on the same socket |

Every fixture teardown re-checks the invariants.

#### Suite totals

151 test cases across ten files. The 28 socket tests
([test_echo_server.cpp](tests/test_echo_server.cpp),
[test_ledger_server.cpp](tests/test_ledger_server.cpp)) build on Linux only; CI
runs all of them plus the TSan and ASan matrix on every push
([.github/workflows/ci.yml](.github/workflows/ci.yml)).

---

# Step 6 — PostgreSQL durability: `SELECT … FOR UPDATE` and real commits
`[Database]` `[Persistence]` — ⬜ Not started

### What is expected to be performed after this stage
- Each worker owns one `pqxx::connection`; transfers run inside a DB transaction.
- Rows are locked with `SELECT … FOR UPDATE` **in the same ascending-id order** as
  the in-memory locks — a second lock ordering is a second chance to deadlock.
- Idempotency is enforced by the DB unique index, not only by the in-memory cache.
- A client hears "ok" only after `COMMIT` returns.

### How it will be done
The insertion points already exist and are marked in the code. The handler factory
runs *on* the worker thread ([src/core/LedgerRequestHandler.cpp:95](src/core/LedgerRequestHandler.cpp#L95)),
so each connection is created, used, and destroyed on one thread and needs no lock
— which is also why handlers are an abstract interface owned by `unique_ptr` and
not `std::function` (a `pqxx::connection` is move-only, and `std::function`
requires a copyable target). `seedDemoAccounts()` gets replaced by
`SELECT id, balance FROM accounts` ([src/main.cpp:4](src/main.cpp#L4)), and the
same `nc` commands must produce the same answers.

📁 Insertion points: [src/core/LedgerCore.cpp](src/core/LedgerCore.cpp) ·
[src/core/LedgerRequestHandler.cpp](src/core/LedgerRequestHandler.cpp) ·
[src/main.cpp](src/main.cpp) · [db/migrations/](db/migrations/) ·
DSN already wired at [docker-compose.yml](docker-compose.yml) (`LEDGER_PG_DSN`)

### Expected outcome
The ledger survives a process restart. Invariants I1–I5 hold against durable rows,
not just against memory.

---

# Step 7 — Sanitizer and stress verification of the DB-backed path
`[Concurrency]` `[Verification]` — ⬜ Not started

### What is expected to be performed after this stage
- ThreadSanitizer and AddressSanitizer clean on the Postgres-backed build.
- The Step 3 concurrency suite re-run against real rows, including a forced
  Postgres deadlock (`deadlock_timeout` is already turned down to 1s in
  [docker-compose.yml](docker-compose.yml)) to prove retry behaviour.
- Invariants re-checked after crash/restart, not just after clean shutdown.

### How it will be done
The sanitizer targets already exist (`make tsan`, `make asan`, and both CI
matrix jobs in [.github/workflows/ci.yml](.github/workflows/ci.yml)); this stage
extends their coverage over the new code path rather than building new machinery.
The method stays the one used in Steps 3–5: break the code deliberately and
confirm each test fails.

### Expected outcome
The same correctness claims that hold in memory today, re-established against a
real database.

---

# Step 8 — Load tests, tuning, TPS and p95
`[Performance]` — ⬜ Not started

### What is expected to be performed after this stage
- Locust load tests driving the binary port.
- Measured throughput and p95 latency; queue depth and rejection rate under load.
- Worker count and queue bound tuned against measurements, not guesses.

### How it will be done
[bench/](bench/) is the reserved home. The message codes are already frozen
([include/ledger/proto/Messages.h:52](include/ledger/proto/Messages.h#L52))
precisely so a load script can depend on them. The counters this stage needs —
`submitted`, `completed`, `rejected`, `droppedNoSink` — already exist on
[ThreadPool](include/ledger/concurrent/ThreadPool.h#L147), which is also what
makes Step 10 cheap.

### Expected outcome
Real numbers, and a defensible answer to "how many workers, and why that many".

---

# Step 9 — Final architecture documentation and diagrams
`[Documentation]` — ⬜ Not started

### What is expected to be performed after this stage
- Architecture diagrams for the thread model, the request path, and lock ordering.
- Trade-offs and the correctness arguments consolidated in English.
- The Chinese design doc reconciled with what was actually built.

📁 [docs/](docs/) · [README.md](README.md)

---

# Step 10 — Observability surface: the engine reports on itself
`[Backend]` `[Protocol]` `[Visibility]` — ⬜ **Next**

### What is expected to be performed after this stage
- A `stats` request/response pair so any client can read live engine state.
- An event stream (`subscribe`) so a client is *pushed* transfer and connection
  events instead of polling.
- The numbers reported must be the engine's real counters — a dashboard that
  displays invented data is worse than no dashboard.

### How it will be done
Most of the data already exists; it is just trapped inside the shutdown printout
at [src/main.cpp:149](src/main.cpp#L149). This step exposes it over the wire:

| What the console shows | Where the number already comes from |
|---|---|
| Requests submitted / completed / rejected / dropped, per pool | [ThreadPool](include/ledger/concurrent/ThreadPool.h#L147) — `submitted()`, `completed()`, `rejected()`, `droppedNoSink()` |
| Live queue depth (backpressure, visible) | [BlockingQueue](include/ledger/concurrent/BlockingQueue.h) |
| Active and cumulative connections | [LedgerServer::activeCount_](include/ledger/net/LedgerServer.h#L178), `totalConnections()` |
| Transfers committed / rejected | `LedgerCore::transferCount()`, `rejectedCount()` |
| "The ledger balances" | `LedgerCore::verifyInvariants()` |
| All account balances at once, consistently | `LedgerCore::audit()` ([LedgerCore.h:102](include/ledger/core/LedgerCore.h#L102)) — already takes `auditMutex_` exclusively, so the snapshot is coherent |

Work to do:
1. Add `Stats`/`StatsResp` (and `Subscribe`/`Event`) to
   [Messages.h](include/ledger/proto/Messages.h) — **append** new `MsgType`
   values; the existing numbers are frozen and never reused. Declaring `fields()`
   once means both codecs pick them up for free.
2. Handle them in [LedgerRequestHandler](src/core/LedgerRequestHandler.cpp#L27) —
   `std::visit` will not compile until every new type has an arm.
3. Byte-level golden entries in [tests/test_codec.cpp](tests/test_codec.cpp), the
   same as every other message.
4. For the push stream, keep the existing rule: **only the loop thread writes to
   sockets**. Events fan out via `Connection::send()` / `runInLoop`, never from a
   worker directly.
5. Counters read for display must be `memory_order_relaxed` atomics — a dashboard
   read must not perturb the thing it is measuring.

### Expected outcome
`{"id":"1","type":"stats"}` over `nc` returns the live state of the engine. The
frontend then has something true to render, and Step 8's load tests get their
metrics for free.

---

# Step 11 — Gateway: a WebSocket/HTTP bridge to the browser
`[Frontend Infrastructure]` `[Networking]` — ⬜ Next

### What is expected to be performed after this stage
- A small bridge process that speaks the NDJSON protocol on one side and
  WebSocket/HTTP on the other, so a browser can reach the engine.
- The engine's protocol stays unchanged — no HTTP parsing inside the C++ server.
- The JSON port reachable from the host when running under Docker.

### How it will be done
A browser cannot open a raw TCP socket, so something has to translate. The bridge
is deliberately *outside* the C++ engine: putting an HTTP/WebSocket stack into the
event loop would mean a second protocol, a second framing layer, and a second set
of edge-triggered bugs in the code whose correctness the whole project rests on.
The bridge is a thin, replaceable process — if it dies, the engine does not care.

Plan:
1. Add `gateway/` — a small Node (or Python) process holding one TCP connection
   per browser session to port 9001, relaying lines both ways verbatim.
2. Serve the Step 12 static files from the same process, so the console is one
   `make ui` away.
3. **Publish port 9001**: [docker-compose.yml](docker-compose.yml) currently maps
   only `9000:9000`, so the NDJSON port the console needs is not reachable from
   the host. Either add `9001:9001` or run the gateway inside the compose network.
4. The gateway relays; it does not interpret. Amounts stay strings end to end
   (see Step 5) — the moment the gateway does `JSON.parse` into a number and back,
   the int64 guarantee is gone.
5. Surface backpressure honestly: a `SERVER_BUSY` reply is shown as
   `SERVER_BUSY`, never retried silently. Watching the queue refuse work is one of
   the more interesting things the console can show.

### Expected outcome
`http://localhost:8080` reaches the engine, with a WebSocket carrying the exact
same NDJSON messages that `nc` sends today.

---

# Step 12 — Web console: see the ledger and the concurrency work
`[Frontend]` `[Visibility]` — ⬜ Next

### What is expected to be performed after this stage
A single page that makes the backend observable, with four panels:

1. **Accounts** — every account, balance formatted by its currency exponent
   (¥5,000 vs $50.00), live-updated after each transfer.
2. **Transfer** — a form that sends a real `transfer` message and displays the raw
   request and raw response side by side. The wire format is part of the demo, not
   hidden by it.
3. **Live journal** — transfers streaming in as they commit, each showing its two
   balanced entries. This is where double-entry stops being a claim in a README.
4. **Engine vitals** — queue depth, active workers, connections, transfers
   committed vs rejected, and the invariant check, all from Step 10's `stats`.

Plus the panel the project actually exists for:

5. **Concurrency demo** — fire N simultaneous opposing transfers (1001→2002 and
   2002→1001, the pattern that deadlocks a naive implementation), then show total
   money before and after. It does not move. Also: an overdraft race where exactly
   10 of 50 withdrawals succeed, and a button that fills the queue so
   `SERVER_BUSY` appears on screen.

### How it will be done
- Static HTML/CSS/JS served by the Step 11 gateway. No build step and no framework
  unless one earns its place — the interesting engineering is behind the socket,
  and a toolchain here is a maintenance cost that adds nothing to the demo.
- **Amounts are strings, always.** Never `JSON.parse` an amount into a JS number;
  format for display with `BigInt` and the currency exponent from
  [src/money/Currency.cpp:20](src/money/Currency.cpp#L20). This is the same trap
  the codec already refuses to fall into at
  [JsonCodec.cpp:45](src/proto/JsonCodec.cpp#L45) — the frontend must not reopen it.
- A visible pipeline diagram (`socket → frame → queue → worker → ledger → response`)
  that lights up as messages move, so the architecture in the README becomes a
  thing you watch rather than a thing you read.
- The console is a *client*, with no privileged access. Everything on screen is
  something a `nc` session could have obtained, which keeps it honest.

📁 Planned: `ui/` (static console) · `gateway/` (from Step 11) ·
`make ui` target in [Makefile](Makefile)

### Expected outcome
Open a browser, watch balances move, watch the queue fill and refuse, watch 640k
opposing transfers conserve money exactly. Every claim in the README becomes
demonstrable in ten seconds instead of requiring a compiler.

---

## Appendix A — The invariants

These five properties are what "the ledger is correct" actually means. They are
asserted by the test suite and by `make db-check`, not merely described.

| | Invariant | Where it is checked |
|---|---|---|
| **I1** | Every transaction's entries sum to zero | Deferred constraint trigger at [db/migrations/004_entries.sql:129](db/migrations/004_entries.sql#L129); [db/checks/invariants.sql](db/checks/invariants.sql) |
| **I2** | Every account balance equals the sum of its entries | `LedgerCore::verifyInvariants()`; `make db-check`. **The one that catches a lost update** — those rows are individually valid and only wrong in relation to each other |
| **I3** | Total money per currency is conserved across 32 threads and ~100k concurrent transfers | `TotalMoneyIsConserved` in [tests/test_concurrency.cpp](tests/test_concurrency.cpp) |
| **I4** | No user account balance goes negative under concurrent load | `ConcurrentWithdrawalsCannotOverdraw`; `CHECK (allow_negative OR balance >= 0)` |
| **I5** | The same idempotency key always yields the same transaction, applied once | In-memory shard cache; `UNIQUE (idempotency_key)` in the schema |

I3 is the load-bearing one. It is the assertion a race condition cannot hide
behind: every other check can be satisfied by a system that quietly loses money in
one place and invents it in another, but conservation cannot.

---

## Appendix B — How correctness was verified

Three methods, used at every step.

### 1. Deliberately broken builds

A test that cannot fail proves nothing. Every correctness claim in this document
was confirmed by breaking the code and watching the test catch it:

| Step | Break | Result |
|---|---|---|
| 3 | Remove the `std::swap` that orders lock acquisition | Deadlocks within 30 seconds |
| 3 | Downgrade account locks to `shared_lock` | **8,081 units lost out of 20,000,000**; trips both conservation and the I2 recompute |
| 4 | Read once instead of draining to `EAGAIN` | 131,018 of 1,048,576 bytes arrive, then the connection stalls |
| 4 | Skip the `EPOLLOUT` registration | 3,994,597 of 4,194,304 bytes arrive |
| 5 | `notify_all` → `notify_one` in `close()` | Process hangs forever with no output; killed by the ctest timeout |
| 5 | `tryPush` → blocking `push` in `submit()` | 200 submissions take 9,780 ms instead of under 1 ms |

Note what the failure *shapes* are. Two of the six do not fail an assertion at
all — they never return. A missed notification and a lock-order deadlock both
present as a hang, so the ctest timeout is the only thing that can judge them.
That is why `gtest_discover_tests` sets one.

### 2. Sanitizers

`make tsan` and `make asan` build with ThreadSanitizer and
AddressSanitizer/UBSan; both run as CI matrix jobs
([.github/workflows/ci.yml](.github/workflows/ci.yml)). Both are clean.

ThreadSanitizer found three real races in Step 4 that reading the code had not:
`EventLoop::threadId_`, `Acceptor::accepted_`, and reading `connections_.size()`
from outside the loop thread. All three are now atomic.

It is equally important to know what the sanitizers *cannot* see. Two threads
calling `write()` on the same socket interleave their bytes and desynchronise the
protocol — but `write()` is thread-safe, so this is not a data race and
ThreadSanitizer will never flag it. That bug is prevented structurally instead, by
funnelling every socket write through the loop thread.

### 3. Building on two platforms

Everything except `net/` builds and runs natively on macOS; `net/` needs `epoll`,
`eventfd`, and `accept4`, so it compiles only on Linux and the server runs in the
container. Keeping the second platform alive costs a little and has already paid
for itself: `main.cpp` parsed `--port` and then only read it inside the
`LEDGER_HAS_EPOLL` branch, so on macOS the variable was written and never read.
AppleClang's `-Wunused-but-set-variable` plus `-Werror` rejected the build. The
Linux CI could never have seen it — on Linux the variable *is* read.

Two compilers disagreeing is information. A warning that only one of them raises
is usually pointing at a code path the other never compiles.

---

## Appendix C — Decisions worth remembering

Short list of the choices that shaped everything downstream, and the reason each
one is not arbitrary.

| Decision | Why |
|---|---|
| Balances are derived, entries are the truth | Makes a lost update *detectable* (I2) instead of silent |
| Lock accounts in ascending id, never in transfer direction | Makes the wait-for graph provably acyclic; the loser blocks while holding nothing |
| Self-transfer check happens *before* any lock | Otherwise the same non-recursive mutex is locked twice and the thread wedges itself |
| `auditMutex_`: shared by writers, exclusive by readers | It protects a *property* ("no transfer in flight"), not a variable |
| `int64_t` minor units + per-currency exponent | ¥5,000 and $50.00 are the same stored number; nothing may divide by 100 |
| Integers cross JSON as strings | JS doubles silently truncate above 2⁵³ — one cent, no error |
| Currency on the wire as `"USD"`, not an enum value | The enum's order is tied to a SQL file; three self-describing bytes remove the coupling |
| Message type codes are frozen and append-only | Clients and load scripts depend on them |
| Bounded queue, `tryPush` only, `SERVER_BUSY` on refusal | An unbounded queue answers a slow database by growing forever; a blocking one stalls every connection |
| `close()` and `stop_token` are different shutdowns | "Finish what is queued" and "abandon it" are both needed, and they are not the same request |
| Handlers as `unique_ptr<Interface>`, not `std::function` | `pqxx::connection` is move-only; Step 6 would otherwise not compile |
| Weak pointers in both lifetime directions | A cycle leaks every closed connection — a failure that never crashes, it just grows |
| Demo data injected by transfer, never by setting balances | Otherwise I2 is false from the first row and the main referee is broken from the start |
| Framing errors close the connection; decode errors do not | Once the stream cannot be realigned, reading on only produces more noise |

---

## Appendix D — Protocol reference

The complete wire specification. The README explains the same messages in plain
language for a non-technical reader; this is the version you implement against.

### Envelope

Both encodings carry the same three header fields.

| Field | JSON key | Binary | Notes |
|---|---|---|---|
| Message type | `type` (string name) | `u16` code | See the table below |
| Protocol version | `v` (string int) | `u16` | Always `1`. **Optional on JSON requests**, defaults to 1 — forcing a human to type `"v":1` at `nc` would kill the point of the JSON port. Always present on responses |
| Request id | `id` (string int) | `u32` | Echoed on the response so a client can match them. A `u32` fits in a double without loss, but it is still sent as a string for consistency |

### Message types

Codes are **frozen**: new messages are appended, old values never reused. Clients
and the Step 8 load script depend on them
([Messages.h:52](include/ledger/proto/Messages.h#L52)).

| Request | Code | Response | Code |
|---|---|---|---|
| `ping` | `0x0001` | `pong` | `0x8001` |
| `transfer` | `0x0002` | `transfer_ok` | `0x8002` |
| `get_account` | `0x0003` | `account` | `0x8003` |
| | | `error` | `0x80FF` |

The high bit means "this is a response". A response type arriving as a request is
`UNKNOWN_MESSAGE_TYPE`, not a silently accepted message.

### Message fields

Field order below is `fields()` order, which **is** the binary payload order
([Messages.h](include/ledger/proto/Messages.h)).

| Message | Fields |
|---|---|
| `ping` | *(none)* |
| `pong` | *(none)* |
| `transfer` | `idem_key` (string, ≤128) · `from` (i64) · `to` (i64) · `amount` (i64, > 0) · `ccy` (currency) |
| `transfer_ok` | `tx_id` (i64) · `from_balance` (i64) · `to_balance` (i64) |
| `get_account` | `account_id` (i64) |
| `account` | `id` (i64) · `balance` (i64) · `ccy` (currency) · `status` (`ACTIVE` / `CLOSED`) |
| `error` | `code` (ErrorCode) · `message` (string) |

Both legs of a transfer must already be in `ccy`; there is no FX in v1.

### JSON encoding rules

- One object per line, newline-delimited (NDJSON). One line in, one line out.
- **Every integer field is a string.** Bare numbers are rejected with
  `INTEGER_NOT_STRING` ([JsonCodec.cpp:45](src/proto/JsonCodec.cpp#L45)) rather
  than accepted while the values happen to be under 2⁵³. Protobuf's canonical JSON
  mapping made the same call for the same reason.
- `ccy` is the three-letter code; `status` is `ACTIVE` or `CLOSED`; `code` is the
  SCREAMING_SNAKE name from [Currency.cpp:90](src/money/Currency.cpp#L90).

### Binary encoding rules

```
+--------+---------+--------+----------+------------------+
| len:u32| type:u16| ver:u16| reqId:u32|   payload ...    |
+--------+---------+--------+----------+------------------+
 ^ excludes itself
          \------- 8-byte header -------/
          \------------- len covers this ----------------/
```

Big-endian throughout. `LengthPrefixSplitter` owns the first four bytes; the codec
sees everything after them ([BinaryCodec.h](include/ledger/proto/BinaryCodec.h)).

| Field type | Encoding |
|---|---|
| `int64_t` | 8 bytes big-endian, two's complement |
| `std::string` | `[u16 length][bytes]`, no NUL terminator |
| `Currency` | three ASCII bytes (`"USD"`) — *not* the enum value |
| `ErrorCode` | `u16`, the enum's numeric value |
| `AccountStatus` | `u8` |

Max frame: **64 KB** (`kMaxFrameSize`). NDJSON needs that bound more than binary
does — with no length prefix, a client that never sends a newline can grow the
buffer without limit.

The exact bytes of every message are pinned by the golden table in
[tests/test_codec.cpp](tests/test_codec.cpp), which is the real specification: a
round-trip test cannot detect a `fields()` reordering, because encoder and decoder
change together and stay consistent with each other.

### Error codes

Values are serialized as `u16` on the binary port, so **new codes are appended to
the end of the enum only** — inserting one shifts every existing client's codes
([Result.h:13](include/ledger/common/Result.h#L13)).

**Request rejected, connection stays open:**

| Code | Meaning |
|---|---|
| `INVALID_AMOUNT` | Amount ≤ 0 |
| `SELF_TRANSFER` | `from` == `to`. Checked *before* any lock is taken — see Step 3 |
| `UNKNOWN_CURRENCY` | Currency code not in the table |
| `ACCOUNT_NOT_FOUND` | No such account |
| `ACCOUNT_CLOSED` | Account exists but is closed |
| `CURRENCY_MISMATCH` | The two legs differ in currency |
| `INSUFFICIENT_FUNDS` | The double-spend guard |
| `AMOUNT_OVERFLOW` | The arithmetic would overflow `int64` |
| `DUPLICATE_ACCOUNT` | That owner already has an account in that currency |
| `MALFORMED_FRAME` | Bytes do not form a valid message (short field, bad JSON) |
| `UNKNOWN_MESSAGE_TYPE` | Unrecognised type, or wrong direction |
| `INTEGER_NOT_STRING` | Integer field sent as a bare JSON number |
| `MISSING_FIELD` | Required key absent |
| `SERVER_BUSY` | Queue full; backpressure is working as designed |

**Stream unrecoverable — reply, then close:**

| Code | Meaning |
|---|---|
| `UNSUPPORTED_VERSION` | `v` != `kProtocolVersion` |
| `FRAME_TOO_LARGE` | Message exceeded `kMaxFrameSize` — the length prefix is nonsense, or a line never ended |

The split is the Step 5 rule: a decode error means the frame boundary is known but
the contents are not, so skip the message and keep the connection. A framing error
means the stream can no longer be aligned, and continuing to read only produces
more noise.

Internal codes (`SOCKET_ERROR`, `BIND_FAILED`, `LISTEN_FAILED`, `EPOLL_ERROR`,
`CONNECTION_CLOSED`) exist for server-side plumbing and are never sent to a client.

### Demo dataset

[`seedDemoAccounts()`](src/core/LedgerRequestHandler.cpp#L101) — matches
[db/seeds/dev_seed.sql](db/seeds/dev_seed.sql).

| Id | Owner | Ccy | Balance | Negative allowed | |
|---|---|---|---|---|---|
| `9001` | 99 | USD | `0` → `-162000` | ✅ | source of all USD |
| `9002` | 99 | JPY | `0` → `-5000` | ✅ | source of all JPY |
| `1001` | 1 | USD | `115000` | ❌ | Alice, $1,150.00 |
| `2002` | 2 | USD | `47000` | ❌ | Bob, $470.00 |
| `1003` | 1 | JPY | `5000` | ❌ | Alice, ¥5,000 — exponent 0 |

Balances are injected by four seed transfers, not by writing `balance` directly, so
I2 holds from the first row and `verifyInvariants()` is a working referee from
startup. `dev_seed.sql` uses `dev_transfer()` rather than `UPDATE` for the same
reason.

The final seed transfer is the worked example from §4 of the design document —
Alice sends $50.00 to Bob — which is why 1001 lands on `115000` and 2002 on
`47000`, matching the Stage 2 SQL seed exactly.

---

## Where we are right now

**Done: Steps 0 through 5.** The engine is a live, working TCP server. Two ports,
24 worker threads, an in-memory ledger, 151 test cases, and a CI pipeline that
runs them alongside ThreadSanitizer and AddressSanitizer builds on every push.
`nc localhost 9001` is a functioning client today.

**Not started: Steps 6 through 12.** The backend gap is persistence — nothing
survives a restart, because `seedDemoAccounts()` is still standing in for
`SELECT id, balance FROM accounts`. The presentation gap is the one being closed
first.

**Immediately next — the visibility track, in this order:**

1. **Step 10** — add `stats` and an event stream to the protocol. Small, mostly
   plumbing counters that already exist out through the wire. Do this first: the
   frontend is only worth building on top of numbers that are real.
2. **Step 11** — the gateway process, plus publishing port 9001 in
   [docker-compose.yml](docker-compose.yml).
3. **Step 12** — the web console itself.

**Then the backend track resumes at Step 6** (PostgreSQL), with the console
already running — so swapping memory for durable storage is a change that can be
watched happening, and the invariant panel is the thing that says whether it
worked.

### What is runnable today

```bash
make test          # the full suite, no Docker needed
make db-all        # reset schema, seed, 22 constraint tests, invariant check
make up            # Postgres 16 + Linux build container
docker compose exec engine bash -c './build/src/ledger_engine'
nc localhost 9001  # then paste a JSON request; Ctrl-C prints run stats
```

Two caveats worth knowing before you try it:

- The server only builds and runs on Linux (epoll), so it needs the container.
  Everything else — ledger core, money, protocol, thread pool, and their sanitizer
  builds — runs natively on macOS.
- [docker-compose.yml](docker-compose.yml) publishes port 9000 but **not 9001**,
  so `nc localhost 9001` works from inside the container, not from the host. Step
  11 fixes that because the console needs it.

Full setup instructions, the protocol reference, and every make target are in
**[README.md](README.md)**.
