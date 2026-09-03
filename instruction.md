# Working rules for this project

House rules for anyone — human or agent — working in this repository.

---

## 1. The two documents

This project keeps two documents, and they have strictly separate jobs. Do not
let their content bleed into each other.

### `README.md` — for the reader with no software background

Written for someone who does **not** know what a server, a thread, or a protocol
is. Rules:

- Explain every technical term the first time it appears. There is a glossary
  section; add to it rather than assuming knowledge.
- Lead with the problem and why it is hard, not with the architecture.
- Every command shown must say what it does and what the reader should see.
- No implementation detail, no design rationale, no test internals. If a sentence
  answers "why was it built this way", it belongs in `progress.md`.

### `progress.md` — for the non technical reader to understand want had been done

The complete engineering record: every step, what it delivered, how, where the
code is, what the tests prove, and why each decision was made. Rules:

- Steps are numbered and never renumbered — the numbers appear in commit
  messages, source comments, and the README.
- Link to code as `path/to/file.ext:line` so claims can be checked.
- Record the reasoning, including approaches that were tried and rejected. A
  decision without its "why" is worth very little six months later.
- The full wire protocol specification lives here, in the appendices.

### The update rule

**Any change to the project updates both documents in the same change.** A
feature, a fix, a refactor, a new stage — before it is done:

- `progress.md` gets the technical account: what changed, why, where.
- `README.md` gets whatever a non-technical user would now see differently — new
  commands, new behaviour, new limitations. If genuinely nothing user-facing
  changed, say so in the summary rather than silently skipping it.

---

## 2. Testing

### Structure: Arrange-Act-Assert

Every test is written in three labelled phases, in this order:

```cpp
TEST(Suite, DoesTheThing) {
  // Arrange —— set up the world this test needs
  Buffer buf;
  buf.append("hello world");

  // Act —— the single action under test
  const std::string consumed = buf.retrieveAsString(6);

  // Assert —— what must now be true
  EXPECT_EQ(consumed, "hello ");
  EXPECT_EQ(buf.view(), "world");
}
```

The labels are literal comments (`// Arrange`, `// Act`, `// Assert`), because a
reader skimming a 600-line test file should be able to find the one line that
actually does the thing without reading the rest.

Four allowed variations, each of which must be labelled as such:

| Situation | Label to use |
|---|---|
| No setup needed | Start at `// Act` |
| A pure fact table — no setup, no action, just constants to verify | `// Assert only` plus a note saying why |
| Action and check genuinely interleave (a stream arriving in pieces) | `// Act & Assert` |
| Shared setup for a whole fixture | `// Arrange（共用）` on the fixture's `SetUp()` |

Do not invent an empty Arrange block to satisfy the pattern. AAA is there to make
the structure obvious, not to be decorated onto tests that do not have it.

Concurrency tests have a specific shape: the Act phase spawns the threads **and
joins them all**. No assertion may run while a thread is still going, or a racy
interleaving can be mistaken for a passing run.

### Which framework, and why two

| Layer | Framework | What it tests |
|---|---|---|
| C++ unit and integration — `tests/*.cpp` | **GoogleTest**, run by `ctest` | Calls the C++ API directly. The ledger core, the codecs, the queue, the event loop |
| End-to-end — `tests/e2e/*.py` | **pytest** | Starts the real `ledger_engine` binary and talks to it over a real TCP socket. Knows nothing about the internals |

**pytest does not and cannot replace the GoogleTest suite.** Those tests call C++
functions directly; reaching them from Python would mean building and maintaining
a binding layer whose only purpose is to run tests, and it would make the
sanitizer builds far harder to run. The two suites test different things and both
are required.

Rules for the pytest suite:

- **Standard library only**, plus pytest. A socket and `json.loads` are the whole
  client. A dependency shared with the server would let a mistake cancel itself
  out.
- **Black-box.** Assert only on what a browser, a load script, or a person using
  `nc` could observe. That is what makes these tests survive Step 6, when the
  storage layer underneath is replaced.
- **One engine process per test.** The engine seeds its demo accounts at startup,
  so a fresh process is a clean ledger and tests cannot leak state into each other.
- **Integers stay strings.** Never `json.loads` an amount into a Python-side
  assumption about numeric JSON; the wire format is quoted for a reason.

### A test that cannot fail proves nothing

Before claiming a test verifies something, break the code deliberately and
confirm the test goes red. Record what happened — the failure mode and any
numbers — in `progress.md`, Appendix B. Existing entries there are the model.

Note the failure *shapes*: a deadlock and a missed `notify_all` never fail an
assertion, they simply never return. That is why `gtest_discover_tests` sets a
timeout and why timing tests carry explicit upper bounds.

### Running them

```bash
make test    # GoogleTest via ctest
make e2e     # pytest against a real running server (needs Linux)
make tsan    # ThreadSanitizer build + tests
make asan    # AddressSanitizer + UBSan build + tests
```

---

## 3. Code

- **C++20.** Warnings are errors in CI (`-DLEDGER_WERROR=ON`).
- **Run `make fmt` before committing.** CI runs `make fmt-check` and fails on
  drift.
- **The server needs Linux** (epoll). Everything else builds natively on macOS —
  keep it that way, because a second compiler catches what one cannot.
- **Comments explain *why*, not *what*.** The code already says what it does.
  Existing comments are being migrated from Traditional Chinese to English; new
  comments should be in English, but do not rewrite unrelated Chinese comments as
  a side effect of another change.
- **Never renumber, reorder, or reuse a protocol value.** Message type codes and
  `ErrorCode` values are serialized on the wire. New ones are appended to the end.
  Reordering `fields()` silently changes the binary layout — the golden table in
  `tests/test_codec.cpp` is what catches it.
- **Money is `int64_t` minor units.** No floating point, and never divide by 100 —
  look the exponent up per currency.

---

## 4. Git

- Commit messages say what changed and why, not just which files.
- One logical change per commit. Stage 5 was split into 5a/5b/5c precisely so a
  codec bug and a queue bug could not have to be debugged at the same time.
- Do not commit build output, `__pycache__`, or anything under `build/`.
