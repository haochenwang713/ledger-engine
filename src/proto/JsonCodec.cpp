#include <ledger/proto/Field.h>
#include <ledger/proto/JsonCodec.h>

#include <charconv>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace ledger::proto {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kKeyVersion = "v";
constexpr std::string_view kKeyReqId = "id";
constexpr std::string_view kKeyType = "type";

constexpr std::string_view kStatusActive = "ACTIVE";
constexpr std::string_view kStatusClosed = "CLOSED";

// ---------------------------------------------------------------------------
// 整數 ↔ 字串。★ 整個 JSON 編碼裡最重要的兩個函式。
//
// 字串化的規則在這裡只存在一次。任何 int64 欄位都走這條路，
// 所以不可能有某個欄位「忘記字串化」。
// ---------------------------------------------------------------------------

[[nodiscard]] Json toJsonInt(std::int64_t v) {
  return std::to_string(v);
}

[[nodiscard]] Result<std::int64_t> fromJsonInt(const Json& j) {
  // ★ 這一行是防線本身。
  //
  //   如果放行 j.is_number()，前端某天忘記加引號送了裸數字，程式會
  //   「剛好能動」—— 直到金額超過 2^53 才開始默默算錯。那種 bug 會
  //   潛伏很久，而且事後極難證明是從哪裡開始錯的。
  //
  //   當場拒絕，讓錯誤發生在寫程式的當下，而不是上線之後。
  if (!j.is_string()) {
    return ErrorCode::IntegerNotString;
  }

  const auto& s = j.get_ref<const std::string&>();
  if (s.empty()) {
    return ErrorCode::MalformedFrame;
  }

  std::int64_t out = 0;
  const char* first = s.data();
  const char* last = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(first, last, out);

  // 三件事都要成立：解析沒出錯、沒有溢位、整個字串都被吃掉。
  //
  // 第三個條件擋掉 "123abc" 和 "12 " 這種輸入 —— from_chars 會
  // 高高興興地回傳 123 並把 ptr 停在 'a'。不檢查 ptr 的話，
  // 垃圾輸入會被安靜地接受成一個看似合理的數字。
  if (ec != std::errc{} || ptr != last) {
    return ErrorCode::MalformedFrame;
  }
  return out;
}

// ---------------------------------------------------------------------------
// 基本型別的分派。與 BinaryCodec 的 writeValue / readValue 一一對應 ——
// 支援的型別集合必須一致，否則加欄位時會有一邊編譯失敗（這是好事，
// static_assert 讓漏掉變成編譯期錯誤而不是執行期驚喜）。
// ---------------------------------------------------------------------------

template <typename T>
[[nodiscard]] Json toJsonValue(const T& v) {
  if constexpr (std::is_same_v<T, std::int64_t>) {
    return toJsonInt(v);
  } else if constexpr (std::is_same_v<T, std::string>) {
    return v;
  } else if constexpr (std::is_same_v<T, Currency>) {
    return std::string(codeOf(v));
  } else if constexpr (std::is_same_v<T, ErrorCode>) {
    return std::string(toString(v));
  } else if constexpr (std::is_same_v<T, AccountStatus>) {
    return std::string(v == AccountStatus::Closed ? kStatusClosed : kStatusActive);
  } else {
    static_assert(kAlwaysFalse<T>, "JsonCodec 沒有支援這個欄位型別");
  }
}

/// 解析一個欄位值。成功回 ErrorCode::Ok 並寫入 out，失敗回具體錯誤碼。
///
/// ⚠ 為什麼這裡用「輸出參數 + ErrorCode」而不是 Result<T>：
///
///   欄位型別之一是 ErrorCode 本身（ErrorResp::code），而 Result<ErrorCode>
///   在語意上是矛盾的 —— 成功值與錯誤值同型別，兩個隱式建構子的簽章
///   完全相同，無法區分「成功地解析出一個錯誤碼」和「解析失敗」。
///
///   Result<T> 是好用的型別，但它有這個先天限制。泛型程式碼會把所有
///   角落都走過一遍，所以這種限制在泛型化的當下才第一次浮現。
template <typename T>
[[nodiscard]] ErrorCode fromJsonValue(const Json& j, T& out) {
  if constexpr (std::is_same_v<T, std::int64_t>) {
    const Result<std::int64_t> parsed = fromJsonInt(j);
    if (!parsed) {
      return parsed.error();
    }
    out = parsed.value();
    return ErrorCode::Ok;
  } else if constexpr (std::is_same_v<T, std::string>) {
    if (!j.is_string()) {
      return ErrorCode::MalformedFrame;
    }
    out = j.get<std::string>();
    return ErrorCode::Ok;
  } else if constexpr (std::is_same_v<T, Currency>) {
    if (!j.is_string()) {
      return ErrorCode::MalformedFrame;
    }
    const Result<Currency> parsed = currencyFromCode(j.get_ref<const std::string&>());
    if (!parsed) {
      return parsed.error();
    }
    out = parsed.value();
    return ErrorCode::Ok;
  } else if constexpr (std::is_same_v<T, ErrorCode>) {
    if (!j.is_string()) {
      return ErrorCode::MalformedFrame;
    }
    const ErrorCode code = errorCodeFromString(j.get_ref<const std::string&>());
    if (code == ErrorCode::Count) {
      return ErrorCode::MalformedFrame;
    }
    out = code;
    return ErrorCode::Ok;
  } else if constexpr (std::is_same_v<T, AccountStatus>) {
    if (!j.is_string()) {
      return ErrorCode::MalformedFrame;
    }
    const auto& s = j.get_ref<const std::string&>();
    if (s == kStatusActive) {
      out = AccountStatus::Active;
      return ErrorCode::Ok;
    }
    if (s == kStatusClosed) {
      out = AccountStatus::Closed;
      return ErrorCode::Ok;
    }
    return ErrorCode::MalformedFrame;
  } else {
    static_assert(kAlwaysFalse<T>, "JsonCodec 沒有支援這個欄位型別");
  }
}

