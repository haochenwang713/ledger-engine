#pragma once

#include <cassert>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

namespace ledger {

/// 引擎的錯誤碼。之後 Stage 5 的協定會把這些直接對映成回應碼。
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // --- 請求本身不合法 ---
  InvalidAmount,    ///< 金額 <= 0
  SelfTransfer,     ///< 來源與目的是同一個帳戶
  UnknownCurrency,  ///< 幣別代碼不認識

  // --- 帳戶狀態問題 ---
  AccountNotFound,
  AccountClosed,
  CurrencyMismatch,   ///< 兩腳幣別不同（v1 不做匯兌）
  InsufficientFunds,  ///< 餘額不足 —— 這就是擋 double-spending 的地方

  // --- 算術 ---
  AmountOverflow,  ///< int64 加減會溢位

  // --- 其他 ---
  DuplicateAccount,  ///< 同一 owner 同一幣別已有帳戶

  // --- 網路層（Stage 4）---
  SocketError,       ///< socket 系統呼叫失敗，詳情看 errno
  BindFailed,        ///< bind() 失敗，通常是 port 已被占用
  ListenFailed,      ///< listen() 失敗
  EpollError,        ///< epoll_create / epoll_ctl / epoll_wait 失敗
  ConnectionClosed,  ///< 對端關閉了連線
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

// ---------------------------------------------------------------------------
// Result<T> —— 「成功帶值，失敗帶錯誤碼」的最小實作。
//
// 為什麼不用例外：轉帳失敗（餘額不足、幣別不符）是「預期中會發生的事」，
// 不是例外狀況。在每秒上萬筆的熱路徑上丟例外既慢又難推理。
// 而且錯誤要能一路傳回 TCP 回應，用回傳值最直接。
//
// 為什麼不用 std::expected：那是 C++23，本專案定在 C++20。
// 這個實作介面刻意做得跟 std::expected 相近，未來升級時容易替換。
// ---------------------------------------------------------------------------
template <typename T>
class Result {
 public:
  /// 成功
  Result(T value) : storage_(std::move(value)) {}  // NOLINT(google-explicit-constructor)

  /// 失敗
  Result(ErrorCode error) : storage_(error) {  // NOLINT(google-explicit-constructor)
    assert(error != ErrorCode::Ok && "失敗的 Result 不該帶 Ok");
  }

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] ErrorCode error() const noexcept {
    return ok() ? ErrorCode::Ok : std::get<ErrorCode>(storage_);
  }

  /// 只有 ok() 為真時才能呼叫。
  [[nodiscard]] const T& value() const& {
    assert(ok() && "對失敗的 Result 取值");
    return std::get<T>(storage_);
  }
  [[nodiscard]] T&& value() && {
    assert(ok() && "對失敗的 Result 取值");
    return std::get<T>(std::move(storage_));
  }

  /// 失敗時回傳替代值，省掉呼叫端的 if。
  [[nodiscard]] T valueOr(T fallback) const {
    return ok() ? std::get<T>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, ErrorCode> storage_;
};

/// 沒有回傳值的操作用這個。
class Status {
 public:
  Status() : code_(ErrorCode::Ok) {}
  Status(ErrorCode code) : code_(code) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
  explicit operator bool() const noexcept { return ok(); }
  [[nodiscard]] ErrorCode error() const noexcept { return code_; }

 private:
  ErrorCode code_;
};

}  // namespace ledger
