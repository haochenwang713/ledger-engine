#pragma once

#include <ledger/common/Result.h>
#include <ledger/proto/Messages.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ledger::proto {

/// 一則訊息是從哪一種編碼進來的。
///
/// Task 會帶著它跑完整趟 worker 流程，回程時用同一個 codec 編碼回去。
/// 少了它，worker 就得知道自己在服務哪個 port —— 那是不該有的耦合。
enum class CodecTag : std::uint8_t {
  Binary,
  Json,
};

[[nodiscard]] std::string_view nameOf(CodecTag tag) noexcept;

// ---------------------------------------------------------------------------
// Codec —— 位元組與中立訊息型別之間的翻譯機。
//
// 四個方法而不是兩個，是因為 server 與 client 兩邊都要用：
//   server 走 decodeRequest / encodeResponse
//   client 走 encodeRequest / decodeResponse（Stage 8 的 Locust TCP client
//          會需要，測試的 round-trip 也需要）
//
// 全部標成 const 且不持有狀態 —— codec 可以被多執行緒同時使用，
// 一個行程只需要各一份。這是「純函式」這個設計選擇帶來的直接好處。
//
// ⚠ 輸入的 frame 是「一則訊息的位元組」，框架開銷已由 FrameSplitter 剝除。
//   輸出則相反：encode 出來的字串「含」框架開銷，可以直接 send()。
//   之所以不對稱，是因為解碼時切包與解碼是分開的兩步（要先知道界線才能解），
//   而編碼時長度只有寫完 payload 才知道，硬拆成兩步反而要多一次複製。
// ---------------------------------------------------------------------------
class Codec {
 public:
  Codec() = default;
  Codec(const Codec&) = delete;
  Codec& operator=(const Codec&) = delete;
  virtual ~Codec() = default;

  [[nodiscard]] virtual CodecTag tag() const noexcept = 0;

  [[nodiscard]] virtual Result<RequestEnvelope> decodeRequest(std::string_view frame) const = 0;
  [[nodiscard]] virtual Result<ResponseEnvelope> decodeResponse(std::string_view frame) const = 0;

  [[nodiscard]] virtual std::string encodeRequest(const RequestEnvelope& env) const = 0;
  [[nodiscard]] virtual std::string encodeResponse(const ResponseEnvelope& env) const = 0;
};

}  // namespace ledger::proto
