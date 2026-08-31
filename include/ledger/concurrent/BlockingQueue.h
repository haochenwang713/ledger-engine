#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace ledger::concurrent {

// ---------------------------------------------------------------------------
// BlockingQueue<T> — a bounded multi-producer, multi-consumer queue.
//
// This is the only boundary between the IO thread and the worker threads.
//
// Why bounded (8192 by default)
//
//   An unbounded queue responds to a slow database in two bad ways: memory
//   grows without limit, and queueing time explodes — the client timed out
//   long ago while a worker is still processing what it sent thirty seconds
//   back. Both are worse than saying no.
//
//   A bounded queue is backpressure: under overload the system says no
//   explicitly instead of pretending it is still coping. 8192 entries is
//   roughly 0.6 seconds of backlog; past that the work is pointless anyway.
//
// Why not a lock-free MPMC queue
//
//   1. Wrong target. A push/pop pair is about 100 ns; waiting on the database
//      is about 1,500,000 ns. Four orders of magnitude apart.
//   2. New bugs. It means handling ABA and reclamation yourself (hazard
//      pointers or epochs), and TSan is not reliable about hand-written atomic
//      algorithms.
//   3. Contention is mild. The critical section pushes one Task and holds the
//      lock for roughly 50 ns.
//
//   The time to reconsider is Stage 8, if perf shows futex waits above 5% of
//   CPU — and the right next step then is a queue per worker with round-robin
//   dispatch, not a lock-free queue.
//
// Two ways to finish, with different meanings
//
//   close()      Refuse new work, but hand out what is already queued (drain).
//                This is normal shutdown — those clients are still waiting.
//   stop_token   Abandon the queue immediately. This is cancellation.
//
//   They answer different questions: "do we still accept work" and "do we
//   leave now or after finishing". Collapsing them into one flag forces a
//   choice at shutdown, when what is wanted is precisely the combination of
//   "no new work" and "finish what we have".
//
// Why condition_variable_any rather than condition_variable
//
//   Only the former has wait(lock, stop_token, pred). The cost is registering
//   a stop_callback on each blocking wait — and that only happens when the
//   queue is empty and a worker actually sleeps, which is exactly when the
//   system is not busy. A little idle overhead in exchange for eliminating the
//   whole "forgot to notify, so join() never returns" class of bug is worth it.
// ---------------------------------------------------------------------------
template <typename T>
class BlockingQueue {
 public:
  static constexpr std::size_t kDefaultCapacity = 8192;

  explicit BlockingQueue(std::size_t capacity = kDefaultCapacity)
      : capacity_(capacity == 0 ? 1 : capacity) {}

  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;

  ~BlockingQueue() { close(); }

  // -------------------------------------------------------------------------
  // Producer side
  // -------------------------------------------------------------------------

  /// Non-blocking push. Returns false immediately when full or closed.
  ///
  /// The IO thread may only ever use this, never push().
  ///
  ///   If the event loop thread blocks, every connection stalls — not just the
  ///   one whose request happened to arrive when the queue filled up. The
  ///   correct response to a full queue is a SERVER_BUSY reply.
  ///
  ///   This is the single most important rule in the server, and breaking it
  ///   produces no compile error: it shows up as throughput dropping to zero
  ///   under load.
  [[nodiscard]] bool tryPush(T value) {
    {
      std::unique_lock lock(mutex_);
      if (closed_ || queue_.size() >= capacity_) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      queue_.push_back(std::move(value));
      pushed_.fetch_add(1, std::memory_order_relaxed);
    }
    // Unlock before notifying: the woken thread should not immediately run
    // into a lock we are still holding.
    notEmpty_.notify_one();
    return true;
  }

  /// Blocking push; waits for room. Returns false if the queue is closed.
  ///
  /// For tests and offline bulk import only. It has no place on the server's
  /// hot path.
  [[nodiscard]] bool push(T value) {
    {
      std::unique_lock lock(mutex_);
      notFull_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
      if (closed_) {
        return false;
      }
      queue_.push_back(std::move(value));
      pushed_.fetch_add(1, std::memory_order_relaxed);
    }
    notEmpty_.notify_one();
    return true;
  }

  // -------------------------------------------------------------------------
  // Consumer side
  // -------------------------------------------------------------------------

  /// Blocking pop. nullopt means "closed and drained" — the worker is done.
  [[nodiscard]] std::optional<T> pop() {
    std::unique_lock lock(mutex_);

    // Including closed_ in the predicate is what makes shutdown terminate.
    // Waiting only on !queue_.empty() would leave these threads asleep forever
    // once nothing new arrives, and join() would never return.
    notEmpty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    return popLocked(lock);
  }

  /// Blocking pop that a stop_token can interrupt.
  ///
  /// Returns nullopt in three cases:
  ///   1. stop was already requested on entry (cancellation; the queue's
  ///      remaining contents are abandoned)
  ///   2. closed and drained (normal finish)
  ///   3. stop was requested while waiting
  [[nodiscard]] std::optional<T> pop(std::stop_token stopToken) {
    std::unique_lock lock(mutex_);

    // Check once up front: if we have been asked to stop, do not take on more.
    if (stopToken.stop_requested()) {
      return std::nullopt;
    }

    // This overload wakes and returns false on request_stop() with nobody
    // having to remember to notify. That is the whole reason for stop_token.
    if (!notEmpty_.wait(lock, stopToken, [this] { return closed_ || !queue_.empty(); })) {
      return std::nullopt;
    }
    return popLocked(lock);
  }

  /// Non-blocking pop. nullopt when empty.
  [[nodiscard]] std::optional<T> tryPop() {
    std::unique_lock lock(mutex_);
    return popLocked(lock);
  }

  // -------------------------------------------------------------------------
  // Lifetime
  // -------------------------------------------------------------------------

  /// Close the queue: refuse pushes and wake everyone waiting. Elements
  /// already queued can still be popped (drain).
  ///
  /// notify_all, not notify_one — every blocked thread has to wake and see
  /// closed_. Miss one and join() hangs forever, and that bug lives on the
  /// shutdown path, which is the least exercised code there is.
  void close() {
    {
      std::lock_guard lock(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
    }
    notEmpty_.notify_all();
    notFull_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }

  // -------------------------------------------------------------------------
  // Observability
  // -------------------------------------------------------------------------

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] bool empty() const { return size() == 0; }

  /// Counters use relaxed ordering: only the final tally matters, and they do
  /// not need to be ordered against anything else. That is exactly what atomics
  /// are good for — values that do not have to change together with something.
  [[nodiscard]] std::uint64_t totalPushed() const noexcept {
    return pushed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t totalPopped() const noexcept {
    return popped_.load(std::memory_order_relaxed);
  }
  /// Pushes refused because the queue was full or closed — the direct measure
  /// of backpressure, and a curve in the Stage 8 report.
  [[nodiscard]] std::uint64_t totalRejected() const noexcept {
    return rejected_.load(std::memory_order_relaxed);
  }

 private:
  /// The common tail of all three pop overloads. The lock must already be held.
  [[nodiscard]] std::optional<T> popLocked(std::unique_lock<std::mutex>& lock) {
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    popped_.fetch_add(1, std::memory_order_relaxed);

    lock.unlock();
    notFull_.notify_one();
    return value;
  }

  mutable std::mutex mutex_;
  std::condition_variable_any notEmpty_;
  std::condition_variable_any notFull_;

  std::deque<T> queue_;
  const std::size_t capacity_;
  bool closed_{false};

  std::atomic<std::uint64_t> pushed_{0};
  std::atomic<std::uint64_t> popped_{0};
  std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace ledger::concurrent
