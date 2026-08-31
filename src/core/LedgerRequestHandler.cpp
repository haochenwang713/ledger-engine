#include <ledger/core/Account.h>
#include <ledger/core/LedgerRequestHandler.h>

#include <memory>
#include <shared_mutex>
#include <string>
#include <variant>

namespace ledger {

namespace {

/// ErrorCode → 給 client 看的訊息。
///
/// 刻意只回錯誤碼的名字，不回內部細節。「INSUFFICIENT_FUNDS」足夠讓
/// client 決定怎麼辦；「帳戶 1001 餘額 4200 少於 5000」則洩漏了
/// 呼叫者未必有權知道的資訊。這在真實的金融系統是硬性要求。
[[nodiscard]] std::string describe(ErrorCode code) {
  return std::string(toString(code));
}

}  // namespace

proto::ResponseEnvelope LedgerRequestHandler::handle(const proto::RequestEnvelope& env) {
  // std::visit 讓「新增一種請求」變成編譯期檢查的事 ——
  // 少處理一個 variant 分支會編譯失敗，而不是執行期靜靜地掉進 default。
  return std::visit(
      [&](const auto& msg) -> proto::ResponseEnvelope {
        using T = std::decay_t<decltype(msg)>;

        if constexpr (std::is_same_v<T, proto::PingReq>) {
          return proto::ResponseEnvelope{env.reqId, proto::PongResp{}};
        } else if constexpr (std::is_same_v<T, proto::TransferReq>) {
          return onTransfer(env.reqId, msg);
        } else if constexpr (std::is_same_v<T, proto::GetAccountReq>) {
          return onGetAccount(env.reqId, msg);
        } else {
          static_assert(proto::kAlwaysFalse<T>, "有請求型別沒有被處理");
        }
      },
      env.body);
}

proto::ResponseEnvelope LedgerRequestHandler::onTransfer(std::uint32_t reqId,
                                                         const proto::TransferReq& req) {
  // 協定型別 → 核心型別。這一行就是這個類別存在的理由：
  // LedgerCore 永遠不需要知道什麼是 reqId、什麼是 wire format。
  const TransferRequest coreReq{req.idemKey, req.from, req.to, req.amount, req.ccy};

  const Result<TransferResult> result = core_.transfer(coreReq);
  if (!result) {
    return proto::makeError(reqId, result.error(), describe(result.error()));
  }

  return proto::ResponseEnvelope{
      reqId,
      proto::TransferOkResp{
          result.value().txId, result.value().fromBalance, result.value().toBalance}};
}

proto::ResponseEnvelope LedgerRequestHandler::onGetAccount(std::uint32_t reqId,
                                                           const proto::GetAccountReq& req) {
  // ┌─ registry 臨界區開始（L1，shared）────────────────
  //   find() 內部取 registry 的 shared_lock，回傳後就放掉。
  //   指標之所以能安全地帶出臨界區，是因為三個前提同時成立：
  //     ① unordered_map 是 node-based，rehash 不搬節點
  //     ② 存的是 unique_ptr，Account 物件位址不變
  //     ③ 帳戶只新增、永不 erase（關戶只標記 closed）
  // └─ registry 臨界區結束 ─────────────────────────────
  Account* account = registry_.find(req.accountId);
  if (account == nullptr) {
    return proto::makeError(
        reqId, ErrorCode::AccountNotFound, describe(ErrorCode::AccountNotFound));
  }

  // ┌─ 帳戶臨界區開始（L2，shared）──────────────────────
  //   餘額與 closed 必須一起讀。分兩次讀的話，中間可能插入一筆
  //   轉帳，回報出「舊餘額 + 新狀態」這種從未真實存在過的組合。
  //
  //   只取一把鎖，所以沒有順序問題，也不可能死鎖。
  std::shared_lock lock(account->mutex);
  const proto::AccountResp resp{
      account->id(),
      account->balanceLocked(),
      account->currency(),
      account->isClosedLocked() ? proto::AccountStatus::Closed : proto::AccountStatus::Active};
  // └─ 帳戶臨界區結束（RAII，離開作用域時釋放）───────────

  return proto::ResponseEnvelope{reqId, resp};
}

concurrent::HandlerFactory makeLedgerHandlerFactory(LedgerCore& core, AccountRegistry& registry) {
  // 這個 lambda 會在每一條 worker 執行緒上各被呼叫一次。
  // 捕捉的是參照（core 與 registry 的生命週期比 pool 長），
  // 產生的是每個 worker 專屬的 handler 實例。
  return [&core, &registry](std::size_t /*workerIndex*/) {
    return std::make_unique<LedgerRequestHandler>(core, registry);
  };
}

Status seedDemoAccounts(LedgerCore& core, AccountRegistry& registry) {
  // --- 系統帳戶：錢從這裡流入系統，所以允許負餘額 ---
  const Result<Account*> usdSystem = registry.createWithId(9001,
                                                           /*owner=*/99,
                                                           Currency::USD,
                                                           /*initialBalance=*/0,
                                                           /*allowNegative=*/true);
  if (!usdSystem) {
    return usdSystem.error();
  }
  const Result<Account*> jpySystem =
      registry.createWithId(9002, /*owner=*/99, Currency::JPY, 0, /*allowNegative=*/true);
  if (!jpySystem) {
    return jpySystem.error();
  }

  // --- 使用者帳戶：一律從零開始 ---
  for (const auto& [id, owner, ccy] : {
           std::tuple{AccountId{1001}, OwnerId{1}, Currency::USD},  // Alice USD
           std::tuple{AccountId{2002}, OwnerId{2}, Currency::USD},  // Bob   USD
           std::tuple{AccountId{1003}, OwnerId{1}, Currency::JPY},  // Alice JPY
       }) {
    const Result<Account*> created = registry.createWithId(id, owner, ccy, 0, false);
    if (!created) {
      return created.error();
    }
  }

  // --- 用轉帳注入餘額，而不是直接設定 balance ---
  //
  // 這一步看起來繞路，但它是刻意的：直接寫 balance 會讓
  // I2（餘額必須等於該帳戶所有分錄的總和）從第一秒就不成立，
  // 於是 verifyInvariants() 這個最重要的裁判從一開始就是壞的。
  //
  // 走轉帳的話，每一分錢都有對應的分錄，不變式從第一筆資料就成立。
  // db/seeds/dev_seed.sql 用 dev_transfer() 而不是 UPDATE，是同一個理由。
  const TransferRequest seedTransfers[] = {
      {"seed-usd-alice", 9001, 1001, 120000, Currency::USD},
      {"seed-usd-bob", 9001, 2002, 42000, Currency::USD},
      {"seed-jpy-alice", 9002, 1003, 5000, Currency::JPY},  // ¥5,000（exponent=0）
      // 設計文件第 4 節的範例交易：Alice 轉 $50.00 給 Bob。
      // 跑完之後 1001 = 115000、2002 = 47000，與 Stage 2 的種子資料一致。
      {"req-a3f9-01", 1001, 2002, 5000, Currency::USD},
  };

  for (const TransferRequest& req : seedTransfers) {
    const Result<TransferResult> result = core.transfer(req);
    if (!result) {
      return result.error();
    }
  }

  return Status{};
}

}  // namespace ledger
