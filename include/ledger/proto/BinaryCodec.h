#pragma once

#include <ledger/proto/Codec.h>

#include <cstddef>
#include <cstdint>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// BinaryCodec —— 長度前綴 + 手刻 big-endian 序列化。
//
// 完整 frame 的佈局（LengthPrefixSplitter 負責前 4 個位元組）：
//
//   ┌────────┬────────┬────────┬─────────┬──────────────────┐
//   │ len:u32│ type:u16│ ver:u16│ reqId:u32│   payload ...    │
//   └────────┴────────┴────────┴─────────┴──────────────────┘
//    ↑ 不含自己  └──────── 表頭 8 位元組 ────────┘
//        └─────────── len 涵蓋這一段 ───────────────────────┘
//
// payload 的欄位順序 = 訊息 struct 的 fields() 順序。
// 基本型別的編碼方式：
//
//   int64_t        8 位元組 big-endian（二補數）
//   std::string    [u16 len][bytes]，無結尾 NUL
//   Currency       3 個 ASCII 位元組（"USD"），不是 enum 值
//   ErrorCode      u16，直接用 enum 的數值
//   AccountStatus  u8
//
// ★ Currency 為什麼用三個字母而不是 enum 的一個位元組：
//   enum 的順序綁在 db/migrations/001_currencies.sql 上，有人重排
//   那份 SQL 就會靜默改變 wire format。三個字母是自我描述的 ——
//   hexdump 直接看得到 "USD"，而且跟 DB 的表示法一致。
//   多兩個位元組換掉一整類靜默失效，很划算。
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
