#pragma once

#include <ledger/common/Result.h>
#include <ledger/common/Types.h>
#include <ledger/core/AccountRegistry.h>
#include <ledger/core/Journal.h>
#include <ledger/money/Currency.h>

#include <atomic>
#include <shared_mutex>
#include <string>

namespace ledger {

struct TransferRequest {
  std::string idempotencyKey;  ///< Stage 3 只忠實記錄，Stage 5 才用它去重
  AccountId from;
  AccountId to;
  Amount amount;  ///< 必須為正
  Currency currency;
};

struct TransferResult {
  TxId txId;
  Amount fromBalance;  ///< 轉帳後的餘額
  Amount toBalance;
};

/// 一次對帳快照。所有帳戶在「同一時刻」的狀態。
struct AuditSnapshot {
  Amount totalPerCurrency[kCurrencyCount]{};
  std::size_t accountCount = 0;
};

// ---------------------------------------------------------------------------
// LedgerCore —— 全系統唯一知道「怎麼轉帳」的地方。
//
// 這是整個專案正確性的集中點。想檢查有沒有死鎖、有沒有 race，
// 只需要讀這一個檔案。net 層完全不知道什麼是餘額，db 層完全不知道什麼是鎖順序。
//
// ===========================================================================
// 鎖階層（只能由上往下取，永不反向）
// ===========================================================================
//
//   L0  auditMutex_    轉帳取 shared，對帳取 unique   ← 反直覺，見下方說明
//   L1  registry 鎖    查找時取 shared，取得指標後立刻放掉
//   L2  帳戶鎖         unique，「一律 account_id 遞增順序」★
//   L3  journal 鎖     unique，在 L2 放掉之後才取
//
// ★ L2 那一行就是防死鎖的全部。
//
// ---------------------------------------------------------------------------
// 為什麼排序取鎖能保證不死鎖
// ---------------------------------------------------------------------------
// 死鎖需要四個條件同時成立（Coffman 條件）：互斥、持有並等待、
// 不可搶佔、循環等待。前三個是 mutex 的本質，改不掉。我們打破第四個。
//
//   天真做法「先鎖來源再鎖目的」：
//     T1 轉 1001→2002：持有 1001，等 2002
//     T2 轉 2002→1001：持有 2002，等 1001
//     → 環 → 兩個 worker 永遠不會回來
//
//   排序法「一律先鎖 id 較小的」：
//     T1 和 T2 都是先 1001 再 2002
//     T2 被擋在 1001 時，手上「沒有持有任何鎖」
//     → 它不可能成為別人的阻礙者 → 環無法形成
//
// 形式化：對帳戶定義全序（id 遞增），規定每個執行緒的取鎖序列必為遞增。
// 若存在環 T₁→T₂→…→Tₙ→T₁，沿環走一圈 id 必須嚴格遞增，
// 但最後又要回到起點 —— 矛盾。這是證明，不是經驗法則。
//
// ---------------------------------------------------------------------------
// auditMutex_ 的反直覺用法
// ---------------------------------------------------------------------------
// 一般人看到 shared_mutex 會想「讀取用 shared」。這裡剛好相反：
//
//   transfer()  取 shared_lock  ← 「寫入」資料的操作
//   audit()     取 unique_lock  ← 「讀取」資料的操作
//
// 因為這把鎖保護的不是某個變數，而是「有沒有交易正在進行中」這件事。
//   - 轉帳彼此之間可以並行（它們靠帳戶鎖互斥），所以都拿 shared —— 零成本
//   - 對帳需要全系統靜止才能取得一致快照，所以拿 unique
//
// 沒有這把鎖的話，對帳會逐一鎖 A 讀、放開、鎖 B 讀，中途可能讀到
// A 的新餘額配 B 的舊餘額，於是誤報「帳目不平」—— 明明系統是對的。
// ---------------------------------------------------------------------------
class LedgerCore {
 public:
  LedgerCore(AccountRegistry& registry, Journal& journal) noexcept
      : registry_(registry), journal_(journal) {}

  LedgerCore(const LedgerCore&) = delete;
  LedgerCore& operator=(const LedgerCore&) = delete;

  /// 執行一筆轉帳。這是整個引擎的核心操作。
  ///
  /// 全有或全無：要嘛兩個帳戶的餘額與兩筆分錄都生效，
  /// 要嘛什麼都沒發生並回傳錯誤碼。中間狀態不會被任何執行緒觀察到。
  Result<TransferResult> transfer(const TransferRequest& req);

  /// 取得全系統的一致快照。會暫停所有轉帳。
  [[nodiscard]] AuditSnapshot audit() const;

  /// 某幣別的總餘額。守恆定律說它必須恆等於初始值。
  [[nodiscard]] Amount totalBalance(Currency ccy) const;

  /// 驗證 I1（交易平衡）與 I2（餘額可從分錄重算）。
  /// 這是 Stage 7 判定有沒有 race condition 的裁判。
  [[nodiscard]] bool verifyInvariants() const;

  [[nodiscard]] std::uint64_t transferCount() const noexcept {
    return completed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t rejectedCount() const noexcept {
    return rejected_.load(std::memory_order_relaxed);
  }

 private:
  AccountRegistry& registry_;
  Journal& journal_;

  /// 見上方「反直覺用法」說明。
  mutable std::shared_mutex auditMutex_;

  /// 記憶體中的交易序號。relaxed 就夠：只需要唯一遞增，
  /// 不需要跟其他資料建立順序關係。
  std::atomic<TxId> nextTxId_{900000};

  /// 統計。relaxed —— 只要最終計數正確即可。
  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace ledger
