# Stage 0 — 架構設計

> **狀態：已定案。** 第 5 點（記憶體與 DB 的關係）選定 **方案 B — write-through**。
> 幣別、協定格式、帳戶 id 來源皆採預設值。Stage 1 骨架已完成。

---

## 0. 已定案決策

| 決策 | 結論 | 對架構的影響 |
|---|---|---|
| 幣別 | 多幣別 | 帳戶身分含幣別；不變式改為 per-currency；金額小數位數必須查表 |
| 冪等性 | 納入 | 協定加 `idempotency_key`；`transactions` 表加 UNIQUE；記憶體加分片快取 |
| 交易腳數 | 固定兩腳 | 一筆 transaction 恰好兩筆 entry；只需排序兩個帳戶 |
| 記憶體 vs DB | **方案 B（write-through）** | 記憶體鎖是真正的併發控制；DB commit 成功才套用記憶體餘額 |
| 協定格式 | 長度前綴 + binary | Stage 5 定細節 |
| 帳戶 id | server 端產生（BIGINT 遞增） | client 不指定 id |

### 幣別清單

| ISO 4217 | 幣別 | exponent | int64 值 5000 代表 |
|---|---|---|---|
| USD | 美元 | 2 | $50.00 |
| EUR | 歐元 | 2 | €50.00 |
| GBP | 英鎊 | 2 | £50.00 |
| CNY | 人民幣 | 2 | ¥50.00 |
| TWD | 新台幣 | 2 | NT$50.00 |
| JPY | 日圓 | **0** | **¥5,000** |

**JPY 陷阱**：同一個 `int64 amount = 5000`，USD 是五十元，JPY 是五千元 —— 差一百倍。所有顯示/解析必須查 `currency.exponent`，絕不能寫死 `/100`。

### 三條基本規則

1. 金額一律 `int64_t` 存最小單位，永不用 `double`。
2. 轉帳兩腳幣別必須相同，不同幣別回 `CURRENCY_MISMATCH`（v1 不做 FX）。
3. `entries` 表 append-only，永不 UPDATE / DELETE。沖銷靠反向交易。

---

## 1. 模組切分

**切分原則：net 層不知道什麼是餘額，core 層不知道什麼是 socket。**

```
ledger-engine/
├── CMakeLists.txt
├── docker-compose.yml          # postgres:16 + engine
├── Dockerfile.dev              # Ubuntu 22.04 建置容器
├── Makefile                    # 常用指令封裝
├── .clang-format
├── db/migrations/
│   ├── 001_currencies.sql
│   ├── 002_accounts.sql
│   ├── 003_transactions.sql
│   └── 004_entries.sql
├── include/ledger/
│   ├── common/     Result.h  Config.h  Logger.h  Metrics.h  Version.h
│   ├── money/      Currency.h  Money.h
│   ├── core/       Account.h  AccountRegistry.h  LedgerCore.h
│   │               Journal.h  IdempotencyCache.h
│   ├── concurrent/ BlockingQueue.h  ThreadPool.h  Task.h
│   ├── net/        EventLoop.h  Acceptor.h  Connection.h  Buffer.h
│   ├── proto/      Frame.h  Messages.h  Codec.h
│   └── db/         PgPool.h  LedgerRepository.h
├── src/                        # 對應 .cpp
├── tests/
│   ├── test_version.cpp        # Stage 1
│   ├── test_money.cpp          # Stage 2
│   ├── test_ledger_core.cpp    # Stage 3
│   ├── test_codec.cpp          # Stage 5
│   └── test_concurrency.cpp    # Stage 7 — TSan 主戰場
└── bench/
    ├── tcp_client.py           # Locust 用的自訂 TCP client
    └── locustfile.py
```

| 類別 | 單一職責 | Stage |
|---|---|---|
| `Currency` | ISO 4217 ↔ enum、exponent 查表、格式化。無狀態無鎖 | 2 |
| `Money` | 值型別 `{int64 units, Currency ccy}`，加減檢查溢位與幣別。immutable | 3 |
| `Account` | `id/owner/currency/balance/version` **＋自己的 `shared_mutex`**。位址穩定 | 3 |
| `AccountRegistry` | 只做 `id → Account*` 查找與新增。一把 map 級 `shared_mutex`，**只保護容器結構** | 3 |
| `LedgerCore` | **唯一知道「怎麼轉帳」的地方**。排序取鎖、驗證、套用、產生 entries | 3 / 6 |
| `Journal` | 記憶體中 append-only 的交易與 entry 序列，供測試重算餘額 | 3 |
| `IdempotencyCache` | 64 分片 `key → 結果`，每片一把 `shared_mutex`，帶 TTL | 5 |
| `BlockingQueue<T>` | 有界 MPMC 佇列：`push` / `try_push` / `pop` / `close` | 5 |
| `ThreadPool` | 啟動 N 個 worker，每個綁一條 DB 連線 | 5 |
| `Buffer` | 可成長位元組緩衝，雙游標。ET 模式累積半包 | 4 |
| `Connection` | 單一 fd 的完整狀態。由 `shared_ptr` 持有 | 4 |
| `Acceptor` | listen fd，迴圈 `accept4(NONBLOCK)` 到 `EAGAIN` | 4 |
| `EventLoop` | `epoll_wait` 主迴圈、`wakeupFd`、pending write。**唯一能對 socket `write()` 的地方** | 4 |
| `Codec` | 長度前綴切包與序列化。純函式 | 5 |
| `PgPool` | 持有 N 條 `pqxx::connection`，worker 長期持有 | 6 |
| `LedgerRepository` | 把轉帳翻譯成一個 Postgres 交易。**唯一寫 SQL 的地方** | 6 |

