// ---------------------------------------------------------------------------
// 值型別層的測試。這一層完全沒有執行緒，所以可以測到很細。
//
// 重點在兩件容易寫錯的事：
//   1. JPY 的 exponent = 0 —— 同一個整數在不同幣別代表不同金額
//   2. int64 溢位 —— 有號整數溢位是未定義行為，必須事前擋
//
// Test structure: Arrange-Act-Assert. Several tests here are pure fact tables
// (a currency's exponent is a constant, not something you "do"), so they carry
// an "Assert only" marker rather than an invented Arrange step. The convention
// is in instruction.md.
// ---------------------------------------------------------------------------

#include <ledger/money/Currency.h>
#include <ledger/money/Money.h>

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace ledger {
namespace {

constexpr Amount kMaxAmount = std::numeric_limits<Amount>::max();
constexpr Amount kMinAmount = std::numeric_limits<Amount>::min();

// === Currency ==============================================================

TEST(Currency, ExponentsMatchSchema) {
  // Assert only —— 這是一張事實表，沒有動作可以執行。
  // 必須與 db/migrations/001_currencies.sql 一致。
  EXPECT_EQ(exponentOf(Currency::USD), 2);
  EXPECT_EQ(exponentOf(Currency::EUR), 2);
  EXPECT_EQ(exponentOf(Currency::GBP), 2);
  EXPECT_EQ(exponentOf(Currency::CNY), 2);
  EXPECT_EQ(exponentOf(Currency::TWD), 2);
  EXPECT_EQ(exponentOf(Currency::JPY), 0);  // ★ 唯一的例外
}

TEST(Currency, CodeRoundTrip) {
  for (std::size_t i = 0; i < kCurrencyCount; ++i) {
    // Arrange
    const auto ccy = static_cast<Currency>(i);

    // Act
    const auto parsed = currencyFromCode(codeOf(ccy));

    // Assert
    ASSERT_TRUE(parsed.ok());
    EXPECT_EQ(parsed.value(), ccy);
  }
}

TEST(Currency, RejectsUnknownCode) {
  // Act
  const auto unknown = currencyFromCode("XXX");
  const auto lowercase = currencyFromCode("usd");
  const auto blank = currencyFromCode("");

  // Assert
  EXPECT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error(), ErrorCode::UnknownCurrency);
  EXPECT_FALSE(lowercase.ok());  // 小寫不接受
  EXPECT_FALSE(blank.ok());
}

// ★ 這組測試就是整個多幣別設計的重點。
TEST(Currency, SameIntegerMeansDifferentMoney) {
  // Arrange
  constexpr Amount stored = 5000;

  // Act
  const std::string asUsd = formatAmount(stored, Currency::USD);
  const std::string asJpy = formatAmount(stored, Currency::JPY);

  // Assert —— 同一個 5000，差一百倍。
  // 任何寫死 /100 的程式碼都會在 JPY 上出錯。
  EXPECT_EQ(asUsd, "50.00");
  EXPECT_EQ(asJpy, "5000");
}

TEST(Currency, FormatHandlesSmallAndNegativeAmounts) {
  // Assert only —— 一張格式化對照表。
  EXPECT_EQ(formatAmount(5, Currency::USD), "0.05");  // 小數要補零，不是 "0.5"
  EXPECT_EQ(formatAmount(50, Currency::USD), "0.50");
  EXPECT_EQ(formatAmount(0, Currency::USD), "0.00");
  EXPECT_EQ(formatAmount(0, Currency::JPY), "0");

  EXPECT_EQ(formatAmount(-5, Currency::USD), "-0.05");  // 符號要在最前面
  EXPECT_EQ(formatAmount(-5000, Currency::USD), "-50.00");
  EXPECT_EQ(formatAmount(-5000, Currency::JPY), "-5000");
}

TEST(Currency, FormatHandlesInt64Extremes) {
  // Act —— INT64_MIN 取相反數會溢位，所以格式化時必須繞過去（見 Currency.cpp）。
  const std::string minStr = formatAmount(kMinAmount, Currency::USD);
  const std::string maxStr = formatAmount(kMaxAmount, Currency::JPY);

  // Assert —— 這裡不驗確切字串，只確認它不會崩、不會是空的。
  EXPECT_FALSE(minStr.empty());
  EXPECT_EQ(minStr.front(), '-');
  EXPECT_EQ(maxStr, std::to_string(kMaxAmount));
}

// === 溢位檢查 ==============================================================

TEST(Overflow, DetectsAdditionOverflow) {
  // Assert only —— 一張邊界對照表。
  EXPECT_TRUE(addWouldOverflow(kMaxAmount, 1));
  EXPECT_TRUE(addWouldOverflow(1, kMaxAmount));
  EXPECT_TRUE(addWouldOverflow(kMinAmount, -1));

  EXPECT_FALSE(addWouldOverflow(kMaxAmount, 0));
  EXPECT_FALSE(addWouldOverflow(kMaxAmount, -1));
  EXPECT_FALSE(addWouldOverflow(100, 200));
}

TEST(Overflow, DetectsSubtractionOverflow) {
  // Assert only —— 一張邊界對照表。
  EXPECT_TRUE(subWouldOverflow(kMinAmount, 1));
  EXPECT_TRUE(subWouldOverflow(kMaxAmount, -1));

  EXPECT_FALSE(subWouldOverflow(kMinAmount, 0));
  EXPECT_FALSE(subWouldOverflow(kMinAmount, -1));
  EXPECT_FALSE(subWouldOverflow(200, 100));
}

// === Money =================================================================

TEST(Money, AddsAndSubtractsWithinCurrency) {
  // Arrange
  const Money a{5000, Currency::USD};
  const Money b{2500, Currency::USD};

  // Act
  const auto sum = a.plus(b);
  const auto diff = a.minus(b);

  // Assert
  ASSERT_TRUE(sum.ok());
  EXPECT_EQ(sum.value(), Money(7500, Currency::USD));
  ASSERT_TRUE(diff.ok());
  EXPECT_EQ(diff.value(), Money(2500, Currency::USD));
}

// ★ 把幣別放進型別裡的整個意義就在這一條。
TEST(Money, RefusesToMixCurrencies) {
  // Arrange
  const Money usd{5000, Currency::USD};
  const Money jpy{5000, Currency::JPY};

  // Act
  const auto sum = usd.plus(jpy);
  const auto diff = usd.minus(jpy);

  // Assert
  EXPECT_EQ(sum.error(), ErrorCode::CurrencyMismatch);
  EXPECT_EQ(diff.error(), ErrorCode::CurrencyMismatch);
}

TEST(Money, ReportsOverflowInsteadOfWrapping) {
  // Arrange
  const Money big{kMaxAmount, Currency::USD};
  const Money small{kMinAmount, Currency::USD};

  // Act
  const auto overflowed = big.plus(Money{1, Currency::USD});
  const auto underflowed = small.minus(Money{1, Currency::USD});
  const auto negated = small.negated();

  // Assert
  EXPECT_EQ(overflowed.error(), ErrorCode::AmountOverflow);
  EXPECT_EQ(underflowed.error(), ErrorCode::AmountOverflow);
  EXPECT_EQ(negated.error(), ErrorCode::AmountOverflow);
}

TEST(Money, ToStringIncludesCurrencyCode) {
  // Act
  const std::string usd = Money(5000, Currency::USD).toString();
  const std::string jpy = Money(5000, Currency::JPY).toString();

  // Assert
  EXPECT_EQ(usd, "50.00 USD");
  EXPECT_EQ(jpy, "5000 JPY");
}

// === Result ================================================================

TEST(Result, CarriesEitherValueOrError) {
  // Arrange
  const Result<int> good{42};
  const Result<int> bad{ErrorCode::InsufficientFunds};

  // Assert
  EXPECT_TRUE(good.ok());
  EXPECT_EQ(good.value(), 42);
  EXPECT_EQ(good.error(), ErrorCode::Ok);

  EXPECT_FALSE(bad.ok());
  EXPECT_EQ(bad.error(), ErrorCode::InsufficientFunds);
  EXPECT_EQ(bad.valueOr(-1), -1);
}

TEST(Result, ErrorCodesHaveNames) {
  // Assert only —— 這些名字會直接出現在協定回應裡，等同於對外契約。
  EXPECT_EQ(toString(ErrorCode::InsufficientFunds), "INSUFFICIENT_FUNDS");
  EXPECT_EQ(toString(ErrorCode::CurrencyMismatch), "CURRENCY_MISMATCH");
  EXPECT_EQ(toString(ErrorCode::SelfTransfer), "SELF_TRANSFER");
}

}  // namespace
}  // namespace ledger
