#pragma once

#include <ledger/common/Result.h>
#include <ledger/common/Types.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace ledger {

// ---------------------------------------------------------------------------
// 幣別。
//
// 記憶體裡用 enum（1 byte）而不是字串：比較是一個整數比較，
// 而幣別比較會出現在每一筆轉帳的熱路徑上。字串在 DB 與協定層才用。
//
// 順序必須與 db/migrations/001_currencies.sql 的內容一致。
// ---------------------------------------------------------------------------
enum class Currency : std::uint8_t {
  USD = 0,
  EUR,
  JPY,
  GBP,
  CNY,
  TWD,
};

inline constexpr std::size_t kCurrencyCount = 6;

/// 最小單位的小數位數。
///
/// ★ 整個多幣別設計的核心。同一個 Amount = 5000：
///     USD（exponent 2）→ $50.00
///     JPY（exponent 0）→ ¥5,000
///   差一百倍。任何顯示或解析都必須查這個函式，
///   絕對不能在程式裡寫死 /100。
[[nodiscard]] std::int8_t exponentOf(Currency ccy) noexcept;

/// ISO 4217 三字母代碼，例如 "USD"。
[[nodiscard]] std::string_view codeOf(Currency ccy) noexcept;

/// 從代碼解析。不認識的代碼回 ErrorCode::UnknownCurrency。
[[nodiscard]] Result<Currency> currencyFromCode(std::string_view code) noexcept;

/// 把最小單位的整數格式化成人看的金額字串。
///
///   formatAmount(5000, Currency::USD) == "50.00"
///   formatAmount(5000, Currency::JPY) == "5000"
///   formatAmount(-5,   Currency::USD) == "-0.05"
///
/// 刻意不加千分位與貨幣符號 —— 那是呈現層的事，這裡只負責小數點位置。
[[nodiscard]] std::string formatAmount(Amount minorUnits, Currency ccy);

}  // namespace ledger
