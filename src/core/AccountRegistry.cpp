#include <ledger/core/AccountRegistry.h>

#include <mutex>
#include <utility>

namespace ledger {

Result<Account*> AccountRegistry::create(OwnerId owner,
                                         Currency ccy,
                                         Amount initialBalance,
                                         bool allowNegative) {
  // fetch_add 保證多執行緒同時開戶也不會拿到重複的 id。
  // relaxed 就夠了：我們只需要「唯一」，不需要 id 的順序跟其他資料
  // 之間建立 happens-before 關係。
  const AccountId id = nextId_.fetch_add(1, std::memory_order_relaxed);
  return createWithId(id, owner, ccy, initialBalance, allowNegative);
}

Result<Account*> AccountRegistry::createWithId(
    AccountId id, OwnerId owner, Currency ccy, Amount initialBalance, bool allowNegative) {
  // 開戶要改容器結構，所以是排他鎖。這是 registry 唯一會用 unique_lock 的地方。
  std::unique_lock lock(mutex_);

  if (accounts_.find(id) != accounts_.end()) {
    return ErrorCode::DuplicateAccount;
  }

  const auto key = std::make_pair(owner, ccy);
  if (ownerIndex_.find(key) != ownerIndex_.end()) {
    return ErrorCode::DuplicateAccount;
  }

  auto account = std::make_unique<Account>(id, owner, ccy, initialBalance, allowNegative);
  Account* raw = account.get();

  accounts_.emplace(id, std::move(account));
  ownerIndex_.emplace(key, id);

  // 把自動編號的水位推到這個 id 之後。
  //
  // 為什麼需要：測試與種子資料會用明確的 id（1001、2002）開戶，
  // 但那不會推進 nextId_。少了這幾行，之後 create() 產生的 id
  // 遲早會撞上已經存在的帳戶，開戶就開始莫名其妙失敗。
  //
  // 這跟 db/seeds/dev_seed.sql 裡那句 setval() 是同一件事 ——
  // 明確指定主鍵之後，序列必須跟著往前推。
  //
  // 用 CAS 迴圈而不是直接 store：可能有別的執行緒同時在 fetch_add，
  // 直接覆寫會把它剛取走的號碼再發一次。
  AccountId watermark = nextId_.load(std::memory_order_relaxed);
  while (watermark <= id &&
         !nextId_.compare_exchange_weak(watermark, id + 1, std::memory_order_relaxed)) {
    // compare_exchange_weak 失敗時會把現值寫回 watermark，迴圈條件重新判斷
  }

  return raw;
}

Account* AccountRegistry::find(AccountId id) const {
  // 共享鎖：多個 worker 可以同時查找，彼此不阻塞。
  std::shared_lock lock(mutex_);

  const auto it = accounts_.find(id);
  if (it == accounts_.end()) {
    return nullptr;
  }

  // 回傳裸指標，函式結束時鎖就放掉了。
  // 這是安全的 —— 前提見 AccountRegistry.h 的三點說明。
  return it->second.get();
}

std::size_t AccountRegistry::size() const {
  std::shared_lock lock(mutex_);
  return accounts_.size();
}

std::vector<AccountId> AccountRegistry::allIds() const {
  std::shared_lock lock(mutex_);
  std::vector<AccountId> ids;
  ids.reserve(accounts_.size());
  for (const auto& [id, account] : accounts_) {
    ids.push_back(id);
  }
  return ids;
}

std::vector<Account*> AccountRegistry::allAccounts() const {
  std::shared_lock lock(mutex_);
  std::vector<Account*> out;
  out.reserve(accounts_.size());
  for (const auto& [id, account] : accounts_) {
    out.push_back(account.get());
  }
  return out;
}

}  // namespace ledger
