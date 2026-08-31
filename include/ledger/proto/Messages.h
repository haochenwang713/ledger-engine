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
// The protocol's message types.
//
// These are neutral value types: they know nothing about binary and nothing
// about JSON. BinaryCodec and JsonCodec are translators; LedgerCore and the
// workers only ever see these.
//
// The benefit only becomes obvious in Stage 6: with the database wired in,
// there is still exactly one copy of the business logic, and supporting a
// second encoding does not double the integration tests.
// ---------------------------------------------------------------------------

/// Protocol version. A mismatched request is rejected and the connection closed.
///
/// Deciding the semantics now matters: Stage 0 specified a ver field but not
/// what to do when it disagrees, which would have left it a dead field that is
/// always 1. "Mismatch means reject" costs two lines and makes it real.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// Upper bound on one message.
///
/// NDJSON needs this more than the binary port does: with no length prefix, a
/// client that never sends a newline can grow the buffer without limit.
inline constexpr std::size_t kMaxFrameSize = 64 * 1024;

/// Upper bound on an idempotency key. Binary stores its length in a u16; this
/// is deliberately stricter.
inline constexpr std::size_t kMaxIdemKeyLength = 128;

// ---------------------------------------------------------------------------
// Message codes. The high bit means "this is a response".
//
// These numbers are frozen once published — clients and the Locust script
// depend on them. New messages are appended; old values are never reused.
// ---------------------------------------------------------------------------
enum class MsgType : std::uint16_t {
  // --- requests ---
  Ping = 0x0001,
  Transfer = 0x0002,
  GetAccount = 0x0003,

  // --- responses ---
  Pong = 0x8001,
  TransferOk = 0x8002,
  Account = 0x8003,
  Error = 0x80FF,
};

[[nodiscard]] constexpr bool isResponseType(MsgType t) noexcept {
  return (static_cast<std::uint16_t>(t) & 0x8000U) != 0;
}

/// Account state. Mirrors the status column in db/migrations/002_accounts.sql.
enum class AccountStatus : std::uint8_t {
  Active = 0,
  Closed = 1,
};

// ---------------------------------------------------------------------------
// MsgType <-> name, used for the JSON "type" field.
//
// One array rather than two switch statements: both directions read the same
// data, so there is no way to end up encoding a new name while only decoding
// the old one.
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
// Requests
// ---------------------------------------------------------------------------

/// Heartbeat. No payload. It verifies the whole path — framing, decoding,
/// queue, worker, response — without touching the ledger, and it is what the
/// load test uses to warm up.
struct PingReq {
  static constexpr MsgType kType = MsgType::Ping;
  static constexpr auto fields() { return std::tuple{}; }
};

/// A transfer. The reason this system exists.
struct TransferReq {
  std::string idemKey;
  AccountId from{0};
  AccountId to{0};
  Amount amount{0};
  Currency ccy{Currency::USD};

  static constexpr MsgType kType = MsgType::Transfer;

  // Declared once. Binary order is this tuple's order; JSON keys are these
  // strings. The two encodings cannot disagree, because they read the same
  // description.
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

/// Look up one account. A pure read; even after Stage 6 it never touches the
/// database.
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
// Responses
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

/// An error response.
///
/// The code is the ErrorCode from common/Result.h rather than a protocol-local
/// enum. A second table is a second thing that can drift, and the translation
/// between them would be its own source of bugs.
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
// Envelopes — a message plus the header fields common to all of them.
//
// reqId was missing from the original Stage 0 design.
//
//   The echo server did not need it: the response *was* the request, so the
//   correlation was implicit. But with twenty workers running in parallel the
//   response order is necessarily scrambled — send three transfers and the
//   third may come back first. Without a correlation id a client simply cannot
//   match responses to requests.
//
//   reqId and idempotency_key are different things:
//     reqId   correlation within one connection. May repeat, is not persisted,
//             and the server echoes it back verbatim.
//     idemKey the deduplication guarantee across connections and restarts.
//             It goes into a UNIQUE index in the database.
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

/// The message code of whatever the envelope actually holds.
[[nodiscard]] MsgType typeOf(const Request& req) noexcept;
[[nodiscard]] MsgType typeOf(const Response& resp) noexcept;

/// Shorthand for an error response, used by both codecs and by Stage 5c.
[[nodiscard]] ResponseEnvelope makeError(std::uint32_t reqId,
                                         ErrorCode code,
                                         std::string message = {});

}  // namespace ledger::proto
