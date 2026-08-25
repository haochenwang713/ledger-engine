#include <ledger/core/LedgerCore.h>
#include <ledger/money/Money.h>

#include <algorithm>
#include <mutex>
#include <vector>

namespace ledger {

Result<TransferResult> LedgerCore::transfer(const TransferRequest& req) {
  // =========================================================================
  // 第一階段：不需要任何鎖就能做的驗證。
  // 先擋掉明顯不合法的請求，避免白白去搶鎖。
  // =========================================================================

  if (req.amount <= 0) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return ErrorCode::InvalidAmount;
  }

  // ★ 這一行必須在取鎖之前。
  //   若 from == to，下面 minmax 之後 lo 和 hi 會是同一個 Account*，
  //   於是同一把非遞迴 shared_mutex 被鎖兩次 —— 那是未定義行為，
  //   實務上就是執行緒把自己卡死，而且看起來像隨機 hang，極難 debug。
  if (req.from == req.to) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return ErrorCode::SelfTransfer;
  }

  // =========================================================================
  // 第二階段：L0 —— 對帳鎖，取 shared。
  //
  // 轉帳彼此之間都是 shared，所以互不阻塞（幾乎零成本）。
  // 只有 audit() 會取 unique，那時所有轉帳會一起等 —— 這正是我們要的。
  // =========================================================================
  std::shared_lock auditGuard(auditMutex_);

  // =========================================================================
  // 第三階段：L1 —— 查表拿到 Account*，然後立刻放掉 registry 鎖。
  //
  // find() 內部自己取 shared_lock 並在回傳前釋放。
  // 拿到的裸指標永遠有效，前提見 AccountRegistry.h 的三點說明。
  // =========================================================================
  Account* from = registry_.find(req.from);
  Account* to = registry_.find(req.to);

  if (from == nullptr || to == nullptr) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return ErrorCode::AccountNotFound;
  }

  // =========================================================================
  // 第四階段：排序。
  //
  // 這兩行是整個防死鎖設計的全部。
  // 不論請求是 1001→2002 還是 2002→1001，取鎖順序都是 1001 先、2002 後。
  // =========================================================================
  Account* lo = from;
  Account* hi = to;
  if (lo->id() > hi->id()) {
    std::swap(lo, hi);
  }

  // 這裡先算好，因為進臨界區之後要用。
  TxId txId = 0;
  Amount fromBalanceAfter = 0;
  Amount toBalanceAfter = 0;

  {
    // =======================================================================
    // 第五階段：L2 —— 臨界區開始。一律小 id 先。
    //
    // 從「檢查餘額」到「寫入餘額」全程持有這兩把鎖。
    // 任何想觀察這兩個帳戶的執行緒，至少要取得其中一把的 shared_lock，
    // 因此絕對看不到「from 已扣、to 未加」的中間狀態。
    // =======================================================================
    std::unique_lock loGuard(lo->mutex);
    std::unique_lock hiGuard(hi->mutex);

    // --- 幣別一致性 ---
    // 對應 Stage 2 schema 裡那組複合外鍵：跨幣別轉帳不該存在。
    if (from->currency() != to->currency()) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return ErrorCode::CurrencyMismatch;
    }
    if (from->currency() != req.currency) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return ErrorCode::CurrencyMismatch;
    }

    // --- 帳戶狀態 ---
    if (from->isClosedLocked() || to->isClosedLocked()) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return ErrorCode::AccountClosed;
    }

    // --- 餘額是否足夠 —— 這就是擋 double-spending 的那一行 ---
    //
    // 為什麼這個檢查必須跟下面的扣款在同一個臨界區裡：
    // 如果檢查完先放鎖再扣款，兩個執行緒可以都通過檢查（都看到餘額 5000），
    // 然後各扣 3000 —— 餘額變成 -1000。那就是超額扣款。
    if (!from->canDebitLocked(req.amount)) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return ErrorCode::InsufficientFunds;
    }

    // --- 溢位檢查 ---
    // 有號整數溢位在 C++ 是未定義行為，不是回繞。必須在做加減「之前」判斷，
    // 否則編譯器有權把事後檢查最佳化掉。
    if (subWouldOverflow(from->balanceLocked(), req.amount) ||
        addWouldOverflow(to->balanceLocked(), req.amount)) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return ErrorCode::AmountOverflow;
    }

    // --- 到這裡為止都沒有改動任何狀態。以下才是真正的動作 ---
    txId = nextTxId_.fetch_add(1, std::memory_order_relaxed);

    from->applyLocked(-req.amount);
    to->applyLocked(req.amount);

    fromBalanceAfter = from->balanceLocked();
    toBalanceAfter = to->balanceLocked();

    // =======================================================================
    // 臨界區結束。RAII 反向釋放：先 hiGuard 再 loGuard。
    //
    // 為什麼 journal 的寫入要放在外面：
    //   帳戶鎖是最熱的資源。臨界區裡每多做一件事，同一組帳戶的
    //   併發上限就低一分。journal 只是 append，不影響正確性，移出去。
    // =======================================================================
  }

  // =========================================================================
  // 第六階段：L3 —— 記錄事實。帳戶鎖已經放掉了。
  // =========================================================================
  const TransactionRecord record{
      txId,
      req.idempotencyKey,
      req.currency,
      req.amount,
      req.from,
      req.to,
  };
  const Entry debitEntry{txId, req.from, req.currency, -req.amount, fromBalanceAfter};
  const Entry creditEntry{txId, req.to, req.currency, req.amount, toBalanceAfter};

  journal_.append(record, debitEntry, creditEntry);

  completed_.fetch_add(1, std::memory_order_relaxed);

  return TransferResult{txId, fromBalanceAfter, toBalanceAfter};
}

