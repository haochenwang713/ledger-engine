#include <ledger/proto/BinaryCodec.h>
#include <ledger/proto/Field.h>
#include <ledger/proto/FrameSplitter.h>

#include <cstring>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace ledger::proto {
namespace {

// ---------------------------------------------------------------------------
// ByteWriter —— 往 std::string 尾端寫 big-endian 基本型別。
//
// 沒有失敗路徑：寫入永遠會成功（頂多是 std::string 重新配置）。
// 所有的複雜度都在讀取那一側。
// ---------------------------------------------------------------------------
class ByteWriter {
 public:
  explicit ByteWriter(std::string& out) noexcept : out_(out) {}

  void u8(std::uint8_t v) { out_.push_back(static_cast<char>(v)); }

  void u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>((v >> 8) & 0xFFU));
    u8(static_cast<std::uint8_t>(v & 0xFFU));
  }

  void u32(std::uint32_t v) {
    u16(static_cast<std::uint16_t>((v >> 16) & 0xFFFFU));
    u16(static_cast<std::uint16_t>(v & 0xFFFFU));
  }

  /// int64 先轉成 uint64 再位移。
  ///
  /// 為什麼不直接對 int64 做 >>：帶號整數的右移對負數是實作定義行為
  /// （算術位移或邏輯位移由編譯器決定）。轉成無號之後位移是明確定義的，
  /// 而且 int64 → uint64 的轉換在 C++20 起明確規定是二補數重新詮釋。
  void i64(std::int64_t v) {
    const auto u = static_cast<std::uint64_t>(v);
    u32(static_cast<std::uint32_t>((u >> 32) & 0xFFFFFFFFU));
    u32(static_cast<std::uint32_t>(u & 0xFFFFFFFFU));
  }

  void bytes(std::string_view sv) { out_.append(sv.data(), sv.size()); }

  /// [u16 長度][位元組]
  void str(std::string_view sv) {
    u16(static_cast<std::uint16_t>(sv.size()));
    bytes(sv);
  }

 private:
  std::string& out_;
};

// ---------------------------------------------------------------------------
// ByteReader —— 有邊界檢查的讀取游標。
//
// ★ 設計重點：不用例外，改用一個 ok_ 旗標。
//
//   每個讀取函式先檢查剩餘長度，不足就把 ok_ 設成 false 並回傳零值。
//   後續的讀取全部變成 no-op。這讓呼叫端可以「一路讀完再檢查一次」，
//   而不是每讀一個欄位就寫一次 if —— 那種程式碼沒有人會全部寫對。
//
//   零值回傳配上「最後統一檢查」是關鍵：中途失敗之後的讀取雖然回垃圾，
//   但那些垃圾永遠不會被採用，因為 ok() 為 false 時整個訊息就被丟棄。
// ---------------------------------------------------------------------------
class ByteReader {
 public:
  explicit ByteReader(std::string_view data) noexcept : data_(data) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] bool exhausted() const noexcept { return pos_ >= data_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

  std::uint8_t u8() {
    if (!need(1)) {
      return 0;
    }
    return static_cast<std::uint8_t>(data_[pos_++]);
  }

  std::uint16_t u16() {
    const std::uint16_t hi = u8();
    const std::uint16_t lo = u8();
    return static_cast<std::uint16_t>((hi << 8) | lo);
  }

  std::uint32_t u32() {
    const std::uint32_t hi = u16();
    const std::uint32_t lo = u16();
    return (hi << 16) | lo;
  }

  std::int64_t i64() {
    const std::uint64_t hi = u32();
    const std::uint64_t lo = u32();
    return static_cast<std::int64_t>((hi << 32) | lo);
  }

  std::string str() {
    const std::uint16_t len = u16();
    if (!need(len)) {
      return {};
    }
    std::string s(data_.substr(pos_, len));
    pos_ += len;
    return s;
  }

  /// 三個 ASCII 位元組的 ISO 4217 代碼。
  Currency ccy() {
    if (!need(3)) {
      return Currency::USD;
    }
    const std::string_view code = data_.substr(pos_, 3);
    pos_ += 3;
    const Result<Currency> parsed = currencyFromCode(code);
    if (!parsed) {
      ok_ = false;
      return Currency::USD;
    }
    return parsed.value();
  }

 private:
  /// 剩餘位元組不足 n 就標記失敗。一旦失敗就永遠失敗。
  bool need(std::size_t n) noexcept {
    if (!ok_ || remaining() < n) {
      ok_ = false;
      return false;
    }
    return true;
  }

  std::string_view data_;
  std::size_t pos_{0};
  bool ok_{true};
};

