#pragma once

#include <ledger/proto/Codec.h>
#include <ledger/proto/Messages.h>

#include <chrono>
#include <memory>
#include <utility>

namespace ledger::concurrent {

// ---------------------------------------------------------------------------
// ResponseSink — where a worker hands its result back.
//
// Why an abstract interface rather than holding a net::Connection directly
//
//   Connection lives in net/, which is Linux-only because of epoll. If Task
//   depended on it, all of concurrent/ would become Linux-only too — and the
//   thread pool is precisely the code that benefits most from being run under
//   TSan everywhere.
//
//   With the interface in between:
//     - concurrent/ stays platform-independent and fully testable on macOS
//     - tests can use a fake sink and never open a socket
//     - the weak_ptr semantics are preserved exactly (see W2 below)
//
// deliver() is called from a worker thread, so implementations must be thread
// safe.
//
//   The Stage 5c implementation does two things: append the bytes to the
//   Connection's outputBuffer_ (guarded by a small mutex) and ask the IO thread
//   to write via runInLoop.
//
//   A worker must never call write(fd) itself. Two workers writing the same
//   socket interleave their bytes, the length prefixes stop lining up, and the
//   protocol falls apart. That is not a data race — write() is thread safe — so
//   TSan will never find it. Only the architectural rule prevents it.
// ---------------------------------------------------------------------------
class ResponseSink {
 public:
  ResponseSink() = default;
  ResponseSink(const ResponseSink&) = delete;
  ResponseSink& operator=(const ResponseSink&) = delete;
  virtual ~ResponseSink() = default;

  virtual void deliver(const proto::ResponseEnvelope& resp, proto::CodecTag codec) = 0;
};

using ResponseSinkPtr = std::shared_ptr<ResponseSink>;
using ResponseSinkWeakPtr = std::weak_ptr<ResponseSink>;

// ---------------------------------------------------------------------------
// Task — one unit of work handed from the IO thread to a worker.
//
// W2: by the time a worker finishes, the connection may be gone.
//
//   A client can disconnect during the 1.5 ms a worker spends waiting on the
//   database. A shared_ptr here would pin the dead connection object — and its
//   fd — until the worker finishes. A raw pointer would be a use-after-free.
//
//   weak_ptr is the only correct choice: the worker locks it on completion and
//   quietly drops the result if it has expired.
// ---------------------------------------------------------------------------
struct Task {
  using Clock = std::chrono::steady_clock;

  ResponseSinkWeakPtr sink;
  proto::RequestEnvelope request;
  proto::CodecTag codec{proto::CodecTag::Binary};

  /// When this entered the queue.
  ///
  /// now() minus this at pop time is the queueing delay, which is a different
  /// number from the processing time — and under overload it is the one that
  /// dominates. Stage 8 measures them separately, otherwise queueing delay
  /// gets misdiagnosed as slow processing.
  Clock::time_point enqueuedAt{Clock::now()};

  Task() = default;

  Task(ResponseSinkWeakPtr s, proto::RequestEnvelope req, proto::CodecTag tag)
      : sink(std::move(s)), request(std::move(req)), codec(tag) {}

  [[nodiscard]] Clock::duration queueWait() const { return Clock::now() - enqueuedAt; }
};

}  // namespace ledger::concurrent
