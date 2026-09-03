#pragma once

#include <cstdint>

namespace ledger {

// ---------------------------------------------------------------------------
// ServerStats — what the engine can say about itself.
//
// Step 10 exists because until now the only proof this server works was a green
// ctest line and a shutdown printout. Every number below already existed; it was
// simply trapped inside the process. This header is the seam that lets it out.
//
// Why the interface lives in common/ rather than net/
//
//   LedgerRequestHandler is in core/, and core/ must not depend on net/ — that
//   direction would make the ledger unbuildable without epoll, undoing the
//   two-platform property that has already paid for itself twice.
//
//   So the handler depends on this abstract source, and LedgerServer implements
//   it. The handler never learns that sockets exist.
//
// Why every field is a cheap read
//
//   A dashboard polls. If answering a poll were expensive, the act of watching
//   the engine would change what it is watching — and at one poll per second
//   that is not a subtle effect.
//
//   Everything here is an atomic load except the two queue depths, which take
//   the queue's mutex for the length of a size() call. Deliberately absent:
//   anything that needs LedgerCore::audit() or verifyInvariants(). Those take
//   auditMutex_ *exclusively* and walk every account, so a polling dashboard
//   would repeatedly freeze all transfers. See progress.md, Step 10.
// ---------------------------------------------------------------------------

/// One coherent-enough reading of the server's counters.
///
/// "Coherent enough" is honest: the fields are read one after another without a
/// global lock, so a very busy engine can produce a snapshot whose numbers are
/// microseconds apart. That is the right trade — the alternative is stopping the
/// world to count, which is exactly what a monitoring path must never do.
struct ServerStatsSnapshot {
  std::int64_t uptimeMillis{0};

  std::int64_t connectionsActive{0};
  std::int64_t connectionsTotal{0};

  // --- binary pool (port 9000) ---
  std::int64_t binaryWorkers{0};
  std::int64_t binaryQueueDepth{0};
  std::int64_t binaryQueueCapacity{0};
  std::int64_t binarySubmitted{0};
  std::int64_t binaryCompleted{0};
  std::int64_t binaryRejected{0};
  std::int64_t binaryDropped{0};

  // --- json pool (port 9001) ---
  std::int64_t jsonWorkers{0};
  std::int64_t jsonQueueDepth{0};
  std::int64_t jsonQueueCapacity{0};
  std::int64_t jsonSubmitted{0};
  std::int64_t jsonCompleted{0};
  std::int64_t jsonRejected{0};
  std::int64_t jsonDropped{0};
};

/// Anything that can report on a running server. Implemented by LedgerServer.
class ServerStatsSource {
 public:
  ServerStatsSource() = default;
  virtual ~ServerStatsSource() = default;

  ServerStatsSource(const ServerStatsSource&) = delete;
  ServerStatsSource& operator=(const ServerStatsSource&) = delete;

  /// Called from a worker thread, so it must be safe to call concurrently with
  /// everything else the server is doing.
  [[nodiscard]] virtual ServerStatsSnapshot statsSnapshot() const = 0;
};

}  // namespace ledger