// ---------------------------------------------------------------------------
// 走訪欄位描述表
// ---------------------------------------------------------------------------

template <typename Msg>
void encodeFieldsJson(Json& obj, const Msg& msg) {
  std::apply(
      [&](auto&&... field) {
        ((obj[std::string(field.name)] = toJsonValue(msg.*(field.member))), ...);
      },
      Msg::fields());
}

template <typename Msg, typename FieldDesc>
void assignField(Msg& msg, const FieldDesc& field, const Json& obj, ErrorCode& err) {
  // 一旦有欄位失敗就停止 —— 後面的欄位不再處理，第一個錯誤被保留下來。
  if (err != ErrorCode::Ok) {
    return;
  }

  const auto it = obj.find(std::string(field.name));
  if (it == obj.end()) {
    err = ErrorCode::MissingField;
    return;
  }

  using T = std::remove_cvref_t<decltype(msg.*(field.member))>;
  err = fromJsonValue<T>(*it, msg.*(field.member));
}

template <typename Msg>
[[nodiscard]] Result<Msg> decodeFieldsJson(const Json& obj) {
  Msg msg{};
  ErrorCode err = ErrorCode::Ok;

  std::apply([&](auto&&... field) { (assignField(msg, field, obj, err), ...); }, Msg::fields());

  if (err != ErrorCode::Ok) {
    return err;
  }
  return msg;
}

// ---------------------------------------------------------------------------
// 表頭
// ---------------------------------------------------------------------------

template <typename Msg>
[[nodiscard]] std::string encodeLine(std::uint32_t reqId, const Msg& msg) {
  Json obj = Json::object();
  obj[std::string(kKeyVersion)] = kProtocolVersion;
  obj[std::string(kKeyReqId)] = std::to_string(reqId);
  obj[std::string(kKeyType)] = std::string(nameOfMsgType(Msg::kType));
  encodeFieldsJson(obj, msg);

  // NDJSON：一則訊息一行，結尾必須有換行。
  // 這裡直接把 framing 加上去，呼叫端拿到就能 send()。
  return obj.dump() + "\n";
}

struct JsonHeader {
  MsgType type{MsgType::Ping};
  std::uint32_t reqId{0};
};

[[nodiscard]] Result<JsonHeader> readJsonHeader(const Json& obj) {
  // 版本欄位可省略 —— 這是刻意的。JSON port 的主要價值是
  // 「nc localhost 9001 直接打字就能測」，強制每次手打 "v":1 會殺掉那個價值。
  // 有送就檢查，沒送就當作目前版本。
  if (const auto it = obj.find(std::string(kKeyVersion)); it != obj.end()) {
    if (!it->is_number_unsigned() || it->get<std::uint64_t>() != kProtocolVersion) {
      return ErrorCode::UnsupportedVersion;
    }
  }

  const auto typeIt = obj.find(std::string(kKeyType));
  if (typeIt == obj.end() || !typeIt->is_string()) {
    return ErrorCode::MissingField;
  }
  const Result<MsgType> type = msgTypeFromName(typeIt->get_ref<const std::string&>());
  if (!type) {
    return type.error();
  }

  // reqId 同樣走字串。它是 u32，其實塞得進 double 不會失真，
  // 但規則統一比「這個欄位剛好安全」更容易維護 —— 沒有例外就沒有
  // 「這個到底要不要加引號」的判斷，也就沒有判斷錯的機會。
  std::uint32_t reqId = 0;
  if (const auto idIt = obj.find(std::string(kKeyReqId)); idIt != obj.end()) {
    const Result<std::int64_t> parsed = fromJsonInt(*idIt);
    if (!parsed) {
      return parsed.error();
    }
    if (parsed.value() < 0 || parsed.value() > std::numeric_limits<std::uint32_t>::max()) {
      return ErrorCode::MalformedFrame;
    }
    reqId = static_cast<std::uint32_t>(parsed.value());
  }

  return JsonHeader{type.value(), reqId};
}

