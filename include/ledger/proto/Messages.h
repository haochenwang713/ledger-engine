#pragma once

#include <ledger/common/Result.h>
#include <ledger/common/Types.h>
#include <ledger/money/Currency.h>
#include <ledger/proto/Field.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// 協定的訊息型別。
//
// 這裡定義的是「中立值型別」—— 它們不知道 binary，也不知道 JSON。
// BinaryCodec 和 JsonCodec 是翻譯機，LedgerCore 和 worker 只看得到這些型別。
//
// 好處在 Stage 6 才會完全顯現：接上 DB 之後業務邏輯只有一份，
// 不會因為多支援一種編碼而多一倍的整合測試。
// ---------------------------------------------------------------------------

/// 協定版本。不相符的請求直接回 UnsupportedVersion 並關連線。
///
/// 為什麼要現在就決定語意：Stage 0 定了 ver 欄位卻沒說不合怎麼辦，
/// 那它就會變成一個永遠是 1 的死欄位。定成「不合就拒絕」只要兩行，
/// 但讓這個欄位真的有作用。
inline constexpr std::uint16_t kProtocolVersion = 1;

/// 單則訊息的位元組上限。
///
/// NDJSON 沒有長度前綴，所以更需要這個 —— 沒有上限的話，
/// 一個永遠不送換行的 client 可以讓 Buffer 無限成長直到 OOM。
inline constexpr std::size_t kMaxFrameSize = 64 * 1024;

/// 冪等鍵的長度上限。binary 用 u16 存長度，這裡定得更嚴。
inline constexpr std::size_t kMaxIdemKeyLength = 128;

// ---------------------------------------------------------------------------
// 訊息代碼。最高位元 = 1 表示這是回應。
//
// 數值一旦發布就不能改 —— client 和 Locust 腳本都會依賴它。
// 新增訊息一律往後加，永不重用舊值。
// ---------------------------------------------------------------------------
enum class MsgType : std::uint16_t {
  // --- 請求 ---
  Ping = 0x0001,
  Transfer = 0x0002,
  GetAccount = 0x0003,

  // --- 回應 ---
  Pong = 0x8001,
  TransferOk = 0x8002,
  Account = 0x8003,
  Error = 0x80FF,
};

[[nodiscard]] constexpr bool isResponseType(MsgType t) noexcept {
  return (static_cast<std::uint16_t>(t) & 0x8000U) != 0;
}

/// 帳戶狀態。與 db/migrations/002_accounts.sql 的 status 欄位對應。
enum class AccountStatus : std::uint8_t {
  Active = 0,
  Closed = 1,
};

// ---------------------------------------------------------------------------
// MsgType ↔ 名字。JSON 的 "type" 欄位用這張表。
//
// 刻意做成一張陣列而不是兩個 switch —— 雙向查找共用同一份資料，
// 不可能出現「encode 用新名字、decode 只認得舊名字」的漂移。
// ---------------------------------------------------------------------------
struct MsgTypeName {
  MsgType type;
  std::string_view name;
};

inline constexpr MsgTypeName kMsgTypeNames[] = {
    {MsgType::Ping, "ping"},
    {MsgType::Transfer, "transfer"},
    {MsgType::GetAccount, "get_account"},
    {MsgType::Pong, "pong"},
    {MsgType::TransferOk, "transfer_ok"},
    {MsgType::Account, "account"},
    {MsgType::Error, "error"},
};

[[nodiscard]] std::string_view nameOfMsgType(MsgType type) noexcept;
[[nodiscard]] Result<MsgType> msgTypeFromName(std::string_view name) noexcept;

// ---------------------------------------------------------------------------
// 請求訊息
// ---------------------------------------------------------------------------

/// 心跳。沒有負載，用途是驗證整條路（切包 → 解碼 → 佇列 → worker → 回程）
/// 在不碰帳本的情況下是通的。壓測暖機也用它。
struct PingReq {
  static constexpr MsgType kType = MsgType::Ping;
  static constexpr auto fields() { return std::tuple{}; }
};

/// 轉帳。這是整個系統存在的理由。
struct TransferReq {
  std::string idemKey;
  AccountId from{0};
  AccountId to{0};
  Amount amount{0};
  Currency ccy{Currency::USD};

  static constexpr MsgType kType = MsgType::Transfer;

