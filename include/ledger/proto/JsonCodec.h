#pragma once

#include <ledger/proto/Codec.h>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// JsonCodec —— NDJSON（一行一個 JSON 物件）。
//
//   {"v":1,"id":"7","type":"transfer","idem_key":"req-a3f9-01",
//    "from":"1001","to":"2002","amount":"5000","ccy":"USD"}
//
// 三個保留 key：
//   "v"     協定版本。可省略（省略即視為目前版本，方便手打測試）
//   "id"    reqId，server 原樣抄回
//   "type"  訊息名稱，見 kMsgTypeNames
//
// 其餘的 key 來自訊息 struct 的 fields()。
//
// ★★ 所有整數一律編碼成字串。這不是風格潔癖，是正確性問題。
//
//   JavaScript 的 Number 是 IEEE-754 double，只能精確表示到 2^53。
//   帳戶 id 和金額都是 int64。如果送裸數字：
//
//     JSON.parse('{"amount":9007199254740993}').amount
//     // → 9007199254740992   ← 少了 1，沒有錯誤，沒有警告
//
//   對記帳系統來說這是最難堪的 bug 種類 —— 它在金額還小的時候
//   完全正常，等到數字變大才開始默默算錯。
//
//   protobuf 的 canonical JSON mapping 也規定 int64 必須是字串。
//   這不是巧合，是所有踩過這個坑的人得到的同一個結論。
//
//   所以解碼時 **明確拒絕 JSON number 型別**。前端某天忘記加引號，
//   我們要當場報錯，而不是「剛好這次沒超過 2^53 所以能動」。
// ---------------------------------------------------------------------------
class JsonCodec final : public Codec {
 public:
  [[nodiscard]] CodecTag tag() const noexcept override { return CodecTag::Json; }

  [[nodiscard]] Result<RequestEnvelope> decodeRequest(std::string_view frame) const override;
  [[nodiscard]] Result<ResponseEnvelope> decodeResponse(std::string_view frame) const override;

  /// 輸出含結尾換行，可以直接 send()。
  [[nodiscard]] std::string encodeRequest(const RequestEnvelope& env) const override;
  [[nodiscard]] std::string encodeResponse(const ResponseEnvelope& env) const override;
};

}  // namespace ledger::proto
