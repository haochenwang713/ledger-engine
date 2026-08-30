#pragma once

#include <cassert>
#include <cstdint>
#include <string_view>
#include <type_traits>
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

  // --- 協定層（Stage 5）---
  //
  // ⚠ 新增錯誤碼一律往這個 enum 的「最後面」加。
  //   數值會被 BinaryCodec 直接序列化成 u16 送給 client，
  //   在中間插入一個新值會讓所有既有 client 的錯誤碼整組錯位。
  MalformedFrame,  ///< 位元組不成一則合法訊息（長度不符、欄位讀不完、JSON 語法錯）
  UnknownMessageType,  ///< 型別代碼不認識，或方向不對（請求裡出現回應型別）
  UnsupportedVersion,  ///< 協定版本不是 kProtocolVersion
  FrameTooLarge,       ///< 單則訊息超過 kMaxFrameSize
  IntegerNotString,    ///< JSON 的整數欄位不是字串（見 JsonCodec 的 2^53 說明）
  MissingField,        ///< JSON 物件少了必要的 key
  ServerBusy,          ///< 佇列已滿，背壓生效（Stage 5b 起使用）

  /// 錯誤碼總數。必須永遠是最後一個。
  ///
  /// 它讓 errorCodeFromString() 可以走訪全部的值去做反查，
  /// 而不需要維護第二張名字對照表 —— 少一張表就少一個會漂移的地方。
  Count,
};

[[nodiscard]] std::string_view toString(ErrorCode code) noexcept;

/// toString() 的反向操作。認不得的名字回 ErrorCode::Count。
///
/// 只有 client 端解碼錯誤回應時才需要（JSON 的 code 欄位是名字而非數字）。
/// 實作方式是走訪 0..Count 比對 toString()，所以名字表實體上只有一份。
[[nodiscard]] ErrorCode errorCodeFromString(std::string_view name) noexcept;

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
  // ⚠ T 不能是 ErrorCode 本身。
  //
  //   Result<T> 靠兩個隱式建構子區分成功與失敗：Result(T) 和 Result(ErrorCode)。
  //   當 T == ErrorCode 時這兩個簽章完全相同 —— 多載衝突，而且就算能編譯，
  //   Result<ErrorCode>{SomeError} 到底代表「成功地得到一個錯誤碼」還是
  //   「失敗了」也沒有答案。這個型別在語意上就是矛盾的。
  //
  //   直接 static_assert 擋掉，是為了讓誤用時看到這一行，而不是六十行的
  //   template 展開訊息。要傳遞一個可能失敗的 ErrorCode，請用
  //   「bool + 輸出參數」的形式（見 JsonCodec 的 fromJsonValue）。
  static_assert(!std::is_same_v<T, ErrorCode>,
                "Result<ErrorCode> 在語意上是矛盾的：成功值與錯誤值同型別，"
                "無法區分。請改用 bool + 輸出參數。");

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
