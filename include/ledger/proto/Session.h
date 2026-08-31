#pragma once

#include <ledger/proto/Codec.h>
#include <ledger/proto/FrameSplitter.h>
#include <ledger/proto/Messages.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// Session — turn a run of bytes into a run of requests.
//
// This is the only place in Stage 5c with real branching, and it is
// deliberately platform-independent: it knows nothing about sockets, nothing
// about threads, and holds no state. In go the bytes accumulated so far; out
// come the requests decoded, how many bytes to consume, and any errors that
// should be answered immediately.
//
// The loop is pulled out of ConnectionContext rather than written inline
// because the combination of partial frames, glued frames, decode failures and
// framing failures is the easiest thing in this layer to get wrong — and
// ConnectionContext depends on Connection, which is Linux-only. Extracted, all
// of this is testable on macOS with no socket involved.
//
// The two kinds of failure are handled in opposite ways:
//
//   Decode error — the frame boundary is known, the contents are not understood
//     Skip that message and continue with the next. Reply with an error; the
//     connection stays up.
//
//   Framing error — we no longer know where the next message begins
//     The byte stream cannot be realigned, so reading on produces only noise.
//     Reply, then close the connection.
//
//   Confusing the two puts the connection in a loop: garbage in, error out,
//   more garbage in, forever.
// ---------------------------------------------------------------------------
class Session {
 public:
  struct Outcome {
    /// Decoded successfully; should be handed to a worker.
    std::vector<RequestEnvelope> requests;

    /// Should be sent straight back to the client, bypassing the workers.
    std::vector<ResponseEnvelope> errors;

    /// How many bytes to consume. Partial frames are not counted.
    std::size_t consumed{0};

    /// The stream cannot be realigned; the connection must be closed.
    bool fatal{false};
  };

  /// Both the splitter and the codec are stateless and shared by the server.
  Session(const FrameSplitter& splitter, const Codec& codec) noexcept
      : splitter_(splitter), codec_(codec) {}

  /// Pull as many requests as possible out of the accumulated bytes.
  ///
  /// The caller retrieves(consumed) after handling the outcome. The
  /// RequestEnvelopes are values — already copied during decoding — so they do
  /// not depend on the input buffer staying alive. That is deliberate: they go
  /// onto a queue and may be read by another thread milliseconds later.
  [[nodiscard]] Outcome drain(std::string_view input) const;

  [[nodiscard]] CodecTag codecTag() const noexcept { return codec_.tag(); }

 private:
  const FrameSplitter& splitter_;
  const Codec& codec_;
};

}  // namespace ledger::proto
