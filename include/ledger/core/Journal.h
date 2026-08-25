#pragma once

#include <ledger/common/Types.h>
#include <ledger/money/Currency.h>

#include <cstddef>
#include <shared_mutex>
#include <string>
#include <vector>

namespace ledger {

/// 一筆分錄。負 = 借方（錢出去），正 = 貸方（錢進來）。
/// 對應 db/migrations/004_entries.sql 的 entries 表。
struct Entry {
  TxId txId;
  AccountId accountId;
  Currency currency;
  Amount amount;        ///< 負=借 正=貸，永不為 0
  Amount balanceAfter;  ///< 寫入本筆後該帳戶的餘額（稽核用）
};

/// 交易標頭。對應 transactions 表。
struct TransactionRecord {
  TxId id;
  std::string idempotencyKey;  ///< Stage 5 才會用它來去重；這裡先忠實記錄
  Currency currency;
  Amount amount;  ///< 恆為正
  AccountId debitAccountId;
  AccountId creditAccountId;
};

// ---------------------------------------------------------------------------
// Journal —— 記憶體中的 append-only 事實紀錄。
//
// 核心觀念：entry 是不可變的事實，account.balance 是可變的快照。
//   事實永不改寫；快照隨時可以從事實重算。
//   兩者對不上 = 發生了 lost update。
//
// 這個「可以重算」的性質不是裝飾 —— 它是 Stage 7 併發測試唯一能證明
// 「沒有 race condition」的辦法。recomputeBalance() 就是那個裁判。
//
// 為什麼只用一把普通 shared_mutex 而不像帳戶那樣分片：
//   append 是純寫入且極短（兩次 push_back）。真正的爭用點是帳戶鎖，
//   而 journal 的寫入發生在帳戶鎖「已經放掉之後」，不會延長臨界區。
//   Stage 8 若量測發現這裡成為瓶頸，再改成每執行緒一個 buffer 定期合併。
// ---------------------------------------------------------------------------
class Journal {
 public:
  Journal() = default;

  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;

  /// 記錄一筆交易與它的兩腳分錄。三者一起寫入，中途不會被別人看到一半。
  void append(const TransactionRecord& tx, const Entry& debit, const Entry& credit);

  [[nodiscard]] std::size_t transactionCount() const;
  [[nodiscard]] std::size_t entryCount() const;

  /// 某帳戶的所有分錄，依寫入順序。
  [[nodiscard]] std::vector<Entry> entriesFor(AccountId id) const;

  /// ★ 不變式 I2 的核心：從分錄重算某帳戶的餘額。
  ///   結果必須恆等於 account.balance()。不相等就是 lost update。
  [[nodiscard]] Amount recomputeBalance(AccountId id) const;

  /// 不變式 I1：每筆交易恰好兩腳且相加為零。
  [[nodiscard]] bool allTransactionsBalanced() const;

  /// 某幣別所有分錄的總和。因為每筆交易都相加為零，這個值必須恆為 0。
  [[nodiscard]] Amount totalEntryAmount(Currency ccy) const;

  [[nodiscard]] std::vector<TransactionRecord> transactions() const;

 private:
  mutable std::shared_mutex mutex_;
  std::vector<TransactionRecord> transactions_;
  std::vector<Entry> entries_;
};

}  // namespace ledger
