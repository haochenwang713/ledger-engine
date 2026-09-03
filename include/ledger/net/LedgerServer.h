#pragma once

#include <ledger/common/ServerStats.h>
#include <ledger/concurrent/ThreadPool.h>
#include <ledger/core/LedgerCore.h>
#include <ledger/net/Acceptor.h>
#include <ledger/net/Connection.h>
#include <ledger/net/EventLoop.h>
#include <ledger/proto/BinaryCodec.h>
#include <ledger/proto/FrameSplitter.h>
#include <ledger/proto/JsonCodec.h>
#include <ledger/proto/Session.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ledger::net {

// ---------------------------------------------------------------------------
// ConnectionContext — one connection's protocol state and its return path.
//
// Ownership, which is the easiest thing in Stage 5c to get wrong:
//
//     LedgerServer::connections_  --shared-->  Connection
//                                                  |
//                                 the read callback captures shared
//                                                  v
//                                          ConnectionContext
//                                                  |
//                                             weak  <-- must not be shared
//                                                  v
//                                             Connection
//
//   The context may only hold the Connection weakly. A shared_ptr there is a
//   reference cycle: the two hold each other, the count never reaches zero, and
//   every closed connection leaks its fd and its buffers. That leak never
//   crashes — the process just grows, and it takes a few minutes of load
//   testing to notice.
//
//   In the other direction, each Task holds the context weakly. Client
//   disconnects -> server erases it from connections_ -> Connection destroyed
//   -> its callbacks destroyed -> context destroyed -> the worker's weak_ptr
//   expires -> the result is dropped. That is the whole W2 chain.
//
// Thread affinity:
//     onMessage()  runs only on the IO thread
//     deliver()    runs only on worker threads
//   The only thing they share is Connection::send(), which is itself thread
//   safe (small mutex plus runInLoop). The context has no mutable state of its
//   own to protect.
// ---------------------------------------------------------------------------
class ConnectionContext : public concurrent::ResponseSink,
                          public std::enable_shared_from_this<ConnectionContext> {
 public:
  ConnectionContext(const ConnectionPtr& conn,
                    const proto::FrameSplitter& splitter,
                    const proto::Codec& codec,
                    concurrent::ThreadPool& pool)
      : conn_(conn), session_(splitter, codec), codec_(codec), pool_(pool) {}

  /// Called on the IO thread. Frame, decode, hand to a worker.
  void onMessage(Buffer& buffer);

  /// Called on a worker thread. Encode and give it to the Connection.
  void deliver(const proto::ResponseEnvelope& resp, proto::CodecTag codec) override;

 private:
  /// Encode one response and send it. Drops it silently if the connection is
  /// already gone.
  void sendResponse(const proto::ResponseEnvelope& resp);

  std::weak_ptr<Connection> conn_;  ///< must be weak; see above
  proto::Session session_;
  const proto::Codec& codec_;
  concurrent::ThreadPool& pool_;
};

// ---------------------------------------------------------------------------
// LedgerServer — glue between the network layer, the protocol, the pools and
// the ledger core.
//
// Two ports, two entirely separate thread pools
//
//     :9000  length prefix + binary  ->  ThreadPool("binary", 20 workers)
//     :9001  newline + NDJSON        ->  ThreadPool("json",    4 workers)
//
//   The isolation matters because the JSON port is the debugging and frontend
//   entrance while the binary port carries load tests and real traffic. Sharing
//   one queue means somebody clicking around in a frontend crowds out the load
//   test and dirties the Stage 8 numbers.
//
//   Separate pools are stronger than separate queues: threads are isolated too,
//   so "a slow frontend query ties up a load-test worker" is structurally
//   impossible. The cost is four extra threads and, in Stage 6, four extra
//   database connections. Negligible.
//
// One EventLoop serves both ports
//
//   Both acceptors register on the same epoll. IO is nowhere near the
//   bottleneck — a request spends about 0.05 ms of parsing on the IO thread —
//   and a second loop thread would only make debugging harder.
// ---------------------------------------------------------------------------
class LedgerServer : public ServerStatsSource {
 public:
  struct Options {
    std::uint16_t binaryPort{9000};
    std::uint16_t jsonPort{9001};
    std::size_t binaryWorkers{20};
    std::size_t jsonWorkers{4};
    std::size_t binaryQueueCapacity{8192};
    std::size_t jsonQueueCapacity{1024};
  };

  LedgerServer(EventLoop& loop, LedgerCore& core, AccountRegistry& registry, Options options);

  LedgerServer(const LedgerServer&) = delete;
  LedgerServer& operator=(const LedgerServer&) = delete;

  ~LedgerServer();

  [[nodiscard]] bool valid() const noexcept {
    return binaryAcceptor_.valid() && jsonAcceptor_.valid();
  }
  [[nodiscard]] ErrorCode error() const noexcept {
    return binaryAcceptor_.valid() ? jsonAcceptor_.error() : binaryAcceptor_.error();
  }

  Status start();

  /// Stop both pools and wait for their queues to drain.
  /// Must be called after EventLoop::run() has returned.
  void shutdown();

  [[nodiscard]] std::uint16_t binaryPort() const noexcept { return binaryAcceptor_.port(); }
  [[nodiscard]] std::uint16_t jsonPort() const noexcept { return jsonAcceptor_.port(); }

  [[nodiscard]] std::size_t activeConnections() const noexcept {
    return activeCount_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t totalConnections() const noexcept {
    return binaryAcceptor_.acceptedCount() + jsonAcceptor_.acceptedCount();
  }

  [[nodiscard]] const concurrent::ThreadPool& binaryPool() const noexcept { return binaryPool_; }
  [[nodiscard]] const concurrent::ThreadPool& jsonPool() const noexcept { return jsonPool_; }

  /// Step 10 — what the get_stats message answers with.
  ///
  /// Called from a worker thread, concurrently with everything else. Every read
  /// inside is an atomic load or a queue size(); nothing here takes a ledger
  /// lock, so polling this cannot slow transfers down. See common/ServerStats.h.
  [[nodiscard]] ServerStatsSnapshot statsSnapshot() const override;

 private:
  enum class Protocol : std::uint8_t { Binary, Json };

  void onNewConnection(int fd, std::string peer, Protocol protocol);
  void onClose(const ConnectionPtr& conn);

  EventLoop& loop_;

  Acceptor binaryAcceptor_;
  Acceptor jsonAcceptor_;

  // Splitters and codecs are stateless pure-function objects; one of each is
  // shared by every connection.
  proto::LengthPrefixSplitter lengthSplitter_;
  proto::NewlineSplitter newlineSplitter_;
  proto::BinaryCodec binaryCodec_;
  proto::JsonCodec jsonCodec_;

  concurrent::ThreadPool binaryPool_;
  concurrent::ThreadPool jsonPool_;

  /// The server holds each connection's shared_ptr. Erasing from here is the
  /// moment the connection is destroyed. Only the loop thread touches it, so no
  /// lock is needed.
  std::unordered_map<int, ConnectionPtr> connections_;

  /// When the server was constructed, for the uptime field. steady_clock, not
  /// system_clock: a wall-clock adjustment must not make uptime jump or go
  /// negative.
  std::chrono::steady_clock::time_point startedAt_{std::chrono::steady_clock::now()};

  /// An atomic mirror of connections_.size() for other threads to read.
  /// Reading the map's size directly is a data race, and TSan catches it —
  /// Stage 4 already stepped on this.
  std::atomic<std::size_t> activeCount_{0};
};

}  // namespace ledger::net
