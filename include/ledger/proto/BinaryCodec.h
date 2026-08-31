#pragma once

#include <ledger/proto/Codec.h>

#include <cstddef>
#include <cstdint>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// BinaryCodec — length-prefixed frames, hand-rolled big-endian.
//
// The full frame layout (LengthPrefixSplitter owns the first four bytes):
//
//   +--------+---------+--------+----------+------------------+
//   | len:u32| type:u16| ver:u16| reqId:u32|   payload ...    |
//   +--------+---------+--------+----------+------------------+
//    ^ excludes itself
//             \------- 8-byte header -------/
//             \------------- len covers this ----------------/
//
// Payload field order is the message struct's fields() order. The primitives:
//
//   int64_t        8 bytes big-endian, two's complement
//   std::string    [u16 length][bytes], no NUL terminator
//   Currency       three ASCII bytes ("USD"), not the enum value
//   ErrorCode      u16, the enum's numeric value
//   AccountStatus  u8
//
// Currency is three letters rather than a one-byte enum because the enum's
// ordering is tied to db/migrations/001_currencies.sql — reordering that SQL
// would silently change the wire format. Three letters are self-describing:
// "USD" is visible in a hexdump and matches how the database spells it. Two
// extra bytes to remove a whole class of silent breakage is a good trade.
// ---------------------------------------------------------------------------
class BinaryCodec final : public Codec {
 public:
  /// type + ver + reqId
  static constexpr std::size_t kHeaderSize = 8;

  [[nodiscard]] CodecTag tag() const noexcept override { return CodecTag::Binary; }

  [[nodiscard]] Result<RequestEnvelope> decodeRequest(std::string_view frame) const override;
  [[nodiscard]] Result<ResponseEnvelope> decodeResponse(std::string_view frame) const override;

  [[nodiscard]] std::string encodeRequest(const RequestEnvelope& env) const override;
  [[nodiscard]] std::string encodeResponse(const ResponseEnvelope& env) const override;
};

}  // namespace ledger::proto
