#pragma once

#include <ledger/common/Result.h>
#include <ledger/common/Types.h>
#include <ledger/money/Currency.h>

#include <limits>
#include <string>

namespace ledger {

// ---------------------------------------------------------------------------
// Money —— 不可變的值型別：一個整數金額 + 它的幣別。
//
// 為什麼要把幣別跟數字綁在一起：
//   如果金額只是一個裸的 int64，「把美金加到日圓帳戶」在型別上完全合法，
//   要靠人記得檢查。把幣別放進型別裡，加減法就能在同一個地方強制檢查，
//   而且只有這一個地方需要檢查。
//
// 沒有鎖、沒有狀態、可以自由複製。整份檔案在單執行緒下就能測到滿。
// ---------------------------------------------------------------------------
class Money {
 public:
  constexpr Money(Amount units, Currency ccy) noexcept : units_(units), ccy_(ccy) {}

  [[nodiscard]] constexpr Amount units() const noexcept { return units_; }
  [[nodiscard]] constexpr Currency currency() const noexcept { return ccy_; }

  [[nodiscard]] constexpr bool isZero() const noexcept { return units_ == 0; }
  [[nodiscard]] constexpr bool isNegative() const noexcept { return units_ < 0; }
  [[nodiscard]] constexpr bool isPositive() const noexcept { return units_ > 0; }

  /// 相加。幣別不同回 CurrencyMismatch，int64 溢位回 AmountOverflow。
  [[nodiscard]] Result<Money> plus(const Money& other) const noexcept;

  /// 相減。條件同上。
  [[nodiscard]] Result<Money> minus(const Money& other) const noexcept;

  /// 相反數。對 INT64_MIN 會回 AmountOverflow（它的相反數超出範圍）。
  [[nodiscard]] Result<Money> negated() const noexcept;

  /// "50.00 USD" / "5000 JPY"
  [[nodiscard]] std::string toString() const;

  friend constexpr bool operator==(const Money& a, const Money& b) noexcept {
    return a.units_ == b.units_ && a.ccy_ == b.ccy_;
  }
  friend constexpr bool operator!=(const Money& a, const Money& b) noexcept { return !(a == b); }

 private:
  Amount units_;
  Currency ccy_;
};

// ---------------------------------------------------------------------------
// 溢位檢查的自由函式版本。LedgerCore 直接操作裸 Amount 時會用到
// （在臨界區裡多包一層 Money 物件沒有意義）。
//
// 為什麼要自己檢查而不是先加了再看結果：
//   有號整數溢位在 C++ 是「未定義行為」，不是「回繞」。
//   編譯器有權假設它不會發生，於是 `if (a + b < a)` 這種事後檢查
//   會被最佳化整段刪掉。必須在做加法「之前」就判斷。
// ---------------------------------------------------------------------------

/// a + b 是否會溢位。
[[nodiscard]] constexpr bool addWouldOverflow(Amount a, Amount b) noexcept {
  if (b > 0 && a > std::numeric_limits<Amount>::max() - b) return true;
  if (b < 0 && a < std::numeric_limits<Amount>::min() - b) return true;
  return false;
}

/// a - b 是否會溢位。
[[nodiscard]] constexpr bool subWouldOverflow(Amount a, Amount b) noexcept {
  if (b < 0 && a > std::numeric_limits<Amount>::max() + b) return true;
  if (b > 0 && a < std::numeric_limits<Amount>::min() + b) return true;
  return false;
}

}  // namespace ledger