三個關鍵邊界：`Money`/`Currency` 可無執行緒單測到 100%；`LedgerCore` 是唯一有鎖邏輯的檔案（查死鎖只讀這一個）；`LedgerRepository` 是唯一寫 SQL 的地方（換方案 5 的策略只改這一層）。

---

## 2. 執行緒模型

### 為什麼一定要拆（數字）

假設一筆轉帳：純 CPU 約 **0.05 ms**，等 Postgres（含 COMMIT fsync）約 **1.5 ms**。

| 做法 | 單筆佔用 event loop | 理論 TPS 上限 |
|---|---|---|
| 全塞在 event loop | 1.55 ms | 1 ÷ 1.55 ms ≈ **645** |
| IO thread + 20 workers | 0.05 ms | 20 ÷ 1.55 ms ≈ **12,900** |

差 20 倍，而且第一種做法還會讓「一筆慢交易停住所有連線」。**鐵律：event loop 執行緒永遠不做任何會阻塞的事。**

### worker 數量：為什麼是 20+

```
N_threads ≈ N_cores × (1 + 等待/計算) = 4 × (1 + 1.5/0.05) = 124   ← 理論值
```

理論值不能直接用：① Postgres `max_connections` 預設 100，每條連線是一個獨立行程（5–10 MB）；② 124 執行緒在 4 核上輪轉，context switch 與快取汙染吃掉增益。

實務落點 **20–32 worker，每個長期持有一條 DB 連線**（不是每筆借還）。Stage 8 把 worker 數當可調參數，畫 TPS-vs-worker 曲線找轉折點。

### 佇列選型：有界 mutex + condition_variable

**為什麼有界（8192）**：提供背壓。無界佇列在 DB 變慢時記憶體無上限成長、排隊時間爆炸（client 早已 timeout 而 worker 還在處理）。8192 ≈ 0.6 秒積壓，超過就沒意義。

> ⚠️ **佇列滿時 IO 執行緒不可阻塞等待**。只用 `try_push()`，失敗直接回 `SERVER_BUSY`。一旦阻塞，整個 event loop 停擺。

**為什麼不用 lock-free MPMC**：

1. **優化錯地方** — 佇列一次 push/pop ≈ 100 ns，DB 等待 1,500,000 ns，差四個數量級。
2. **新 bug 來源** — 要自己處理 ABA 與記憶體回收（hazard pointer / epoch），且 TSan 對自訂原子演算法判讀不可靠。
3. **競爭不嚴重** — 臨界區只推一個 `Task`，持鎖約 50 ns。12,900 TPS 下對 futex 毫無壓力。

**何時該換**：Stage 8 用 `perf` 看，若 futex 等待佔 CPU > 5%，正確的下一步是**每 worker 一個佇列 + 輪詢派發**（再加 work stealing），而不是換 lock-free。單一共享佇列的好處正是天然負載平衡。

### 回程的兩個不變式

**W1 — 一個 fd 只能有一個寫入者。** 兩個 worker 同時 `write()` 同一 socket，位元組會交織，長度前綴對不上，協定崩潰。這**不是 data race**（`write()` 本身安全），所以 **TSan 抓不到**。規則：只有 IO 執行緒能 `write()` socket。

**W2 — worker 完成時連線可能已不在。** `Task` 存 `std::weak_ptr<Connection>`，worker 完成後 `lock()` 升級，失效就丟棄結果。否則是 use-after-free。

### 回程機制

worker → `conn->queueWrite(resp)`（小 mutex）→ `loop->addPendingWrite(conn)` → `eventfd_write(wakeupFd, 1)` → IO thread 醒來統一 `write()`。

---

## 3. 併發策略