AuditSnapshot LedgerCore::audit() const {
  // unique_lock：這一刻起，所有 transfer() 都會卡在它們的 shared_lock 上。
  // 系統靜止，我們才能安心逐一讀取每個帳戶而不會讀到不一致的組合。
  std::unique_lock auditGuard(auditMutex_);

  AuditSnapshot snapshot;
  const std::vector<Account*> accounts = registry_.allAccounts();
  snapshot.accountCount = accounts.size();

  for (Account* account : accounts) {
    // 仍然要取帳戶鎖 —— auditMutex_ 擋住的是「新的轉帳開始」，
    // 不保證此刻沒有轉帳正處於臨界區中間（它在我們之前就進去了）。
    std::shared_lock guard(account->mutex);
    const std::size_t idx = static_cast<std::size_t>(account->currency());
    snapshot.totalPerCurrency[idx] += account->balanceLocked();
  }

  return snapshot;
}

Amount LedgerCore::totalBalance(Currency ccy) const {
  const AuditSnapshot snapshot = audit();
  return snapshot.totalPerCurrency[static_cast<std::size_t>(ccy)];
}

bool LedgerCore::verifyInvariants() const {
  // 排他的對帳鎖。這一把是這個檢查能成立的關鍵：
  //
  // transfer() 的 auditGuard 是「函式層級」的 shared_lock —— 它一路持有到
  // journal_.append() 做完才釋放。所以只要我們拿得到 unique_lock，
  // 就代表沒有任何轉帳卡在「餘額已改、分錄還沒寫」的那個窗口裡。
  //
  // 少了這把鎖，這個檢查會偶發地誤報失敗，而且只在高負載下出現
  // —— 那是最糟的一種測試：它會讓你去找一個不存在的 bug。
  std::unique_lock auditGuard(auditMutex_);

  // I1：每筆交易恰好兩腳且相加為零
  if (!journal_.allTransactionsBalanced()) {
    return false;
  }

  // I2：每個帳戶的餘額必須等於它所有分錄的總和 ★ 抓 lost update
  for (Account* account : registry_.allAccounts()) {
    std::shared_lock guard(account->mutex);
    const Amount snapshot = account->balanceLocked();
    // 開戶時的初始餘額沒有對應分錄，所以要加回去。見 Account::initialBalance()。
    const Amount recomputed = account->initialBalance() + journal_.recomputeBalance(account->id());
    if (snapshot != recomputed) {
      return false;
    }
  }

  return true;
}

}  // namespace ledger
