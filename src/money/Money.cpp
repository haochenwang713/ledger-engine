#include <ledger/money/Money.h>

#include <limits>
#include <string>

namespace ledger {

Result<Money> Money::plus(const Money& other) const noexcept {
  if (ccy_ != other.ccy_) {
    return ErrorCode::CurrencyMismatch;
  }
  if (addWouldOverflow(units_, other.units_)) {
    return ErrorCode::AmountOverflow;
  }
  return Money{units_ + other.units_, ccy_};
}

Result<Money> Money::minus(const Money& other) const noexcept {
  if (ccy_ != other.ccy_) {
    return ErrorCode::CurrencyMismatch;
  }
  if (subWouldOverflow(units_, other.units_)) {
    return ErrorCode::AmountOverflow;
  }
  return Money{units_ - other.units_, ccy_};
}

Result<Money> Money::negated() const noexcept {
  // INT64_MIN 的相反數是 INT64_MAX + 1，超出範圍。
  if (units_ == std::numeric_limits<Amount>::min()) {
    return ErrorCode::AmountOverflow;
  }
  return Money{-units_, ccy_};
}

std::string Money::toString() const {
  std::string out = formatAmount(units_, ccy_);
  out += ' ';
  out += codeOf(ccy_);
  return out;
}

}  // namespace ledger
