#pragma once

#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// Field —— 一個訊息欄位的靜態描述。
//
// 這是整個 proto 層的地基，也是唯一一處「聰明」的程式碼。存在的理由只有一個：
// 讓每個欄位在整份程式裡只被宣告一次。
//
// 沒有它的話，加一個欄位要改四個地方（binary encode / binary decode /
// json encode / json decode），而漏掉其中一個是靜默的 —— 編譯過、
// 該編碼的測試也過，只有另一種編碼少一個欄位，可能幾週後才被發現。
//
// 有了它，兩個 codec 都只是「走訪 fields() 並依型別分派」的泛型程式碼，
// 欄位清單物理上只存在於訊息 struct 自己身上。
//
// ⚠ 這一層刻意保持極小 —— 只有一個 struct 和兩個 helper，沒有 concept、
//   沒有 CRTP、沒有巨集。template 程式碼的編譯錯誤訊息很難讀，
//   它換來的維護性必須明顯大於它增加的閱讀成本，否則就是負債。
// ---------------------------------------------------------------------------

/// 一個欄位 = 它在協定裡的名字 + 指向它的成員指標。
///
///   Field{"amount", &TransferReq::amount}
///
/// name 決定 JSON 的 key；在 fields() 裡的「位置」決定 binary 的順序。
///
/// ⚠ 重排 fields() 裡的順序會靜默破壞 binary 相容性（JSON 不受影響，
///   因為它靠名字對應）。test_codec.cpp 的 golden hex 表格就是防這件事的。
template <typename Class, typename Member>
struct Field {
  std::string_view name;
  Member Class::*member;
};

// C++20 的 aggregate CTAD 讓 Field{"x", &T::m} 不用寫模板參數。
template <typename Class, typename Member>
Field(std::string_view, Member Class::*) -> Field<Class, Member>;

/// 依序對 Msg 的每個欄位描述呼叫 fn。
///
/// 逗號 fold 的求值順序由標準保證是由左到右 —— 這一點對 binary 編碼
/// 是正確性關鍵，欄位順序錯了整個 payload 就錯位了。
template <typename Msg, typename Fn>
constexpr void forEachField(Fn&& fn) {
  std::apply([&fn](auto&&... field) { (fn(field), ...); }, Msg::fields());
}

/// Msg 宣告了幾個欄位。
template <typename Msg>
[[nodiscard]] constexpr std::size_t fieldCount() {
  return std::tuple_size_v<decltype(Msg::fields())>;
}

/// 給 if constexpr 的窮盡檢查用：碰到沒支援的欄位型別時在編譯期報錯，
/// 而不是安靜地漏掉那個欄位。
template <typename>
inline constexpr bool kAlwaysFalse = false;

}  // namespace ledger::proto