// ---------------------------------------------------------------------------
// 基本型別的分派。
//
// ★ 這兩個函式是「加一個欄位只要改一行」的關鍵。
//   codec 不認識 TransferReq，它只認識這五種基本型別；
//   訊息長什麼樣子由 fields() 描述。
//
//   static_assert 的存在讓「加了一個沒支援型別的欄位」變成編譯錯誤，
//   而不是安靜地被跳過。
// ---------------------------------------------------------------------------
template <typename T>
void writeValue(ByteWriter& w, const T& v) {
  if constexpr (std::is_same_v<T, std::int64_t>) {
    w.i64(v);
  } else if constexpr (std::is_same_v<T, std::string>) {
    w.str(v);
  } else if constexpr (std::is_same_v<T, Currency>) {
    w.bytes(codeOf(v));
  } else if constexpr (std::is_same_v<T, ErrorCode>) {
    w.u16(static_cast<std::uint16_t>(v));
  } else if constexpr (std::is_same_v<T, AccountStatus>) {
    w.u8(static_cast<std::uint8_t>(v));
  } else {
    static_assert(kAlwaysFalse<T>, "BinaryCodec 沒有支援這個欄位型別");
  }
}

template <typename T>
T readValue(ByteReader& r) {
  if constexpr (std::is_same_v<T, std::int64_t>) {
    return r.i64();
  } else if constexpr (std::is_same_v<T, std::string>) {
    return r.str();
  } else if constexpr (std::is_same_v<T, Currency>) {
    return r.ccy();
  } else if constexpr (std::is_same_v<T, ErrorCode>) {
    return static_cast<ErrorCode>(r.u16());
  } else if constexpr (std::is_same_v<T, AccountStatus>) {
    return static_cast<AccountStatus>(r.u8());
  } else {
    static_assert(kAlwaysFalse<T>, "BinaryCodec 沒有支援這個欄位型別");
  }
}

/// 走訪 Msg::fields()，依序把每個欄位寫出去。
///
/// 逗號 fold 的求值順序由標準保證由左到右 —— 對 binary 來說這是
/// 正確性條件，不是風格問題。順序錯了整個 payload 就錯位。
template <typename Msg>
void encodeFields(ByteWriter& w, const Msg& msg) {
  std::apply([&](auto&&... field) { (writeValue(w, msg.*(field.member)), ...); }, Msg::fields());
}

template <typename Msg>
Msg decodeFields(ByteReader& r) {
  Msg msg{};
  std::apply(
      [&](auto&&... field) {
        ((msg.*(field.member) = readValue<std::remove_cvref_t<decltype(msg.*(field.member))>>(r)),
         ...);
      },
      Msg::fields());
  return msg;
}

/// 表頭 + payload → 完整 frame（含長度前綴）。
template <typename Msg>
std::string encodeFrame(std::uint32_t reqId, const Msg& msg) {
  std::string payload;
  ByteWriter payloadWriter(payload);
  encodeFields(payloadWriter, msg);

  std::string out;
  out.reserve(LengthPrefixSplitter::kPrefixSize + BinaryCodec::kHeaderSize + payload.size());

  ByteWriter w(out);
  // len 涵蓋表頭與 payload，不含自己。
  w.u32(static_cast<std::uint32_t>(BinaryCodec::kHeaderSize + payload.size()));
  w.u16(static_cast<std::uint16_t>(Msg::kType));
  w.u16(kProtocolVersion);
  w.u32(reqId);
  w.bytes(payload);
  return out;
}

/// 讀出表頭並驗證。成功時 r 停在 payload 的開頭。
struct Header {
  MsgType type{MsgType::Ping};
  std::uint32_t reqId{0};
};

