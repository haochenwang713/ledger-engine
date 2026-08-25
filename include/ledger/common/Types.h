#pragma once

#include <cstdint>

namespace ledger {

/// 帳戶編號。對應 accounts.id（BIGINT）。
using AccountId = std::int64_t;

/// 帳戶持有人編號。對應 accounts.owner_id。
using OwnerId = std::int64_t;

/// 交易編號。對應 transactions.id。
///
/// 注意：記憶體這邊的 TxId 由 LedgerCore 的 atomic 計數器產生，
/// 只在單一行程的生命週期內唯一。Stage 6 接上 DB 之後，
/// 持久化的 transaction id 會改由 Postgres 的 IDENTITY 序列產生
/// —— 行程內計數器重啟會歸零、多實例會互撞，不能當持久 id。
using TxId = std::int64_t;

/// 金額，單位一律是該幣別的「最小單位」（見 Currency.h 的 exponent）。
///
/// 為什麼是整數而不是 double：
///   浮點數的 0.1 + 0.2 != 0.3。記帳系統不能容忍這件事。
///   int64 的範圍是 ±9.2×10^18，即使是沒有小數位的日圓也綽綽有餘。
using Amount = std::int64_t;

}  // namespace ledger