  // ★ 欄位只在這裡宣告一次。
  //   binary 的順序 = 這個 tuple 的順序；JSON 的 key = 這裡的字串。
  //   兩種編碼不可能對不上，因為它們讀的是同一份描述。
  static constexpr auto fields() {
    return std::tuple{
        Field{"idem_key", &TransferReq::idemKey},
        Field{"from", &TransferReq::from},
        Field{"to", &TransferReq::to},
        Field{"amount", &TransferReq::amount},
        Field{"ccy", &TransferReq::ccy},
    };
  }
};

/// 查單一帳戶。純讀路徑，Stage 6 之後它完全不碰 DB。
struct GetAccountReq {
  AccountId accountId{0};

  static constexpr MsgType kType = MsgType::GetAccount;

  static constexpr auto fields() {
    return std::tuple{
        Field{"account_id", &GetAccountReq::accountId},
    };
  }
};

// ---------------------------------------------------------------------------
// 回應訊息
// ---------------------------------------------------------------------------

struct PongResp {
  static constexpr MsgType kType = MsgType::Pong;
  static constexpr auto fields() { return std::tuple{}; }
};

struct TransferOkResp {
  TxId txId{0};
  Amount fromBalance{0};
  Amount toBalance{0};

  static constexpr MsgType kType = MsgType::TransferOk;

  static constexpr auto fields() {
    return std::tuple{
        Field{"tx_id", &TransferOkResp::txId},
        Field{"from_balance", &TransferOkResp::fromBalance},
        Field{"to_balance", &TransferOkResp::toBalance},
    };
  }
};

struct AccountResp {
  AccountId id{0};
  Amount balance{0};
  Currency ccy{Currency::USD};
  AccountStatus status{AccountStatus::Active};

  static constexpr MsgType kType = MsgType::Account;

  static constexpr auto fields() {
    return std::tuple{
        Field{"id", &AccountResp::id},
        Field{"balance", &AccountResp::balance},
        Field{"ccy", &AccountResp::ccy},
        Field{"status", &AccountResp::status},
    };
  }
};

/// 錯誤回應。
///
/// code 直接用 common/Result.h 的 ErrorCode —— 刻意不在 proto 層再定義
/// 一套對應表。多一張表就多一個會漂移的地方，而且轉譯本身也是 bug 來源。
struct ErrorResp {
  ErrorCode code{ErrorCode::Ok};
  std::string message;

  static constexpr MsgType kType = MsgType::Error;

  static constexpr auto fields() {
    return std::tuple{
        Field{"code", &ErrorResp::code},
        Field{"message", &ErrorResp::message},
    };
  }
};

// ---------------------------------------------------------------------------
// 信封 —— 訊息本體加上跨訊息共通的表頭欄位。
//
// ★ reqId 是 Stage 0 原始設計漏掉的東西。
//
//   Echo server 時代不需要它：回應就是原樣回送，配對是隱含的。
//   但 20 個 worker 平行處理之後，回應順序必然是亂的 —— client 連送
//   三筆轉帳，第三筆可能最先回來。沒有關聯 id，client 根本無法把
//   回應配對回請求。
//
//   reqId 跟 idempotency_key 是兩回事：
//     reqId    —— 只在單一連線內配對用。可重複、不持久化、server 原樣抄回。
//     idemKey  —— 跨連線、跨重啟的去重保證。寫進 DB 的 UNIQUE 索引。
// ---------------------------------------------------------------------------
using Request = std::variant<PingReq, TransferReq, GetAccountReq>;
using Response = std::variant<PongResp, TransferOkResp, AccountResp, ErrorResp>;

struct RequestEnvelope {
  std::uint32_t reqId{0};
  Request body;
};

struct ResponseEnvelope {
  std::uint32_t reqId{0};
  Response body;
};

/// 取出信封裡實際裝的訊息代碼。
[[nodiscard]] MsgType typeOf(const Request& req) noexcept;
[[nodiscard]] MsgType typeOf(const Response& resp) noexcept;

/// 產生一個錯誤回應的捷徑 —— 這在兩個 codec 和 Stage 5c 都會反覆用到。
[[nodiscard]] ResponseEnvelope makeError(std::uint32_t reqId,
                                         ErrorCode code,
                                         std::string message = {});

}  // namespace ledger::proto
