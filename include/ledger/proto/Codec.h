#pragma once

#include <ledger/common/Result.h>
#include <ledger/proto/Messages.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ledger::proto {

/// Which encoding a message arrived in.
///
/// A Task carries this through the whole worker round trip so the response can
/// go back out in the same encoding. Without it, workers would have to know
/// which port they are serving — a coupling that does not belong there.
enum class CodecTag : std::uint8_t {
  Binary,
  Json,
};

[[nodiscard]] std::string_view nameOf(CodecTag tag) noexcept;

// ---------------------------------------------------------------------------
// Codec — the translator between bytes and the neutral message types.
//
// Four methods rather than two, because both sides need it:
//   server: decodeRequest / encodeResponse
//   client: encodeRequest / decodeResponse  (the Stage 8 Locust TCP client
//           needs these, and so do the round-trip tests)
//
// Every method is const and no codec holds state, so one instance per process
// can be shared across all threads. That falls straight out of choosing pure
// functions here.
//
// Note the asymmetry: the frame handed to decode has already had its framing
// stripped by FrameSplitter, while encode returns bytes *with* framing,
// ready to hand to send(). Decoding is two steps because you must know where a
// message ends before you can read it; encoding is one because the length is
// only known once the payload is written, and splitting it would just force an
// extra copy.
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