| 資料 | 機制 | 理由 |
|---|---|---|
| `AccountRegistry` 的 map | `shared_mutex`（一把，map 級） | 查找遠多於開戶。**只保護容器結構，不保護帳戶內容** |
| `Account.balance/status` | `shared_mutex`（每帳戶一把） | 查詢用 `shared_lock`，轉帳用 `unique_lock`。粒度＝單一帳戶 |
| `IdempotencyCache` | 64 分片，每片一把 `shared_mutex` | `hash(key)%64` 把爭用打散成 1/64 |
| 全域對帳 | 一把全域 `shared_mutex` | **反向用法**：轉帳拿 shared（彼此不互斥），對帳拿 unique（取一致快照） |
| Metrics 計數器 | `atomic<uint64_t>` relaxed | 只要最終計數對，不需與其他資料建立順序 |
| 交易 ID 產生器 | `fetch_add(1, relaxed)` | 只需唯一遞增 |
| 關機旗標 | `atomic<bool>` release/acquire | 需要 happens-before。relaxed 在這裡是錯的 |
| `Connection` outbound buffer | 普通 `std::mutex` | 存取者都是寫入者，`shared_mutex` 沒好處還更慢 |
| `BlockingQueue` | `mutex` + 2 個 `condition_variable` | 需要「等待」語意，atomic 只能忙等 |

### 為什麼 balance 不能只用 `atomic<int64_t>`

A 有 5000，同時來兩筆 A→B 各 3000。用 CAS 迴圈：

```
T1: load A → 5000, 檢查 5000 >= 3000 ✓, CAS(5000→2000) 成功
T2: CAS 失敗 → 重試 → load 2000, 檢查 2000 >= 3000 ✗ → 拒絕   ← 這部分是對的
```

問題不在這裡。T1 的完整動作有四件事：

```
① A.balance -= 3000     ← atomic 保護得到
② B.balance += 3000     ← 另一個 atomic，跟 ① 之間沒有原子性
③ INSERT entry(A, -3000)
④ INSERT entry(B, +3000)

在 ① 完成、② 尚未開始的瞬間，第三個執行緒對帳 → 看到總額少了 3000
→ double-entry 最核心的不變式被觀察到破裂
```

**結論**：`atomic` 保護的是**單一變數**；這裡需要的是**四件事一起發生** —— 那是互斥區間的語意。`atomic` 適合不需要跟別人一起變的東西（計數器、旗標、ID）；`shared_mutex` 適合需要一起變且讀多寫少的東西（餘額、帳戶表）。

### 死鎖：A→B 與 B→A

Coffman 四條件：互斥、持有並等待、不可搶佔、**循環等待**。前三個無法避免，所以打破第四個。

**天真做法（先鎖來源）**：T1 持有 1001 等 2002；T2 持有 2002 等 1001 → 成環 → 永久死鎖。

**排序法**：`auto [lo, hi] = std::minmax(fromId, toId);` 一律先鎖 `lo`。T1 和 T2 都是先 1001 再 2002；T2 被擋在 1001 時**手上沒有任何鎖**，不可能成為別人的阻礙者 → 無環。

**形式化保證**：對帳戶定義全序（`account_id` 遞增），規定取鎖順序必為遞增子序列。若存在環 T₁→T₂→…→Tₙ→T₁，沿環走一圈 id 必須嚴格遞增，最後回到起點又須等於起點 —— 矛盾。**這是證明，不是經驗法則。**

```cpp
// 概念示意
if (fromId == toId) return Error::SELF_TRANSFER;   // ← 必須在取鎖前擋掉

auto [loId, hiId] = std::minmax(fromId, toId);
Account* lo = registry.find(loId);
Account* hi = registry.find(hiId);

// ┌─ 臨界區開始 ─────────────────────
std::unique_lock loGuard(lo->mtx);   // 一律小 id 先
std::unique_lock hiGuard(hi->mtx);
   ...驗證與套用...
// └─ 臨界區結束（RAII 反向釋放：hi → lo）
```

**三個必須處理的邊界**：

1. **`from == to`** — 不擋掉的話 `loId == hiId`，同一把非遞迴 mutex 鎖兩次是 UB，實務上自己卡死自己，看起來像隨機 hang。
2. **DB 端順序必須一致** — `SELECT ... FOR UPDATE` 要加 `ORDER BY id`。否則 Postgres row lock 死鎖（SQLSTATE `40P01`），白白浪費一趟往返並需重試。
3. **鎖階層全域一致** — `L0 = registry` → `L1 = 帳戶（id 遞增）` → `L2 = journal`。只能由低往高，永不反向。

**為什麼 registry 鎖可以先放掉**（必須寫進註解的前提）：

```cpp
std::unordered_map<AccountId, std::unique_ptr<Account>> accounts_;
```

- `unordered_map` 是 node-based，rehash 時節點不搬家；
- 存的是 `unique_ptr`，`Account` 物件位址永遠不變；
- 規定**帳戶只新增、永不 erase**（關戶只標記 `status = CLOSED`）。

三者同時成立，`Account*` 一旦取得就永遠有效。**若未來有人加了 `erase()`，這整套就變成 use-after-free。**

**為什麼不用 `std::scoped_lock(m1, m2)`**：① 高競爭下反覆 try/釋放/重試，延遲尾巴不可預測、有活鎖傾向；② 不能直接處理 `shared_mutex` 的 shared 模式；③ 明確排序法是 O(1)、可預測、面試三句話講得完。這裡選**可解釋性**而不是方便。

