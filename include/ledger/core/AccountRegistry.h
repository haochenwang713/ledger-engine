#pragma once

#include <ledger/common/Result.h>
#include <ledger/common/Types.h>
#include <ledger/core/Account.h>

#include <atomic>
#include <map>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace ledger {

// ---------------------------------------------------------------------------
// AccountRegistry —— 只做一件事：AccountId → Account* 的查找與新增。
//
// ★ 這把鎖保護的是「容器結構」，不是「帳戶內容」。
//   查找時取 shared_lock，拿到指標就「立刻放掉」，然後才去取帳戶自己的鎖。
//   兩把鎖幾乎不重疊，registry 這個全域瓶頸的持有時間被壓到最短。
//
// ★ 上面那件事成立，靠三個前提同時滿足：
//     1. unordered_map 是 node-based —— rehash 時節點不搬家
//     2. 裡面存的是 unique_ptr —— Account 物件的位址永遠不變
//     3. 帳戶「只新增、永不 erase」—— 關戶只是把 closed 標記設起來
//
//   ⚠ 如果有人日後加了 erase()，find() 回傳的指標就會變成迷途指標，
//     而且這種 use-after-free 只在高併發下偶爾出現，極難重現。
//     這三個前提是這份設計的地基，改動前請先讀懂它們。
//
// 為什麼是 shared_mutex 而不是 mutex：查找發生在每一筆轉帳上，開戶則罕見。
// 讀多寫少正是 shared_mutex 存在的理由 —— 20 個 worker 可以同時查找。
// ---------------------------------------------------------------------------
class AccountRegistry {
 public:
  AccountRegistry() = default;

  AccountRegistry(const AccountRegistry&) = delete;
  AccountRegistry& operator=(const AccountRegistry&) = delete;

  /// 開一個新帳戶。同一 owner 同一幣別只能有一個
  /// （對應 accounts 表的 UNIQUE (owner_id, currency)）。
  Result<Account*> create(OwnerId owner,
                          Currency ccy,
                          Amount initialBalance = 0,
                          bool allowNegative = false);

  /// 用指定的 id 開戶。測試與種子資料用，方便對照設計文件裡的例子。
  Result<Account*> createWithId(AccountId id,
                                OwnerId owner,
                                Currency ccy,
                                Amount initialBalance = 0,
                                bool allowNegative = false);

  /// 找帳戶。找不到回 nullptr。
  ///
  /// 回傳裸指標是刻意的：呼叫端拿到之後 registry 鎖就放掉了，
  /// 而上面三個前提保證這個指標永遠有效。
  [[nodiscard]] Account* find(AccountId id) const;

  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::vector<AccountId> allIds() const;

  /// 給對帳用：回傳所有 Account 指標。呼叫端負責自己處理鎖。
  [[nodiscard]] std::vector<Account*> allAccounts() const;

 private:
  mutable std::shared_mutex mutex_;

  // node-based + unique_ptr + 永不 erase ⇒ Account* 位址穩定
  std::unordered_map<AccountId, std::unique_ptr<Account>> accounts_;

  // (owner, currency) 的唯一性索引。用 std::map 是因為 std::pair 沒有現成的
  // std::hash，用有序容器省掉自訂 hash 的樣板碼；開戶不在熱路徑上，
  // O(log n) 完全可以接受。
  std::map<std::pair<OwnerId, Currency>, AccountId> ownerIndex_;

  // 下一個自動產生的 id。從 1000 起跳，讓測試可以安全使用 1–999 的固定 id。
  std::atomic<AccountId> nextId_{1000};
};

}  // namespace ledger
