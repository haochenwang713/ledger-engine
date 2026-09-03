# Transactional Ledger & Settlement Engine

**Plain English:** this is the piece of software that sits underneath a bank or a
payment app and actually moves the money. When you tap "send $50 to Bob", something
has to subtract $50 from you, add $50 to Bob, write both facts down permanently,
and make absolutely sure that nothing goes wrong if ten thousand other people are
doing the same thing at the same instant.

That "something" is called a **ledger engine**. This project is one, built from
scratch in a programming language called C++.

> **New to this kind of software?** You are in the right place. This page explains
> everything from the beginning and assumes no background. The deep technical
> record — how it was built, why each decision was made, what was tested — lives in
> a separate file, **[progress.md](progress.md)**.

---

## Contents

- [The problem this solves](#the-problem-this-solves)
- [What this software promises](#what-this-software-promises)
- [A small glossary](#a-small-glossary)
- [How it works, in plain terms](#how-it-works-in-plain-terms)
- [Trying it yourself](#trying-it-yourself)
- [Talking to the engine](#talking-to-the-engine)
- [How money is written down](#how-money-is-written-down)
- [How we know it is correct](#how-we-know-it-is-correct)
- [Where the project is now](#where-the-project-is-now)

---

## The problem this solves

Imagine two things happen at the exact same moment:

- Alice sends $50 to Bob.
- Bob sends $30 to Alice.

Each one is simple on its own. But a computer does not do one thing at a time —
it does thousands of things at once, in overlapping slices. So both of these
transfers are half-finished simultaneously, both are reading and changing the same
two account balances, and they can trip over each other.

Two specific disasters can happen, and both are silent:

**1. Money disappears (or appears from nowhere.)**

Suppose Alice's balance is $100. Two transfers each try to take $50.

```
Transfer A reads the balance:  $100
Transfer B reads the balance:  $100     ← it read before A finished writing
Transfer A writes:             $50
Transfer B writes:             $50      ← should have been $0
```

Alice spent $100 but her account says $50. Fifty dollars was created out of thin
air. Nothing crashed. No error message appeared. The books are simply wrong now,
and they will stay wrong forever. In the industry this is called a **lost update**,
and it is the reason software like this is written so carefully.

**2. Everything freezes.**

To change an account safely, a program "locks" it — the software equivalent of
putting a *do not disturb* sign on a file so nobody else touches it mid-edit.

Now: Alice→Bob locks Alice's account and reaches for Bob's. At the same instant,
Bob→Alice locks Bob's account and reaches for Alice's. Each is holding what the
other needs. Neither will ever let go. Both wait forever, and so does everyone
queued behind them.

That is called a **deadlock**, and it does not look like a crash. It looks like the
app just… stops responding, for everyone, with no explanation.

**This project's entire purpose is to make both of these impossible** — not
"unlikely", not "we tested it and it seemed fine", but structurally impossible.

---

## What this software promises

Nine promises, in plain language.

**1. Every transfer is written down twice, and the two halves must cancel out.**

This is a 700-year-old accounting practice called **double-entry bookkeeping**.
Money is never simply "removed" — it is moved, so every movement is recorded as a
matched pair: *−$50 from Alice* and *+$50 to Bob*. The pair must add up to exactly
zero.

Why this matters: if money ever goes missing, the two halves stop cancelling out,
and the mismatch is immediately visible. Without it, missing money leaves no
trace at all.

**2. Balances can always be rebuilt from scratch.**

Your account balance is not treated as the truth. The permanent list of every
movement ever made is the truth, and the balance is just a running total kept for
speed. At any moment the engine can re-add the entire history and check that the
totals still match. If they ever disagree, something is wrong and we find out —
rather than never knowing.

**3. Money is never lost or created, no matter how busy things get.**

This is tested directly: 32 simultaneous streams of activity, roughly 96,000
transfers among 20 accounts, all at once. Afterwards, the total amount of money in
the system must be **exactly** what it was before. Not approximately. Exactly.

**4. You cannot spend money you do not have.**

Even when 50 withdrawal requests hit the same account in the same instant. If the
account holds $1,000 and everyone tries to take $100, exactly ten succeed and the
rest are refused. Eleven successes would mean the system invented $100.

**5. It cannot freeze.**

The deadlock described above is prevented by a rule rather than by luck: every
transfer always locks the lower-numbered account first, regardless of which
direction the money is flowing. Because everyone follows the same order, nobody can
ever be stuck waiting on someone who is waiting on them. This is proven with
mathematics, not just tested.

**6. Money is never stored as a decimal number.**

Computers are famously bad at decimals — in most programming languages,
`0.1 + 0.2` does not equal `0.3`. It equals `0.30000000000000004`. Tiny errors,
repeated across millions of transactions, become real missing money.

So this engine never stores `$50.00`. It stores `5000` — the number of *cents* —
as a whole number. Whole numbers are exact. Formatting for display happens only at
the very last moment.

**7. Clicking "send" twice does not send twice.**

Every request carries a unique ticket number. If your phone loses signal and
retries, the engine recognises the ticket it has already processed and returns the
original result instead of moving the money again.

**8. The database enforces the rules too.**

The rules are not only written into the program — they are also built into the
storage system itself, as hard constraints. This matters because programs can be
bypassed: someone might edit the data directly, or write a quick script to "fix"
something. The storage layer refuses to accept broken data no matter who is asking.

**9. Different currencies are handled honestly.**

`5000` means **$50.00** in US dollars but **¥5,000** in Japanese yen — because the
yen has no subdivision like cents. The engine knows this per currency and never
assumes "divide by 100".

---

## A small glossary

Every technical word used on this page, defined once.

| Word | What it means here |
|---|---|
| **Ledger** | The permanent record of every money movement. Historically a physical book. |
| **Double-entry** | The practice of recording each movement twice — once as money leaving, once as money arriving — so the two must cancel out. |
| **Account** | A container holding a balance in one specific currency. Alice has a dollar account and a yen account; they are separate. |
| **Balance** | How much is in an account right now. |
| **Entry** | One half of a movement: "−$50 from account 1001". |
| **Transaction** | A complete movement: two matched entries that cancel out. |
| **Transfer** | A request to move money. It becomes a transaction once accepted. |
| **Server** | A program that runs continuously, waiting for other programs to send it requests. This engine is a server. |
| **Client** | Anything that sends requests to a server. A phone app, a website, or a person typing commands. |
| **Port** | A numbered doorway on a computer. One machine can run many servers; port numbers keep them apart. This one uses doorways 9000 and 9001. |
| **Protocol** | The agreed format for messages, so both sides understand each other. Like agreeing to speak English before a phone call. |
| **Concurrency** | Many things happening at overlapping times. The source of nearly every hard problem in this project. |
| **Thread** | One stream of work inside a program. This engine runs 24 of them side by side, so 24 transfers can be processed at once. |
| **Lock** | A *do not disturb* marker on a piece of data, so two threads cannot change it simultaneously. |
| **Deadlock** | Two threads each holding what the other needs, both waiting forever. |
| **Race condition** | A bug that only appears depending on the split-second timing of overlapping work — so it may show up once in ten million times, and never while you are watching. |
| **Backpressure** | When a system is overloaded, politely saying "I am full, try again shortly" instead of accepting work it cannot do. |
| **Idempotency** | The property that doing something twice has the same effect as doing it once. |
| **Invariant** | A statement that must be true at all times, forever. "Total money never changes on its own" is one. |
| **Database** | Software specialising in storing data safely and permanently, even if the power is cut. This project uses one called PostgreSQL. |
| **Container** | A packaged mini-computer-inside-your-computer, so software runs identically on any machine. This project uses Docker for that. |
| **Compiler** | A program that translates human-written source code into something the machine can actually execute. |

---

## How it works, in plain terms

Think of a busy restaurant.

```
   Customer          Waiter            Order queue        Kitchen           Recipe book
     (app)        (one person)       (limited size)    (24 cooks)         (the ledger)

  request ──────▶  takes it down  ──▶  ticket rail  ──▶  cooks it  ──▶  writes it down
                        ▲                                     │
                        └───────── plates come back ──────────┘
                             (only the waiter serves tables)
```

- **One waiter takes every order.** A single part of the program handles all
  incoming and outgoing messages. Having one waiter sounds slow, but they never
  cook — they only take orders and deliver plates, which is fast. And because only
  one person carries plates, two dishes can never collide on the way to a table.
- **Orders go on a rail with limited space.** If the rail is full, the waiter says
  "sorry, we are slammed, try in a moment" rather than accepting an order that will
  never be cooked. A queue that grows forever is worse than an honest refusal:
  customers end up waiting an hour for food they no longer want.
- **24 cooks work in parallel.** They do the actual work simultaneously, which is
  where the speed comes from.
- **Two cooks never touch the same pan.** Before changing an account, a cook claims
  it. And everyone claims accounts in the same fixed order, which is what makes the
  freeze-forever scenario impossible.
- **Everything is written in the recipe book, permanently.** Entries are never
  edited or erased — corrections are added as new entries, exactly like real
  accounting.

The engine also speaks in two different formats through two different doorways:

| Doorway | Format | Who it is for |
|---|---|---|
| **Port 9000** | Compact, machine-optimised | Real applications, at high speed |
| **Port 9001** | Readable text, one line per message | Humans. You can literally type at it and read the replies |

Both doorways understand exactly the same commands. The second exists purely so a
person can look inside and see what the engine is doing.

---

## Trying it yourself

You do not need to understand the code to run it. Every command below is explained.

### What you need

| To do this | You need |
|---|---|
| Run the correctness tests | A C++ compiler and a build tool called CMake. Works on Mac, Linux, or Windows |
| Try the storage layer | Also PostgreSQL (a database), either installed directly or via Docker |
| **Run the live server** | **Linux** — or Docker on a Mac, which provides Linux inside a container |

Why the server needs Linux: it uses a high-speed networking feature called `epoll`
that only exists on Linux. Everything else in the project runs anywhere.

If you are unsure what is installed on your machine, this command checks and tells
you plainly:

```bash
make doctor
```

### Step 1 — Get the code

```bash
git clone https://github.com/<your-username>/ledger-engine.git
cd ledger-engine
```

The first line downloads the project. The second moves you into its folder.

### Step 2 — Run the tests

```bash
make test
```

This compiles the project and then runs its full self-check suite — 151 separate
tests, including ones that launch dozens of simultaneous transfers to try to break
the accounting on purpose. It needs nothing installed beyond a compiler.

You should finish with a line saying all tests passed. That single line is the
project's core claim: money is conserved, overdrafts are impossible, and nothing
deadlocks.

### Step 3 — Start the database (optional)

```bash
make up
```

This starts PostgreSQL inside a container, along with a Linux environment for
running the server. Then:

```bash
make db-all
```

This wipes and rebuilds the storage structure, loads sample accounts, and runs 22
checks that deliberately try to store *invalid* data — a transfer that does not
balance, a movement between mismatched currencies, an attempt to edit history. All
22 must be rejected. If any is accepted, that is a bug.

### Step 4 — Start the engine

On a Mac, go into the Linux container first:

```bash
make shell
```

Then, inside it:

```bash
cmake -S . -B build && cmake --build build -j
./build/src/ledger_engine
```

(On Linux you can skip the container and simply run `make run`.)

You will see:

```
ledger engine up
  binary  port 9000  (length-prefixed)
  json    port 9001  (NDJSON, one object per line)
  workers 20 binary / 4 json (independent queues)
  accounts 5 loaded
```

The engine is now running and waiting. It has loaded five demo accounts.

### Step 5 — Stop it

Press **Ctrl-C**. It finishes any work already in progress, then reports what
happened and re-checks its own books:

```
stopped.
  json     6 handled, 0 refused (queue full), 0 dropped (client gone)
  transfers 5 committed / 1 rejected
  invariants passed — the ledger balances
```

That last line is the engine confirming that every account balance still matches
the sum of its recorded history.

---

## Talking to the engine

Open a second terminal window and connect to the human-readable doorway. `nc` is a
small tool that lets you type text directly at a server:

```bash
nc localhost 9001
```

Now you can type commands. Each one is a single line. The engine answers with a
single line.

> **Note:** if you are running the engine inside Docker on a Mac, run this from
> *inside* the container (`make shell`), because doorway 9001 is not yet opened to
> the outside world. Fixing that is on the roadmap.

### Command 1 — "Are you alive?"

```json
{"id":"1","type":"ping"}
```

Reply:

```json
{"id":"1","type":"pong","v":1}
```

`type` is what you are asking for. `id` is a label you choose, echoed back so you
can match replies to requests when several are in flight. `v` is the version of the
message format.

### Command 2 — "What is in account 1001?"

```json
{"id":"2","type":"get_account","account_id":"1001"}
```

Reply:

```json
{"balance":"115000","ccy":"USD","id":"1001","status":"ACTIVE","type":"account","v":1}
```

Reading it: account 1001 holds `115000` — and since this is US dollars, that means
**$1,150.00**. `ccy` is short for currency. The account is active, not closed.

### Command 3 — "Move $25.00 from account 1001 to account 2002"

```json
{"id":"3","type":"transfer","idem_key":"k1","from":"1001","to":"2002","amount":"2500","ccy":"USD"}
```

Reply:

```json
{"from_balance":"112500","id":"3","to_balance":"49500","tx_id":"900004","type":"transfer_ok","v":1}
```

What each part means:

| Part | Meaning |
|---|---|
| `idem_key` | Your unique ticket number for this request. Send the same one twice and the money moves only once |
| `from` / `to` | Which accounts. They must be different, and both must hold the same currency |
| `amount` | `2500` = **$25.00**, expressed in cents |
| `ccy` | The currency |
| `tx_id` | The engine's permanent reference number for this transaction |
| `from_balance` / `to_balance` | The two new balances — $1,125.00 and $495.00 |

### Command 4 — Try to spend money that is not there

```json
{"id":"4","type":"transfer","idem_key":"k2","from":"2002","to":"1001","amount":"99999999","ccy":"USD"}
```

Reply:

```json
{"code":"INSUFFICIENT_FUNDS","id":"4","message":"INSUFFICIENT_FUNDS","type":"error","v":1}
```

Refused. This is the check that stops money being spent twice, and it is the single
most important refusal the engine makes.

Other refusals you might see, in plain words:

| What comes back | What went wrong |
|---|---|
| `INSUFFICIENT_FUNDS` | Not enough money in the source account |
| `ACCOUNT_NOT_FOUND` | No account with that number |
| `SELF_TRANSFER` | You tried to send money from an account to itself |
| `CURRENCY_MISMATCH` | The two accounts hold different currencies. This engine does not do currency exchange |
| `INVALID_AMOUNT` | The amount was zero or negative |
| `SERVER_BUSY` | The engine is at capacity. Wait a moment and retry — this is the honest refusal, not a failure |
| `INTEGER_NOT_STRING` | You sent a number without quotation marks. See the warning below |
| `MISSING_FIELD` | A required piece of the message was left out |

### One rule if you are writing your own client

**Always put quotation marks around numbers.** Write `"amount":"2500"`, never
`"amount":2500`.

The reason: many programming languages, JavaScript in particular, cannot hold very
large whole numbers precisely. Feed one in and it silently comes back slightly
changed — no error, no warning, just a different number. For money, that is a cent
quietly vanishing. Sending numbers as text sidesteps the problem entirely, so the
engine rejects unquoted numbers rather than accepting them while they happen to be
small enough to survive.

The exact message formats, including the compact one used on doorway 9000, are
documented in [progress.md](progress.md#appendix-d--protocol-reference).

---

## How money is written down

The engine supports six currencies: **USD, EUR, JPY, GBP, CNY, TWD**.

Amounts are always whole numbers of the smallest unit of that currency:

| Currency | Smallest unit | `5000` means |
|---|---|---|
| USD, EUR, GBP, CNY, TWD | cent (or equivalent) | `$50.00` |
| **JPY** | the yen itself — there is no smaller unit | **`¥5,000`** |

This is why the engine never simply divides by 100. It looks up each currency and
formats accordingly. If you build anything on top of this, do the same.

Both sides of a transfer must use the same currency. Converting between currencies
is deliberately out of scope: a proper exchange involves rates, timing, and a
separate holding account, and pretending otherwise is how real systems end up with
untraceable money.

### The demo accounts

The running engine starts with five accounts:

| Number | Owner | Currency | Starts with | In readable form |
|---|---|---|---|---|
| `1001` | Alice | USD | `115000` | $1,150.00 |
| `2002` | Bob | USD | `47000` | $470.00 |
| `1003` | Alice | JPY | `5000` | ¥5,000 |
| `9001` | the system | USD | — | where dollars enter the system |
| `9002` | the system | JPY | — | where yen enter the system |

The two "system" accounts exist because money cannot simply appear. Even the
starting balances are created by transferring *out of* a system account, so every
single cent in the system has a matched pair of records behind it — right from the
very first one.

---

## How we know it is correct

Claiming software is correct is easy. Here is what actually backs it up.

**Tests that try to break it on purpose.** Not "does a transfer work" — anyone can
pass that. The suite runs 32 simultaneous streams performing tens of thousands of
transfers among the same handful of accounts, deliberately including the exact
pattern that causes the freeze-forever bug, and then checks that the total amount
of money is unchanged to the penny.

**Breaking the code deliberately, to prove the tests can catch it.** A test that
cannot fail proves nothing. So each safety mechanism was removed one at a time to
confirm the alarm actually sounds. For example, removing the rule that orders lock
acquisition froze the system within 30 seconds. Weakening the account locks lost
**8,081 units out of 20,000,000** — a loss of four-hundredths of one percent, the
kind of slow bleed that would never be noticed in casual testing, and exactly the
kind that destroys a real financial system.

**Specialised bug-hunting tools.** Two tools called ThreadSanitizer and
AddressSanitizer instrument the program while it runs and detect timing bugs and
memory bugs that ordinary testing cannot see. Both report the code clean. They
found three genuine timing bugs during development that careful human review had
missed.

**Automated checking on every change.** Every time the code is modified, the full
test suite and both bug-hunting tools run automatically. Nothing gets in without
passing.

**The five promises, checked continuously:**

| | The promise |
|---|---|
| **I1** | Every transaction's two halves cancel out exactly |
| **I2** | Every balance equals the sum of that account's history — this is what catches money quietly going missing |
| **I3** | Total money per currency never changes on its own, even under heavy simultaneous load |
| **I4** | No ordinary account can go below zero, no matter how many withdrawals arrive at once |
| **I5** | The same ticket number always produces the same result, applied only once |

---

## Where the project is now

The engine works today. You can start it, connect to it, move money, and watch it
refuse invalid requests. What it cannot yet do is **remember anything after being
switched off** — right now everything lives in memory only. That is the next major
piece of work.

| Step | What it delivers | Status |
|---|---|---|
| 0–5 | Design, build setup, storage structure, the accounting core, the networking layer, the message formats | ✅ Done |
| 10 | The engine reports its own live statistics | ⬜ **Next** |
| 11 | A bridge so a web browser can talk to it | ⬜ |
| 12 | **A visual dashboard** — watch balances change, transfers arrive, and the safety mechanisms work, live in a browser | ⬜ |
| 6 | Permanent storage, so nothing is lost on restart | ⬜ |
| 7 | Re-verifying all the safety guarantees against permanent storage | ⬜ |
| 8 | Performance measurement and tuning | ⬜ |
| 9 | Final diagrams and documentation | ⬜ |

The numbering looks out of order because it is. Steps 0–9 were the original plan;
steps 10–12 were added afterwards, and are being done **first**. The reason: right
now, the only proof this engine works is a line of test output. The dashboard turns
that into something anyone can watch happening. Building it before permanent
storage also means the dashboard is already running when storage is added
underneath — so that change becomes something you can *see* work, rather than
something you have to take on faith.

---

## Learning more

- **[progress.md](progress.md)** — the complete engineering record. Every step,
  what it delivered, how it was built, the reasoning behind each decision, what the
  tests prove, and the full technical message reference. Written for a technical
  reader.
- **[docs/stage-0-architecture.md](docs/stage-0-architecture.md)** — the original
  design document (in Traditional Chinese).

### Every command, in one place

```bash
make doctor     # check what is installed on your machine, and what is missing
make test       # compile and run the full self-check suite
make up         # start the database and the Linux container
make shell      # step inside the Linux container
make run        # start the engine (Linux only)
make db-all     # rebuild storage, load samples, run 22 rejection checks
make db-check   # verify the stored data is still internally consistent
make psql       # open the database directly, for a look around
make down       # shut the containers down
make clean      # delete compiled files
make help       # list everything
```

---

## License

MIT — free to use, modify, and distribute. See [LICENSE](LICENSE).
