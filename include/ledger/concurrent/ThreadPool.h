#pragma once

#include <ledger/concurrent/BlockingQueue.h>
#include <ledger/concurrent/Task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace ledger::concurrent {

// ---------------------------------------------------------------------------
// RequestHandler — handle one request and produce a response. Runs on a worker.
//
// Why an abstract interface rather than std::function
//
//   Stage 6 handlers own a pqxx::connection, which is move-only, and
//   std::function requires a copyable target:
//
//     error: static assertion failed:
//            std::function target must be copy-constructible
//
//   Of the three ways out, this is the only one with no downside:
//     std::move_only_function  C++23, but libc++ still has not implemented it,
//                              so the macOS build would break
//     shared_ptr<connection>   works, but uses a shared pointer to express
//                              something that is never shared
//     unique_ptr<interface>    plain C++20, portable, and says what it means
//
//   unique_ptr is move-only by construction, and the worker owns it outright.
//   "Each worker has its own handler and never shares it" is stated in the type
//   rather than in a comment nobody will read.
//
// handle() is only ever called by the one worker thread that owns it, so
// implementations do not need to be thread safe. That is the point: the Stage 6
// database connection is created, used, and destroyed on a single thread, so it
// needs no protection at all.
// ---------------------------------------------------------------------------
class RequestHandler {
 public:
  RequestHandler() = default;
  RequestHandler(const RequestHandler&) = delete;
  RequestHandler& operator=(const RequestHandler&) = delete;
  virtual ~RequestHandler() = default;

  virtual proto::ResponseEnvelope handle(const proto::RequestEnvelope& req) = 0;
};

/// Build the handler belonging to worker number workerIndex.
///
/// The factory runs *on* that worker thread, exactly once. Stage 6 opens a
/// pqxx::connection here, so the connection is created on the same thread that
/// will use it.
///
/// The factory itself is copyable — it only manufactures, it does not hold the
/// connection — so std::function is fine at this level. The move-only thing is
/// what it produces.
using HandlerFactory = std::function<std::unique_ptr<RequestHandler>(std::size_t workerIndex)>;

// ---------------------------------------------------------------------------
// ThreadPool — N worker threads behind a bounded queue.
//
// Why this layer exists, in numbers
//
//   One transfer: roughly 0.05 ms of CPU, roughly 1.5 ms waiting on PostgreSQL
//   including the COMMIT fsync.
//
//     everything on the event loop:  1 / 1.55 ms  ~=    645 TPS
//     IO thread + 20 workers:       20 / 1.55 ms  ~= 12,900 TPS
//
//   Twenty times. And the first arrangement also lets one slow transfer stall
//   every connection.
//
// Why 20 and not the theoretical 124
//
//   N ~= cores * (1 + wait/compute) = 4 * (1 + 1.5/0.05) = 124. But PostgreSQL
//   defaults to max_connections = 100 and each connection is its own process
//   (5-10 MB); and 124 threads rotating across 4 cores lose the gain to context
//   switches and cache pollution. In practice the answer lands at 20-32.
//
//   Stage 8 treats the worker count as a tunable and plots TPS against it to
//   find the knee.
//
// Two ways to shut down
//
//   shutdown()  Refuse new work, finish what is queued, join. Normal shutdown.
//   abort()     request_stop, abandon the queue, join. Forced.
//
//   The destructor calls shutdown(), and that default is deliberate: queued
//   requests are work the server already accepted and whose clients are still
//   waiting. Dropping them leaves those clients with nothing but their own
//   timeout. Draining costs a few tens of milliseconds.
//
// Why std::jthread
//
//   Workers block in pop(stop_token), so request_stop() wakes them without
//   anyone having to remember to notify. And jthread's destructor does
//   request_stop() and join() by itself, so even if shutdown() is skipped
//   because of an exception the process still cannot hang. A hand-written
//   std::thread plus a flag has no such backstop.
// ---------------------------------------------------------------------------
class ThreadPool {
 public:
  /// Construction starts the workers.
  ///
  /// name is for logs and stats ("binary" / "json"). Stage 5c gives each port
  /// its own pool, hence two names.
  ThreadPool(std::string name,
             std::size_t workerCount,
             std::size_t queueCapacity,
             HandlerFactory factory);

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /// Graceful shutdown.
  ~ThreadPool();

  /// Submit work. Non-blocking — returns false immediately when the queue is
  /// full.
  ///
  /// This is the only submission method the IO thread may use. On false the
  /// caller's job is to produce a SERVER_BUSY response, not to retry or wait.
  [[nodiscard]] bool submit(Task task);

  /// Refuse new work, drain what is queued, join. Safe to call repeatedly.
  void shutdown();

  /// Abandon queued work and join. Safe to call repeatedly.
  ///
  /// Only for "already shutting down and no longer willing to wait". The normal
  /// path is always shutdown().
  void abort();

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] std::size_t workerCount() const noexcept { return workers_.size(); }
  [[nodiscard]] std::size_t queueSize() const { return queue_.size(); }
  [[nodiscard]] std::size_t queueCapacity() const noexcept { return queue_.capacity(); }

  /// Tasks that made it into the queue.
  [[nodiscard]] std::uint64_t submitted() const noexcept { return queue_.totalPushed(); }
  /// Tasks refused because the queue was full — the direct measure of backpressure.
  [[nodiscard]] std::uint64_t rejected() const noexcept { return queue_.totalRejected(); }
  /// Handler invocations that ran to completion.
  [[nodiscard]] std::uint64_t completed() const noexcept {
    return completed_.load(std::memory_order_relaxed);
  }
  /// Results thrown away because the connection was already gone (W2).
  [[nodiscard]] std::uint64_t droppedNoSink() const noexcept {
    return droppedNoSink_.load(std::memory_order_relaxed);
  }
  /// Total queueing time in microseconds. Divide by completed() for the mean.
  [[nodiscard]] std::uint64_t totalQueueWaitMicros() const noexcept {
    return queueWaitMicros_.load(std::memory_order_relaxed);
  }

 private:
  void workerLoop(std::stop_token stopToken, std::size_t index);
  void joinAll();

  const std::string name_;
  HandlerFactory factory_;

  BlockingQueue<Task> queue_;

  /// Declaration order matters: workers_ must come after queue_.
  ///
  ///   Members are destroyed in reverse declaration order, so workers_ goes
  ///   first (each jthread destructor does request_stop and join) and queue_
  ///   follows. The other way round, a worker would still be touching the queue
  ///   after it had been destroyed — a use-after-free that only shows up
  ///   occasionally, at shutdown.
  std::vector<std::jthread> workers_;

  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> droppedNoSink_{0};
  std::atomic<std::uint64_t> queueWaitMicros_{0};
};

}  // namespace ledger::concurrent