### 兩個已知限制（誠實面）

**限制一：沒有跨帳戶的全域一致快照。** 轉帳只鎖兩個帳戶，第三個執行緒逐一鎖 A 讀、放開、鎖 B 讀，可能讀到 A 的新值 + B 的舊值 → 誤報帳目不平。
**解法（Stage 3 就做）**：加一把全域 `shared_mutex`，**轉帳拿 `shared_lock`、對帳拿 `unique_lock`**。轉帳彼此都是 shared 所以零成本互斥；只有罕見的對帳排他。這是 `shared_mutex` 很漂亮的反直覺用法，值得寫進 README。

**限制二：`shared_lock` 不是免費的。** `lock_shared()` 要遞增讀者計數，那是一次寫入；多核高頻讀同一帳戶時 cache line 會在核心間彈跳。
**因應**：Stage 8 單獨壓「純餘額查詢」一輪，真的是瓶頸才考慮 seqlock / RCU。**先量測再決定。** 另外 `shared_mutex` 的寫者優先性由實作決定（libstdc++ 與 pthread rwlock 行為不同），高讀取量下可能讓轉帳飢餓，Stage 7 要實測。

---

## 4. 雙式記帳資料模型

**核心概念：entry 是不可變的事實，balance 是可變的快照。** 事實永不改，快照可隨時從事實重算 —— 這個關係就是系統的自我檢查機制。

### 具體例子

Alice（`1001`, USD, $1,200.00）轉 **$50.00** 給 Bob（`2002`, USD, $420.00）。

**transactions**

| id | idempotency_key | ccy | amount | debit | credit | status |
|---|---|---|---|---|---|---|
| 900001 | req-a3f9-01 | USD | 5000 | 1001 | 2002 | COMMITTED |

**entries**

| transaction_id | account_id | ccy | amount | balance_after | 意義 |
|---|---|---|---:|---:|---|
| 900001 | 1001 | USD | **−5000** | 115000 | 借方 DEBIT |
| 900001 | 2002 | USD | **+5000** | 47000 | 貸方 CREDIT |
| | | | **0** | | ✓ 兩腳相加必為零 |

### 為什麼用帶號整數而非 direction enum

不變式可以直接用一條 SQL 驗：

```sql
-- I1：每筆交易兩腳相加為 0（回空集合才算過）
SELECT transaction_id FROM entries
GROUP BY transaction_id HAVING SUM(amount) <> 0;

-- I2：餘額快照必須等於該帳戶所有 entry 總和（抓 lost update 的殺手鐧）
SELECT a.id, a.balance, SUM(e.amount) AS recomputed
FROM accounts a JOIN entries e ON e.account_id = a.id
GROUP BY a.id, a.balance
HAVING a.balance <> SUM(e.amount);

-- I3：每幣別總額守恆
SELECT currency, SUM(balance) FROM accounts GROUP BY currency;
```

代價是不符傳統會計 DR/CR 呈現。解法：加一個 generated column `direction`。**正確性用帶號，呈現用 direction。**

### Schema（Stage 2 正式寫成 migration）

```sql
CREATE TYPE tx_status AS ENUM ('PENDING','COMMITTED','FAILED');

CREATE TABLE currencies (
  code      CHAR(3)  PRIMARY KEY,        -- 'USD'
  exponent  SMALLINT NOT NULL            -- USD/EUR/GBP/CNY/TWD=2, JPY=0
);

CREATE TABLE accounts (
  id             BIGINT      PRIMARY KEY,
  owner_id       BIGINT      NOT NULL,
  currency       CHAR(3)     NOT NULL REFERENCES currencies(code),
  balance        BIGINT      NOT NULL DEFAULT 0,      -- 最小單位
  allow_negative BOOLEAN     NOT NULL DEFAULT FALSE,  -- 系統帳戶才可為負
  status         SMALLINT    NOT NULL DEFAULT 0,      -- 0=ACTIVE 1=CLOSED
  version        BIGINT      NOT NULL DEFAULT 0,
  created_at     TIMESTAMPTZ NOT NULL DEFAULT now(),
  CONSTRAINT balance_non_negative CHECK (allow_negative OR balance >= 0),
  UNIQUE (owner_id, currency)                  -- 一人一幣別一個帳戶
);

CREATE TABLE transactions (
  id                BIGINT      PRIMARY KEY,
  idempotency_key   TEXT        NOT NULL UNIQUE,       -- 冪等性的真正保證
  status            tx_status   NOT NULL,
  currency          CHAR(3)     NOT NULL REFERENCES currencies(code),
  amount            BIGINT      NOT NULL CHECK (amount > 0),
  debit_account_id  BIGINT      NOT NULL REFERENCES accounts(id),
  credit_account_id BIGINT      NOT NULL REFERENCES accounts(id),
  created_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
  CHECK (debit_account_id <> credit_account_id)        -- 擋自己轉自己
);

CREATE TABLE entries (
  id             BIGSERIAL   PRIMARY KEY,
  transaction_id BIGINT      NOT NULL REFERENCES transactions(id),
  account_id     BIGINT      NOT NULL REFERENCES accounts(id),
  currency       CHAR(3)     NOT NULL,
  amount         BIGINT      NOT NULL CHECK (amount <> 0),  -- 負=借 正=貸
  balance_after  BIGINT      NOT NULL,                      -- 稽核用快照
  created_at     TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX ON entries (account_id, id);   -- 對帳單查詢
CREATE INDEX ON entries (transaction_id);   -- 不變式檢查
```

