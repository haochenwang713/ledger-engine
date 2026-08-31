#pragma once

#include <ledger/concurrent/ThreadPool.h>
#include <ledger/core/AccountRegistry.h>
#include <ledger/core/LedgerCore.h>
#include <ledger/proto/Messages.h>

namespace ledger {

// ---------------------------------------------------------------------------
// LedgerRequestHandler — the translation layer between the protocol and the
// ledger.
//
// This is the only place that knows about both proto:: and core::. Above it,
// net/ only ever sees a RequestEnvelope; below it, LedgerCore only ever sees a
// TransferRequest.
//
// Why even reads go through a worker
//
//   GET_ACCOUNT is one shared_lock and one read, about 50 ns. Putting it on the
//   queue adds a queueing delay and two context switches, so on latency alone
//   it is a loss.
//
//   Consistency is worth more: one path means one set of concurrency semantics
//   to reason about. And when Stage 8 measures, there is no noise from "some
//   requests took a shortcut" — the TPS and p95 numbers are comparable.
//
//   The decision is cheap to reverse: intercept GET_ACCOUNT in
//   ConnectionContext and answer it there. No other layer would notice.
//
// Threading: each worker owns its own LedgerRequestHandler, built on that
// thread by the ThreadPool factory, so this class does not need to be thread
// safe. The LedgerCore and AccountRegistry it shares do their own locking.
//
// The Stage 6 version additionally owns a pqxx::connection — and it is exactly
// because there is one handler per worker that the connection needs no
// protection.
// ---------------------------------------------------------------------------
class LedgerRequestHandler : public concurrent::RequestHandler {
 public:
  LedgerRequestHandler(LedgerCore& core, AccountRegistry& registry) noexcept
      : core_(core), registry_(registry) {}

  [[nodiscard]] proto::ResponseEnvelope handle(const proto::RequestEnvelope& env) override;

 private:
  [[nodiscard]] proto::ResponseEnvelope onTransfer(std::uint32_t reqId,
                                                   const proto::TransferReq& req);
  [[nodiscard]] proto::ResponseEnvelope onGetAccount(std::uint32_t reqId,
                                                     const proto::GetAccountReq& req);

  LedgerCore& core_;
  AccountRegistry& registry_;
};

/// Build the factory the ThreadPool needs.
///
/// The returned lambda runs once on each worker thread, so every worker gets
/// its own handler instance.
[[nodiscard]] concurrent::HandlerFactory makeLedgerHandlerFactory(LedgerCore& core,
                                                                  AccountRegistry& registry);

/// Load the demo accounts.
///
/// Stage 6 replaces this with SELECT id, balance FROM accounts. Until then it
/// is what gives the server something to transfer on startup.
///
/// The data deliberately matches db/seeds/dev_seed.sql exactly (Alice 1001 at
/// 115000, Bob 2002 at 47000), so that once Stage 6 is wired up the same nc
/// commands should produce identical answers — which is itself a check.
///
/// Money does not appear from nowhere: every balance comes from a transfer out
/// of system account 9001, so I1 (a transaction's two legs sum to zero) holds
/// from the very first row, and the magnitude of 9001's negative balance is the
/// total money in circulation.
Status seedDemoAccounts(LedgerCore& core, AccountRegistry& registry);

}  // namespace ledger