/// 把一行文字解析成 JSON 物件。
[[nodiscard]] Result<Json> parseObject(std::string_view frame) {
  // allow_exceptions = false：解析失敗回傳 discarded 而不是丟例外。
  // 熱路徑上不用例外的理由跟 Result<T> 一樣 —— 格式錯誤是預期中
  // 會發生的事（任何人都能往這個 port 送垃圾），不是例外狀況。
  Json parsed = Json::parse(frame.begin(), frame.end(), nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return ErrorCode::MalformedFrame;
  }
  return parsed;
}

}  // namespace

// ---------------------------------------------------------------------------
// 編碼
// ---------------------------------------------------------------------------

std::string JsonCodec::encodeRequest(const RequestEnvelope& env) const {
  return std::visit([&](const auto& msg) { return encodeLine(env.reqId, msg); }, env.body);
}

std::string JsonCodec::encodeResponse(const ResponseEnvelope& env) const {
  return std::visit([&](const auto& msg) { return encodeLine(env.reqId, msg); }, env.body);
}

// ---------------------------------------------------------------------------
// 解碼
// ---------------------------------------------------------------------------

Result<RequestEnvelope> JsonCodec::decodeRequest(std::string_view frame) const {
  const Result<Json> parsed = parseObject(frame);
  if (!parsed) {
    return parsed.error();
  }
  const Json& obj = parsed.value();

  const Result<JsonHeader> header = readJsonHeader(obj);
  if (!header) {
    return header.error();
  }

  RequestEnvelope env;
  env.reqId = header.value().reqId;

  switch (header.value().type) {
    case MsgType::Ping: {
      const Result<PingReq> msg = decodeFieldsJson<PingReq>(obj);
      if (!msg) {
        return msg.error();
      }
      env.body = msg.value();
      break;
    }
    case MsgType::Transfer: {
      Result<TransferReq> msg = decodeFieldsJson<TransferReq>(obj);
      if (!msg) {
        return msg.error();
      }
      if (msg.value().idemKey.size() > kMaxIdemKeyLength) {
        return ErrorCode::MalformedFrame;
      }
      env.body = std::move(msg).value();
      break;
    }
    case MsgType::GetAccount: {
      const Result<GetAccountReq> msg = decodeFieldsJson<GetAccountReq>(obj);
      if (!msg) {
        return msg.error();
      }
      env.body = msg.value();
      break;
    }
    case MsgType::GetStats: {
      const Result<GetStatsReq> msg = decodeFieldsJson<GetStatsReq>(obj);
      if (!msg) {
        return msg.error();
      }
      env.body = msg.value();
      break;
    }

    case MsgType::Pong:
    case MsgType::TransferOk:
    case MsgType::Account:
    case MsgType::Stats:
    case MsgType::Error:
    default:
      return ErrorCode::UnknownMessageType;
  }

  return env;
}

Result<ResponseEnvelope> JsonCodec::decodeResponse(std::string_view frame) const {
  const Result<Json> parsed = parseObject(frame);
  if (!parsed) {
    return parsed.error();
  }
  const Json& obj = parsed.value();

  const Result<JsonHeader> header = readJsonHeader(obj);
  if (!header) {
    return header.error();
  }

  ResponseEnvelope env;
  env.reqId = header.value().reqId;

  switch (header.value().type) {
    case MsgType::Pong: {
      const Result<PongResp> msg = decodeFieldsJson<PongResp>(obj);
      if (!msg) {
        return msg.error();
      }
      env.body = msg.value();
      break;
    }
    case MsgType::TransferOk: {
      const Result<TransferOkResp> msg = decodeFieldsJson<TransferOkResp>(obj);
      if (!msg) {
        return msg.error();
      }
      env.body = msg.value();
      break;
    }
    case MsgType::Account: {
      const Result<AccountResp> msg = decodeFieldsJson<AccountResp>(obj);
      if (!msg) {
        return msg.error();
      }
      env.body = msg.value();
      break;
    }
    case MsgType::Stats: {
      const Result<StatsResp> msg = decodeFieldsJson<StatsResp>(obj);
      if (!msg) {
        return msg.error();
      }
      env.body = msg.value();
      break;
    }
    case MsgType::Error: {
      Result<ErrorResp> msg = decodeFieldsJson<ErrorResp>(obj);
      if (!msg) {
        return msg.error();
      }
      env.body = std::move(msg).value();
      break;
    }

    case MsgType::Ping:
    case MsgType::Transfer:
    case MsgType::GetAccount:
    case MsgType::GetStats:
    default:
      return ErrorCode::UnknownMessageType;
  }

  return env;
}

}  // namespace ledger::proto