### 原子性：兩層

| 層級 | 機制 | 保證 |
|---|---|---|
| **DB 層** | 五個寫入包在同一個 `BEGIN … COMMIT`（1 transaction + 2 entry + 2 balance UPDATE） | WAL + fsync 保證 all-or-nothing。crash 在 COMMIT 前全部消失，之後全部存在。**沒有中間狀態** |
| **記憶體層** | 兩個帳戶的 `unique_lock` 從「檢查餘額」持有到「寫入餘額」全程不放 | 任何觀察者至少要取得其中一把 `shared_lock`，看不到「A 已扣、B 未加」 |

### 多幣別三規則

1. 取鎖後第一件事檢查 `lo->currency == hi->currency`，不符回 `CURRENCY_MISMATCH`。
2. 不變式改成 per-currency：`SUM(amount) GROUP BY currency = 0`。
3. 顯示與解析一律查 exponent，`Money::toString()` 內部查表，只能有一個實作點。

### 冪等性：兩層防護

| 層級 | 機制 | 負責 |
|---|---|---|
| 記憶體 `IdempotencyCache` | 64 分片 hash map，TTL 24h | **效能**。高頻重送直接回舊結果。重啟遺失沒關係，它不是正確性來源 |
| DB `UNIQUE(idempotency_key)` | 唯一索引，衝突回 SQLSTATE `23505` | **正確性**。唯一真正的保證。捕捉 23505 → 回查原交易 → 回傳原結果。跨重啟、跨程序都有效 |

只有記憶體層，重啟後會重複扣款；只有 DB 層，每次重送浪費一趟往返。**記憶體那層是快取，DB 那層是真理。**

### Stage 3 GoogleTest 要驗的五條不變式

- **I1** 每筆交易兩腳相加為零：`SUM(entries.amount WHERE tx_id = T) == 0`
- **I2** 餘額可從 entries 重算：`account.balance == SUM(entries.amount WHERE account_id = A)` — 抓 lost update
- **I3** 每幣別總額守恆：32 執行緒在 100 帳戶間隨機轉帳 10 萬筆，跑完 `SUM(balance) GROUP BY currency` 必須與初始值完全相同。**這是整個專案最重要的一條測試**
- **I4** 使用者帳戶餘額不為負（在 I3 的併發壓力下也是）— 這就是防 double-spending
- **I5** 相同 `idempotency_key` 產生相同 `transaction_id`：同一筆送 100 次，帳戶只變動一次

---

## 5. 記憶體與 PostgreSQL 的關係 —— 已選定方案 B

問題本質：**「誰說了算」與「什麼時候才算數」。**
三方案唯一在變的，是 **DB 的 COMMIT 落在記憶體鎖的哪一邊**。

### 方案 A — DB 為唯一真實來源（未採用）

每筆轉帳都進 Postgres：`BEGIN` → 依 id 遞增 `SELECT … FOR UPDATE` 兩列 → 驗證 → 寫入 → `COMMIT`。記憶體只做唯讀快取，甚至可以不要。

- 互斥機制：Postgres row lock
- 回應時機：COMMIT 之後
- 預估 TPS：3,000 – 8,000
- 崩潰語意：最乾淨，不需 recovery 邏輯
- ✅ 正確性最強、程式碼最少、最好驗證。真實銀行核心就是這樣做的
- ❌ C++ 併發設計退化成「把請求平行送給 DB」。**與專案要展示 `shared_mutex` 併發能力的目標直接衝突**

### ★ 方案 B — 記憶體判斷 + 同步落庫（write-through）【採用】

取記憶體鎖 → 驗證餘額 → **在鎖內**完成 Postgres 交易 → COMMIT 成功才套用記憶體餘額 → 放鎖 → 回應。DB 是最終真相，記憶體是與它保持同步的加速層。

- 互斥機制：記憶體 `shared_mutex`（DB row lock 當第二道防線）
- 回應時機：COMMIT 之後
- 預估 TPS：3,500 – 9,000（寫）／數十萬（純讀，完全不碰 DB）
- 崩潰語意：記憶體只在 COMMIT 成功後才動，永遠不會超前 DB
- ✅ 鎖設計是真正的併發控制；餘額查詢不碰 DB；重啟從 DB 一次載入即可重建
- ❌ 臨界區含 DB 往返（≈1.5 ms）→ 熱點帳戶會被序列化

