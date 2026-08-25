#pragma once

#include <ledger/common/Types.h>
#include <ledger/money/Currency.h>

#include <atomic>
#include <shared_mutex>

namespace ledger {

// ---------------------------------------------------------------------------
// Account —— 一個帳戶，以及保護它的鎖。
//
// 鎖粒度就是「一個帳戶一把鎖」。這是整個併發設計的基礎：
// 兩筆完全不相干的轉帳（1001→2002 和 3003→4004）彼此完全不阻塞。
//
// ★ 為什麼 mutex 是 public
//   取鎖順序是「跨帳戶」的決策 —— 必須同時看到兩個帳戶才知道誰先誰後。
//   Account 自己不可能知道，只有 LedgerCore 知道。所以鎖必須讓外部拿得到。
//   代價是要靠命名紀律：所有名字以 Locked 結尾的方法，
//   呼叫端「必須」已經持有這把鎖。
//
// ★ 為什麼不可複製也不可移動
//   AccountRegistry 會把 Account* 交出去，而呼叫端可能在放掉 registry 鎖之後
//   才拿這個指標去取帳戶鎖。這個指標要一直有效，物件就絕對不能搬家。
//   把複製與移動刪掉，是用型別系統把這個前提釘死，而不是寫在註解裡祈禱。
//   （std::shared_mutex 本身也不可移動，所以這也是必然。）
// ---------------------------------------------------------------------------
class Account {
 public:
  Account(AccountId id, OwnerId owner, Currency ccy, Amount initialBalance, bool allowNegative)
      : id_(id),
        owner_(owner),
        ccy_(ccy),
        allowNegative_(allowNegative),
        initialBalance_(initialBalance),
        balance_(initialBalance) {}

  Account(const Account&) = delete;
  Account& operator=(const Account&) = delete;
  Account(Account&&) = delete;
  Account& operator=(Account&&) = delete;

  // --- 建立後就不再改變的欄位：讀取不需要鎖 -------------------------------
  [[nodiscard]] AccountId id() const noexcept { return id_; }
  [[nodiscard]] OwnerId owner() const noexcept { return owner_; }
  [[nodiscard]] Currency currency() const noexcept { return ccy_; }
  [[nodiscard]] bool allowNegative() const noexcept { return allowNegative_; }

  /// 開戶時的餘額。
  ///
  /// 不變式 I2 需要它：帳戶如果是帶著初始餘額開出來的，那筆錢沒有對應的分錄，
  /// 所以比對時要寫成
  ///     balance == initialBalance + SUM(entries)
  /// 而不是 balance == SUM(entries)。
  ///
  /// 正規做法其實是「所有帳戶都從 0 開始，資金一律由系統帳戶轉入」
  /// —— db/seeds/dev_seed.sql 就是這樣做的。這裡保留初始餘額只是為了讓
  /// 單元測試好寫，不必為了測一筆轉帳先鋪三筆轉帳。
  [[nodiscard]] Amount initialBalance() const noexcept { return initialBalance_; }

  /// 保護下面那些可變狀態的鎖。
  /// 轉帳取 unique_lock（排他），餘額查詢取 shared_lock（可並行）。
  mutable std::shared_mutex mutex;

  // --- 以下方法要求呼叫端「已經持有 mutex」--------------------------------

  [[nodiscard]] Amount balanceLocked() const noexcept { return balance_; }
  [[nodiscard]] std::uint64_t versionLocked() const noexcept { return version_; }
  [[nodiscard]] bool isClosedLocked() const noexcept { return closed_; }

  /// 檢查扣掉 amount 之後餘額是否仍然合法。
  /// 不改變任何狀態 —— 呼叫端要在真的動手之前先問這個。
  [[nodiscard]] bool canDebitLocked(Amount amount) const noexcept {
    if (allowNegative_) {
      return true;  // 系統帳戶：錢由此流入系統，允許負餘額
    }
    return balance_ >= amount;
  }

  /// 套用一筆變動（可正可負）。呼叫端必須先確認過不會溢位、不會違反非負。
  void applyLocked(Amount delta) noexcept {
    balance_ += delta;
    ++version_;
  }

  void setClosedLocked(bool closed) noexcept { closed_ = closed; }

  // --- 便利方法：自己取 shared_lock ---------------------------------------

  /// 單一帳戶的餘額快照。
  ///
  /// 注意這只保證「這一個帳戶」的值是完整的。
  /// 想要「多個帳戶同時一致」的快照，必須走 LedgerCore 的對帳鎖 —— 見 LedgerCore.h。
  [[nodiscard]] Amount balance() const {
    std::shared_lock lock(mutex);
    return balance_;
  }

 private:
  const AccountId id_;
  const OwnerId owner_;
  const Currency ccy_;
  const bool allowNegative_;
  const Amount initialBalance_;

  // 以下由 mutex 保護
  Amount balance_ = 0;
  std::uint64_t version_ = 0;
  bool closed_ = false;
};

}  // namespace ledger
