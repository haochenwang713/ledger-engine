#include <ledger/money/Currency.h>

#include <array>
#include <cstdlib>
#include <string>

namespace ledger {
namespace {

struct CurrencyInfo {
  std::string_view code;
  std::int8_t exponent;
};

// 索引必須與 enum Currency 的值一一對應。
// 用陣列而不是 map：查表是 O(1) 且沒有雜湊成本，而這在熱路徑上。
constexpr std::array<CurrencyInfo, kCurrencyCount> kCurrencies{{
    {"USD", 2},
    {"EUR", 2},
    {"JPY", 0},  // ← 唯一沒有小數位的
    {"GBP", 2},
    {"CNY", 2},
    {"TWD", 2},
}};

constexpr std::size_t indexOf(Currency ccy) noexcept {
  return static_cast<std::size_t>(ccy);
}

}  // namespace

std::int8_t exponentOf(Currency ccy) noexcept {
  return kCurrencies[indexOf(ccy)].exponent;
}

std::string_view codeOf(Currency ccy) noexcept {
  return kCurrencies[indexOf(ccy)].code;
}

Result<Currency> currencyFromCode(std::string_view code) noexcept {
  for (std::size_t i = 0; i < kCurrencies.size(); ++i) {
    if (kCurrencies[i].code == code) {
      return static_cast<Currency>(i);
    }
  }
  return ErrorCode::UnknownCurrency;
}

std::string formatAmount(Amount minorUnits, Currency ccy) {
  const std::int8_t exp = exponentOf(ccy);

  // exponent 為 0（日圓）就直接印整數，不要有小數點。
  if (exp == 0) {
    return std::to_string(minorUnits);
  }

  // 負數要先把符號拆出來再處理，否則 -5 會變成 "-0.-5"。
  const bool negative = minorUnits < 0;

  // 取絕對值時不能直接寫 -minorUnits：
  // INT64_MIN 的相反數超出 int64 範圍，是未定義行為。
  // 先轉成無號型別再取負，是標準保證良好定義的作法。
  const std::uint64_t magnitude = negative ? (~static_cast<std::uint64_t>(minorUnits) + 1U)
                                           : static_cast<std::uint64_t>(minorUnits);

  std::uint64_t divisor = 1;
  for (std::int8_t i = 0; i < exp; ++i) {
    divisor *= 10;
  }

  const std::uint64_t whole = magnitude / divisor;
  const std::uint64_t frac = magnitude % divisor;

  // 小數部分要補足位數：USD 的 5 分要印成 "05" 而不是 "5"。
  std::string fracStr = std::to_string(frac);
  while (fracStr.size() < static_cast<std::size_t>(exp)) {
    fracStr.insert(fracStr.begin(), '0');
  }

  std::string out;
  if (negative) {
    out += '-';
  }
  out += std::to_string(whole);
  out += '.';
  out += fracStr;
  return out;
}

std::string_view toString(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok:
      return "OK";
    case ErrorCode::InvalidAmount:
      return "INVALID_AMOUNT";
    case ErrorCode::SelfTransfer:
      return "SELF_TRANSFER";
    case ErrorCode::UnknownCurrency:
      return "UNKNOWN_CURRENCY";
    case ErrorCode::AccountNotFound:
      return "ACCOUNT_NOT_FOUND";
    case ErrorCode::AccountClosed:
      return "ACCOUNT_CLOSED";
    case ErrorCode::CurrencyMismatch:
      return "CURRENCY_MISMATCH";
    case ErrorCode::InsufficientFunds:
      return "INSUFFICIENT_FUNDS";
    case ErrorCode::AmountOverflow:
      return "AMOUNT_OVERFLOW";
    case ErrorCode::DuplicateAccount:
      return "DUPLICATE_ACCOUNT";
    case ErrorCode::SocketError:
      return "SOCKET_ERROR";
    case ErrorCode::BindFailed:
      return "BIND_FAILED";
    case ErrorCode::ListenFailed:
      return "LISTEN_FAILED";
    case ErrorCode::EpollError:
      return "EPOLL_ERROR";
    case ErrorCode::ConnectionClosed:
      return "CONNECTION_CLOSED";
    case ErrorCode::MalformedFrame:
      return "MALFORMED_FRAME";
    case ErrorCode::UnknownMessageType:
      return "UNKNOWN_MESSAGE_TYPE";
    case ErrorCode::UnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case ErrorCode::FrameTooLarge:
      return "FRAME_TOO_LARGE";
    case ErrorCode::IntegerNotString:
      return "INTEGER_NOT_STRING";
    case ErrorCode::MissingField:
      return "MISSING_FIELD";
    case ErrorCode::ServerBusy:
      return "SERVER_BUSY";
    case ErrorCode::Count:
      break;
  }
  return "UNKNOWN_ERROR";
}

ErrorCode errorCodeFromString(std::string_view name) noexcept {
  // 走訪全部合法值比對名字。這樣「名字」只在上面的 switch 出現一次，
  // 不會有「encode 用新名字、decode 只認得舊名字」的漂移。
  //
  // 成本是 O(n) 的字串比較，但這條路徑只在 client 解碼錯誤回應時走到 ——
  // 而錯誤回應本來就是罕見路徑。用不對稱的成本換掉一整類漂移，很划算。
  const auto count = static_cast<std::uint16_t>(ErrorCode::Count);
  for (std::uint16_t i = 0; i < count; ++i) {
    const auto candidate = static_cast<ErrorCode>(i);
    if (toString(candidate) == name) {
      return candidate;
    }
  }
  return ErrorCode::Count;
}

}  // namespace ledger