### 方案 C — 記憶體權威 + 批次落庫（write-behind）（未採用）

鎖內只做檢查與記憶體套用（≈0.05 ms），立刻放鎖；交易丟進待落庫佇列，獨立 flusher 執行緒把 100 筆合併成一次 COMMIT，攤平 fsync 成本。

- 互斥機制：記憶體 `shared_mutex`（臨界區極短）
- 回應時機：等批次落庫（安全）／立刻（**有丟失風險，記帳系統不可接受**）
- 預估 TPS：30,000 – 80,000
- 崩潰語意：複雜，需要自己的 WAL 與 replay
- ✅ TPS 最高，臨界區最短，熱點帳戶表現最好
- ❌ 批次中一筆失敗要回滾已套用的記憶體狀態 —— 分散式系統等級的難題，做壞了會產生「帳目不平」

### 為什麼選 B

1. **唯一符合專案目標的選項。** 規格明訂要用 `std::shared_mutex`、自訂 event loop、20+ worker 展示併發設計。方案 A 會讓這些變裝飾品 —— 真正做互斥的是 Postgres，C++ 只是代理層。面試官問「你的鎖設計解決了什麼問題」時，方案 A 沒有答案。
2. **方案 C 的難點做不完，失敗模式最糟。** 「批次落庫失敗要回滾已套用的記憶體狀態」在 Stage 3–7 做不對。做壞的後果是帳目不平 —— 對一個以「ACID 記帳引擎」為賣點的作品集，這是最難堪的失敗。
3. **B 的崩潰語意乾淨到一句話說完。** 「記憶體只在 DB COMMIT 成功後才改，所以記憶體永遠不會超前 DB。」重啟時 `SELECT id, balance FROM accounts` 一次載入就重建完畢，不需任何 replay。
4. **B 保留升級到 C 的路。** Stage 8 若要拚 TPS，把 COMMIT 換成 group commit（多 worker 的交易由一個 flusher 合併 fsync，但仍等落庫才回應）就是 C 的安全版本，**完全不用改鎖設計**。

### B 的弱點必須誠實寫進 README —— 而且這正是加分題

臨界區含 DB 往返約 1.5 ms → **單一帳戶理論上限 = 1 ÷ 1.5 ms ≈ 660 TPS**，加再多 worker 都沒用。壓測結果會強烈依賴流量分佈：

- **均勻分佈**（1000 帳戶隨機兩兩轉帳）：碰撞率低，總 TPS 由 worker 數決定 → 約 **9,000**
- **熱點分佈**（所有人轉給同一商戶帳戶）：全部序列化在那一把鎖 → 約 **660**，與 worker 數無關

Stage 8 兩種分佈都跑、把對比圖放進 README，比單純 report 一個「9,000 TPS」有說服力太多 —— 因為它證明你知道系統**什麼時候會壞**。

---

## 6. 一筆轉帳的完整路徑（方案 B）

