#include <ledger/core/Journal.h>

#include <mutex>
#include <unordered_map>

namespace ledger {

void Journal::append(const TransactionRecord& tx, const Entry& debit, const Entry& credit) {
  std::unique_lock lock(mutex_);
  transactions_.push_back(tx);
  entries_.push_back(debit);
  entries_.push_back(credit);
  // 三筆一起在同一個臨界區寫入 —— 任何讀者要嘛看到全部，要嘛一筆都看不到。
  // 若分開三次上鎖，對帳就可能撞見「交易在了但只有一腳分錄」的中間狀態。
}

std::size_t Journal::transactionCount() const {
  std::shared_lock lock(mutex_);
  return transactions_.size();
}

std::size_t Journal::entryCount() const {
  std::shared_lock lock(mutex_);
  return entries_.size();
}

std::vector<Entry> Journal::entriesFor(AccountId id) const {
  std::shared_lock lock(mutex_);
  std::vector<Entry> out;
  for (const Entry& e : entries_) {
    if (e.accountId == id) {
      out.push_back(e);
    }
  }
  return out;
}

Amount Journal::recomputeBalance(AccountId id) const {
  std::shared_lock lock(mutex_);
  Amount sum = 0;
  for (const Entry& e : entries_) {
    if (e.accountId == id) {
      sum += e.amount;
    }
  }
  return sum;
}

bool Journal::allTransactionsBalanced() const {
  std::shared_lock lock(mutex_);

  // txId → {腳數, 金額總和}
  std::unordered_map<TxId, std::pair<int, Amount>> byTx;
  for (const Entry& e : entries_) {
    auto& slot = byTx[e.txId];
    slot.first += 1;
    slot.second += e.amount;
  }

  for (const auto& [txId, agg] : byTx) {
    const auto& [legCount, legSum] = agg;
    if (legCount != 2 || legSum != 0) {
      return false;
    }
  }
  return true;
}

Amount Journal::totalEntryAmount(Currency ccy) const {
  std::shared_lock lock(mutex_);
  Amount sum = 0;
  for (const Entry& e : entries_) {
    if (e.currency == ccy) {
      sum += e.amount;
    }
  }
  return sum;
}

std::vector<TransactionRecord> Journal::transactions() const {
  std::shared_lock lock(mutex_);
  return transactions_;
}

}  // namespace ledger
