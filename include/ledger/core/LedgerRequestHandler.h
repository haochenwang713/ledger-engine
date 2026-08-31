#pragma once

#include <ledger/concurrent/ThreadPool.h>
#include <ledger/core/AccountRegistry.h>
#include <ledger/core/LedgerCore.h>
#include <ledger/proto/Messages.h>

namespace ledger {

// ---------------------------------------------------------------------------
// LedgerRequestHandler —— 協定與帳本之間的翻譯層。
//
// 這是整個系統唯一同時認識 proto:: 和 core:: 的地方。往上，net 層只看得到
// RequestEnvelope；往下，LedgerCore 只看得到 TransferRequest。
//
// ★ 為什麼連純讀請求也走 worker
//
//   GET_ACCOUNT 只需要一次 shared_lock 加一次讀取，大約 50 奈秒 ——
//   丟進佇列反而多了一次排隊與兩次執行緒切換，單看延遲是虧的。
//
//   但一致性更值錢：只有一條路徑，就只有一組併發語意要推理。
//   而且 Stage 8 量測時不會出現「有些請求走了捷徑」的雜訊，
//   TPS 與 p95 的數字才是可比較的。
//
//   這個決定隨時可以反悔 —— 把 GET_ACCOUNT 攔在 ConnectionContext
//   直接回應即可，不影響其他任何一層。
//
// ⚠ 執行緒模型：每條 worker 執行緒擁有自己的一份 LedgerRequestHandler
//   （由 ThreadPool 的工廠在該執行緒上建立），所以這個類別本身
//   不需要是執行緒安全的。它共享的 LedgerCore 與 AccountRegistry
//   自己有鎖。
//
//   Stage 6 的版本會多持有一條 pqxx::connection —— 正是因為
//   handler 是每個 worker 一份，那條連線才不需要任何保護。
// ---------------------------------------------------------------------------
class LedgerRequestHandler : public concurrent::RequestHandler {
 public:
  LedgerRequestHandler(LedgerCore& core, AccountRegistry& registry) noexcept
      : core_(core), registry_(registry) {}

  [[nodiscard]] proto::ResponseEnvelope handle(const proto::RequestEnvelope& env) override;

 private:
  [[nodiscard]] proto::ResponseEnvelope onTransfer(std::uint32_t reqId,
                                                   const proto::TransferReq& req);
  [[nodiscard]] proto::ResponseEnvelope onGetAccount(std::uint32_t reqId,
                                                     const proto::GetAccountReq& req);

  LedgerCore& core_;
  AccountRegistry& registry_;
};

/// 建立 ThreadPool 用的工廠。
///
/// 回傳的 lambda 會在「每一條 worker 執行緒上」各被呼叫一次，
/// 所以每個 worker 拿到的是自己的 handler 實例。
[[nodiscard]] concurrent::HandlerFactory makeLedgerHandlerFactory(LedgerCore& core,
                                                                  AccountRegistry& registry);

/// 載入示範帳戶。
///
/// Stage 6 接上 PostgreSQL 之後這個函式會被 "SELECT id, balance FROM accounts"
/// 取代。在那之前，它讓伺服器一啟動就有東西可以轉帳。
///
/// 資料刻意與 db/seeds/dev_seed.sql 完全一致（Alice 1001 = 115000、
/// Bob 2002 = 47000），這樣 Stage 6 接上 DB 之後，同一組 nc 指令
/// 應該產生一模一樣的回應 —— 那本身就是一條驗證。
///
/// ★ 錢不會憑空出現：所有餘額都來自系統帳戶 9001 的轉出，
///   所以 I1（每筆交易兩腳相加為零）從第一筆資料就成立，
///   而 9001 的負餘額絕對值就是系統中流通的錢總量。
Status seedDemoAccounts(LedgerCore& core, AccountRegistry& registry);

}  // namespace ledger