```
╔══ 客戶端 ══════════════════════════════════════════════════╗
   送出 frame：[len:4][type:2][ver:2][payload]
   payload = { idem_key:"req-a3f9-01", from:1001, to:2002,
               amount:5000, ccy:"USD" }
╚═══════════════════════════════════════════════════════════╝
                            │  TCP :9000
                            ▼
① IO Thread ─ epoll_wait 返回 EPOLLIN | EPOLLET
   │
   ├─ while(true) read(fd, buf, 64K) 直到 EAGAIN
   │     └─ ET 模式必須讀乾。少讀一次，剩下的資料
   │        不會再觸發事件 → 該連線永久靜默。
   │
   ├─ 資料進 Connection::inputBuffer_
   ├─ Codec 嘗試切出完整 frame
   │     ├─ 不足一個 frame（半包）→ 留在 buffer，return，等下次
   │     └─ 一次讀到多個 frame（黏包）→ 迴圈切完為止
   │
   ├─ 解出 TransferRequest
   ├─ 包成 Task{ weak_ptr<Connection>, req, seq }
   │            └─ 用 weak_ptr：client 可能在 worker 做完前就斷線
   │
   └─ queue.try_push(task)
         ├─ 成功 → 繼續切下一個 frame
         └─ 佇列滿 → 直接產生 BUSY 回應
                        絕不阻塞等待，否則整個 event loop 停擺
                            │
                            │  mutex + condvar 喚醒一個閒置 worker
                            ▼
② Worker #7 ─ queue.pop() 醒來
   │
   ├─ [驗證] amount > 0？ from ≠ to？ ccy 是合法幣別？
   │        └─ 任一不過 → 直接跳到 ⑤
   │
   ├─ [冪等-L1] IdempotencyCache.lookup("req-a3f9-01")
   │        └─ 命中 → 取出先前結果，跳到 ⑤（省下整趟 DB）
   │
   ├─ [查表] registry.lock_shared()
   │        ├─ a = find(1001), b = find(2002)
   │        └─ unlock_shared()  ← 立刻放掉，縮短 L0 臨界區
   │           成立前提：Account 位址穩定且永不 erase
   │
   ├─ [排序] auto [lo, hi] = minmax(1001, 2002) → lo=1001, hi=2002
   │
   ├─┌─ 臨界區開始 ─────────────────────────────────────┐
   │ │ unique_lock(lo->mtx)   ← 一律小 id 先，永不例外
   │ │ unique_lock(hi->mtx)
   │ │
   │ │ 檢查 lo.ccy == hi.ccy == USD    否 → CURRENCY_MISMATCH
   │ │ 檢查 a.balance(120000) >= 5000  否 → INSUFFICIENT_FUNDS
   │ │
   │ │┌─ Postgres 交易（worker 專屬連線）─────────────┐
   │ ││ BEGIN;
   │ ││ SELECT id, balance FROM accounts
   │ ││   WHERE id IN (1001, 2002)
   │ ││   ORDER BY id FOR UPDATE;  ← 順序必須與記憶體鎖一致
   │ ││
   │ ││ INSERT INTO transactions(id, idempotency_key, ...)
   │ ││   VALUES (900001, 'req-a3f9-01', ...);
   │ ││   └─ 若撞 UNIQUE → SQLSTATE 23505 → 冪等-L2 生效
   │ ││      回滾後改成查詢原交易，回傳原結果
   │ ││
   │ ││ INSERT INTO entries VALUES
   │ ││   (900001, 1001, 'USD', -5000, 115000),
   │ ││   (900001, 2002, 'USD', +5000,  47000);
   │ ││
   │ ││ UPDATE accounts SET balance = balance - 5000,
   │ ││        version = version + 1 WHERE id = 1001;
   │ ││ UPDATE accounts SET balance = balance + 5000,
   │ ││        version = version + 1 WHERE id = 2002;
   │ ││
   │ ││ COMMIT;   ← WAL fsync。交易在這一刻才真正存在
   │ │└──────────────────────────────────────────────┘
   │ │
   │ │ COMMIT 成功 → a.balance = 115000
   │ │               b.balance =  47000   （記憶體才跟上）
   │ │ COMMIT 失敗 → 記憶體完全不動，result = DB_ERROR
   │ │
   │└─ 臨界區結束（RAII 反向釋放：hi → lo）───────────┘
   │
   ├─ IdempotencyCache.insert("req-a3f9-01", result)
   │
   └─ ⑤ 產生 TransferResponse{ OK, tx_id:900001, new_balance:115000 }
                            │
                            ▼
③ Worker #7 ─ 交棒回 IO Thread（不自己寫 socket）
   │
   ├─ conn = task.weakConn.lock()
   │     └─ 已失效（client 斷線）→ 丟棄結果，取下一個 task
   │
   ├─ conn->queueWrite(resp)      // 小 mutex 保護 outbound buffer
   ├─ loop->addPendingWrite(conn)
   └─ eventfd_write(loop->wakeupFd_, 1)   ← 敲響 event loop
                            │
                            ▼
④ IO Thread ─ epoll_wait 因 wakeupFd 可讀而返回
   │
   ├─ read(wakeupFd_) 清掉計數
   ├─ 排空 pendingWrites_ 清單
   │
   └─ 對每條連線 write(fd, out, n)
         ├─ 全部寫完 → 結束
         └─ EAGAIN（kernel 送出緩衝滿）
              ├─ 剩餘位元組留在 outbound buffer
              ├─ 註冊 EPOLLOUT
              └─ 下次 EPOLLOUT 續寫；寫完取消 EPOLLOUT
                            │
                            ▼
╔══ 客戶端 ══════════════════════════════════════════════════╗
   收到 [len][RESP_OK][tx_id:900001][balance:115000]
╚═══════════════════════════════════════════════════════════╝
```

### 路徑上五個容易寫錯的地方

| 位置 | 陷阱 | 後果 |
|---|---|---|
| ① | ET 模式沒讀到 `EAGAIN` 就 return | 剩餘資料不再觸發事件，該連線**永久靜默**。最難 debug 的 epoll bug，只在資料量大時出現 |
| ① | 佇列滿時阻塞等待 | event loop 停擺，**所有**連線一起死 |
| ② | `from == to` 沒在取鎖前擋掉 | 同一把 mutex 鎖兩次，UB，自己卡死自己 |
| ② | `SELECT FOR UPDATE` 沒加 `ORDER BY id` | Postgres row lock 死鎖（40P01），浪費往返並需重試 |
| ③ | worker 直接 `write(fd)` | 兩個回應的位元組交織，協定崩潰。**TSan 抓不到**（不是 data race） |

---

## 7. Stage 0 驗收

沒有程式碼，所以驗收方式是「能不能不看文件回答這些問題」：