Result<Header> readHeader(ByteReader& r) {
  const std::uint16_t rawType = r.u16();
  const std::uint16_t version = r.u16();
  const std::uint32_t reqId = r.u32();

  if (!r.ok()) {
    return ErrorCode::MalformedFrame;
  }

  // 版本檢查放在型別檢查之前：舊版 client 送來的可能是我們根本
  // 不認識的型別代碼，此時回「版本不支援」比回「型別不認識」精確得多。
  if (version != kProtocolVersion) {
    return ErrorCode::UnsupportedVersion;
  }

  return Header{static_cast<MsgType>(rawType), reqId};
}

/// 解完欄位之後的統一收尾檢查。
///
/// 兩件事一起檢查：
///   1. 讀取過程中有沒有撞到邊界（ok）
///   2. 有沒有多出來的位元組（exhausted）
///
/// 第二點看似吹毛求疵，但它是版本漂移的早期警報：如果 client 用新版
/// 多送了一個欄位，我們寧可明確拒絕，也不要安靜地忽略它。
[[nodiscard]] bool frameFullyConsumed(const ByteReader& r) noexcept {
  return r.ok() && r.exhausted();
}

}  // namespace

std::string_view nameOf(CodecTag tag) noexcept {
  switch (tag) {
    case CodecTag::Binary:
      return "binary";
    case CodecTag::Json:
      return "json";
  }
  return "unknown";
}

// ---------------------------------------------------------------------------
// 編碼
// ---------------------------------------------------------------------------

std::string BinaryCodec::encodeRequest(const RequestEnvelope& env) const {
  return std::visit([&](const auto& msg) { return encodeFrame(env.reqId, msg); }, env.body);
}

std::string BinaryCodec::encodeResponse(const ResponseEnvelope& env) const {
  return std::visit([&](const auto& msg) { return encodeFrame(env.reqId, msg); }, env.body);
}

// ---------------------------------------------------------------------------
// 解碼
// ---------------------------------------------------------------------------

Result<RequestEnvelope> BinaryCodec::decodeRequest(std::string_view frame) const {
  ByteReader r(frame);

  const Result<Header> header = readHeader(r);
  if (!header) {
    return header.error();
  }

  RequestEnvelope env;
  env.reqId = header.value().reqId;

  switch (header.value().type) {
    case MsgType::Ping:
      env.body = decodeFields<PingReq>(r);
      break;
    case MsgType::Transfer: {
      TransferReq req = decodeFields<TransferReq>(r);
      if (req.idemKey.size() > kMaxIdemKeyLength) {
        return ErrorCode::MalformedFrame;
      }
      env.body = std::move(req);
      break;
    }
    case MsgType::GetAccount:
      env.body = decodeFields<GetAccountReq>(r);
      break;

    // 回應型別出現在請求方向是協定違規，不是「不認識」。
    case MsgType::Pong:
    case MsgType::TransferOk:
    case MsgType::Account:
    case MsgType::Error:
    default:
      return ErrorCode::UnknownMessageType;
  }

  if (!frameFullyConsumed(r)) {
    return ErrorCode::MalformedFrame;
  }
  return env;
}

Result<ResponseEnvelope> BinaryCodec::decodeResponse(std::string_view frame) const {
  ByteReader r(frame);

  const Result<Header> header = readHeader(r);
  if (!header) {
    return header.error();
  }

  ResponseEnvelope env;
  env.reqId = header.value().reqId;

  switch (header.value().type) {
    case MsgType::Pong:
      env.body = decodeFields<PongResp>(r);
      break;
    case MsgType::TransferOk:
      env.body = decodeFields<TransferOkResp>(r);
      break;
    case MsgType::Account:
      env.body = decodeFields<AccountResp>(r);
      break;
    case MsgType::Error:
      env.body = decodeFields<ErrorResp>(r);
      break;

    case MsgType::Ping:
    case MsgType::Transfer:
    case MsgType::GetAccount:
    default:
      return ErrorCode::UnknownMessageType;
  }

  if (!frameFullyConsumed(r)) {
    return ErrorCode::MalformedFrame;
  }
  return env;
}

}  // namespace ledger::proto
