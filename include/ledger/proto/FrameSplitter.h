#pragma once

#include <ledger/common/Result.h>
#include <ledger/proto/Messages.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// FrameSplitter — find one complete message in a run of bytes.
//
// It knows where a message starts and ends and nothing about what is inside.
// Codec is the exact opposite. That split is why the two ports can mix and
// match framing with encoding.
//
// Deliberately a pure function: it takes a string_view, reports where it cut,
// holds no state and never touches the Buffer. Three benefits:
//   1. unit-testable without a Buffer, and without linking net/ at all
//   2. no state means no leftovers from the previous call
//   3. the caller decides when to consume, so lifetimes stay obvious
//
// The standard loop on the caller's side (Stage 5c uses exactly this):
//
//   for (;;) {
//     const FrameView fv = splitter.next(buffer.view());
//     if (fv.status == FrameStatus::NeedMore) break;          // partial, wait
//     if (fv.status == FrameStatus::Error)   { conn->close(); break; }
//     handle(fv.frame);                                        // still valid
//     buffer.retrieve(fv.consumed);                            // invalid after
//   }
//
// fv.frame points into the caller's memory and stays valid until the buffer is
// next modified — so handle() must finish before retrieve(). The loop above is
// what that contract looks like in practice.
// ---------------------------------------------------------------------------

enum class FrameStatus : std::uint8_t {
  Ok,        ///< One complete message was found
  NeedMore,  ///< Not enough bytes yet. Nothing consumed; wait for more
  Error,     ///< Protocol violation (oversized, impossible length). Must close
};

struct FrameView {
  FrameStatus status{FrameStatus::NeedMore};

  /// One message's bytes, with the framing (length prefix or newline) removed.
  /// Valid only until the source buffer is modified.
  std::string_view frame{};

  /// How many bytes to consume from the buffer, framing included.
  /// Only meaningful when status == Ok.
  std::size_t consumed{0};

  ErrorCode error{ErrorCode::Ok};
};

class FrameSplitter {
 public:
  explicit FrameSplitter(std::size_t maxFrameSize = kMaxFrameSize) noexcept
      : maxFrameSize_(maxFrameSize) {}

  FrameSplitter(const FrameSplitter&) = delete;
  FrameSplitter& operator=(const FrameSplitter&) = delete;
  virtual ~FrameSplitter() = default;

  [[nodiscard]] virtual FrameView next(std::string_view input) const = 0;

  [[nodiscard]] std::size_t maxFrameSize() const noexcept { return maxFrameSize_; }

 protected:
  std::size_t maxFrameSize_;
};

// ---------------------------------------------------------------------------
// Length prefix — the binary port, :9000.
//
//   [len:u32 big-endian][ ... len bytes ... ]
//
//   len covers everything after itself but not itself, and the frame is those
//   len bytes (the binary header plus the payload).
//
// len excludes itself because then, having read it, "how many more bytes do I
// need" is just len — no arithmetic, so no chance of an off-by-one.
// ---------------------------------------------------------------------------
class LengthPrefixSplitter final : public FrameSplitter {
 public:
  static constexpr std::size_t kPrefixSize = 4;

  explicit LengthPrefixSplitter(std::size_t maxFrameSize = kMaxFrameSize) noexcept
      : FrameSplitter(maxFrameSize) {}

  [[nodiscard]] FrameView next(std::string_view input) const override;
};

// ---------------------------------------------------------------------------
// Newline delimited — the JSON port, :9001 (NDJSON, one object per line).
//
// Newlines rather than a length prefix for one reason, but an important one:
// `nc localhost 9001` and then typing is a usable client. That is what the
// JSON port is for — it is meant for people, not for machines.
//
// The cost is that there is no upper bound implied by the framing, so the
// maxFrameSize check here is not defensive programming but a requirement: a
// client that never sends a newline would otherwise grow the buffer forever.
// ---------------------------------------------------------------------------
class NewlineSplitter final : public FrameSplitter {
 public:
  explicit NewlineSplitter(std::size_t maxFrameSize = kMaxFrameSize) noexcept
      : FrameSplitter(maxFrameSize) {}

  [[nodiscard]] FrameView next(std::string_view input) const override;
};

}  // namespace ledger::proto