- **Q1** event loop 執行緒在一筆轉帳裡花多少時間？為什麼 DB 呼叫不能放在它裡面？
- **Q2** A→B 與 B→A 同時發生，逐步描述兩執行緒的取鎖順序，並說明為什麼等待圖無法成環。
- **Q3** 為什麼 `balance` 不能用 `std::atomic<int64_t>`？舉一個具體數字例子。
- **Q4** worker 完成後為什麼不能自己寫 socket？寫了的話 client 會看到什麼？
- **Q5** DB COMMIT 成功但回應送出前崩潰，client 重送同一筆會發生什麼？擋住它的是哪一層？
- **Q6** `int64 amount = 5000` 在 USD 和 JPY 分別是多少錢？程式碼在哪裡查到這個差別？
- **Q7** 為什麼 registry 的 `shared_lock` 可以在取得 `Account*` 後就放掉？前提被違反會怎樣？

---

## 8. Stage 1 完成紀錄

Stage 1 的產出已建置並驗證通過：

```
$ make test
100% tests passed, 0 tests failed out of 3

$ ./build/src/ledger_engine --version
ledger_engine 0.1.0 | RelWithDebInfo | gcc 13.3.0 | sanitizer: none

$ make tsan
ledger_engine 0.1.0 | RelWithDebInfo | gcc 13.3.0 | sanitizer: TSan
100% tests passed, 0 tests failed out of 3
```

已打通的路徑：CMake configure → 編譯（含 `-Wconversion` 等嚴格警告，零 warning）→ 連結 → 執行 → ctest → TSan/ASan 建置。

---

## 9. Stage 2 完成紀錄

Schema 已實作並在真的 Postgres 16 上驗證通過。

```
$ make db-all
✓ Stage 2 全部通過：schema 重建、種子載入、22 項約束測試、5 條不變式
```

### 相對於第 4 節設計的三處強化

第 4 節的 schema 是概念草稿，實作時把三件原本要靠應用層記得做的事，
下推成資料庫的結構性保證：

**① 「兩腳幣別必須相同」改用複合外鍵強制。**
在 `accounts` 加一條看似多餘的 `UNIQUE (id, currency)`，
`transactions` 與 `entries` 就能建立 `FOREIGN KEY (account_id, currency)`。
效果是跨幣別轉帳的資料**在結構上寫不進去**——因為
`(1001, 'JPY')` 這個組合在 `accounts` 裡根本不存在。
不需要應用層檢查，也不需要額外觸發器。

**② 「兩腳相加為零」用延後約束觸發器強制。**
`CHECK` 只能看同一列，表達不了跨列規則。而兩筆分錄是分兩次 INSERT 的，
立即檢查會在只有一筆時就爆掉——那是正常的中間狀態。
所以用 `CONSTRAINT TRIGGER ... DEFERRABLE INITIALLY DEFERRED`，
把檢查延到 `COMMIT` 那一刻。**資料庫會拒絕提交一筆不平的交易。**

**③ 「entries append-only」用觸發器強制。**
`BEFORE UPDATE` 與 `BEFORE DELETE` 觸發器直接拋例外。
稽核軌跡的定義就是寫下去不能改，這條讓「改歷史」在資料庫層不可能。

### 一處設計修正：transaction id 改由 Postgres 產生

第 3 節原本寫「全域 txn id 產生器用 `atomic fetch_add`」。
那對純記憶體的 `Journal` 內部序號是對的，但**不能拿來當持久化的
`transactions.id`**：行程內的計數器重啟後會歸零、多實例部署會互撞。

改為 `GENERATED BY DEFAULT AS IDENTITY`（背後是 Postgres sequence），
跨重啟、跨連線、跨實例都保證唯一。記憶體的 atomic counter 保留給
Journal 序號與 metrics 使用。

### 驗證涵蓋範圍

`db/checks/constraint_tests.sql` 對每一條約束都寫入違規資料並確認被拒絕，
共 22 項，涵蓋負餘額、不平衡交易、單邊記帳、同帳戶兩腳、跨幣別、
重複冪等鍵、自己轉自己、竄改分錄等。

`db/checks/invariants.sql` 檢查現有資料的 I1–I5。兩者問的是不同問題：
前者問「壞資料寫得進去嗎」（測 schema），後者問「現在的資料是對的嗎」（測資料）。
**兩個都需要**——因為 lost update 寫進去的每一列單獨看都完全合法，
它壞在跨表的關係上，只有 I2 主動比對餘額快照與分錄總和才看得出來。

I2 的偵測能力已實測驗證：手動製造一筆「改了餘額但沒有對應分錄」的
lost update，I2 立刻報出 drift = -3000，I3 也報出該幣別總量不為零。

**Stage 3 的下一步**：實作純記憶體的 `LedgerCore`——`Account`、
`AccountRegistry`、排序取鎖的 `transfer()`，先不接 DB，
用 GoogleTest 驗 I1–I5，其中 I3（32 執行緒隨機轉帳後總量守恆）是重頭戲。
