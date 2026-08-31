#pragma once

#include <ledger/proto/Codec.h>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// JsonCodec — NDJSON, one JSON object per line.
//
//   {"v":1,"id":"7","type":"transfer","idem_key":"req-a3f9-01",
//    "from":"1001","to":"2002","amount":"5000","ccy":"USD"}
//
// Three reserved keys:
//   "v"     protocol version. Optional — omitting it means "current", which
//           keeps hand-typed testing bearable
//   "id"    reqId, echoed back verbatim
//   "type"  message name, see kMsgTypeNames
//
// Every other key comes from the message struct's fields().
//
// Every integer is encoded as a string. This is not tidiness, it is
// correctness.
//
//   A JavaScript Number is an IEEE-754 double and is exact only up to 2^53.
//   Account ids and amounts are int64. Sent as bare numbers:
//
//     JSON.parse('{"amount":9007199254740993}').amount
//     // -> 9007199254740992   one cent gone, no error, no warning
//
//   For a ledger that is the worst kind of bug: everything works while the
//   numbers are small, and it starts quietly miscounting once they are not.
//
//   Protobuf's canonical JSON mapping requires int64 as a string too. That is
//   not a coincidence; it is the conclusion everyone reaches after hitting this.
//
//   So decoding rejects JSON numbers in integer fields outright. If the
//   frontend ever forgets the quotes, it should fail immediately rather than
//   work by luck because the value happened to be under 2^53.
// ---------------------------------------------------------------------------
class JsonCodec final : public Codec {
 public:
  [[nodiscard]] CodecTag tag() const noexcept override { return CodecTag::Json; }

  [[nodiscard]] Result<RequestEnvelope> decodeRequest(std::string_view frame) const override;
  [[nodiscard]] Result<ResponseEnvelope> decodeResponse(std::string_view frame) const override;

  /// The output ends with a newline and can be handed straight to send().
  [[nodiscard]] std::string encodeRequest(const RequestEnvelope& env) const override;
  [[nodiscard]] std::string encodeResponse(const ResponseEnvelope& env) const override;
};

}  // namespace ledger::proto
