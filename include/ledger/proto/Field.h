#pragma once

#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// Field — a static description of one message field.
//
// This is the foundation of the protocol layer and the only clever code in it.
// It exists for exactly one reason: so that every field is declared once.
//
// Without it, adding a field means editing four places (binary encode, binary
// decode, JSON encode, JSON decode), and missing one of the four is silent —
// it compiles, that encoding's tests pass, and only the *other* encoding is
// quietly short a field. It might be weeks before anyone notices.
//
// With it, both codecs are generic code that walks fields() and dispatches on
// the field type. The list of fields physically exists in one place: the
// message struct itself.
//
// This layer is deliberately tiny — one struct and two helpers, no concepts,
// no CRTP, no macros. Template code produces error messages that are hard to
// read, so the maintainability it buys has to clearly outweigh the cost of
// reading it. Beyond this, it would not.
// ---------------------------------------------------------------------------

/// A field is its name in the protocol plus a pointer to the member.
///
///   Field{"amount", &TransferReq::amount}
///
/// The name determines the JSON key. The *position* within fields() determines
/// the binary order.
///
/// Reordering fields() silently breaks binary compatibility while leaving JSON
/// untouched, since JSON matches on names. The golden hex table in
/// test_codec.cpp exists to catch exactly that.
template <typename Class, typename Member>
struct Field {
  std::string_view name;
  Member Class::*member;
};

// C++20 aggregate CTAD, so Field{"x", &T::m} needs no template arguments.
template <typename Class, typename Member>
Field(std::string_view, Member Class::*) -> Field<Class, Member>;

/// Call fn with each of Msg's field descriptors, in order.
///
/// The standard guarantees left-to-right evaluation for a comma fold. For
/// binary encoding that is a correctness requirement, not a style preference:
/// get the order wrong and the whole payload is misaligned.
template <typename Msg, typename Fn>
constexpr void forEachField(Fn&& fn) {
  std::apply([&fn](auto&&... field) { (fn(field), ...); }, Msg::fields());
}

/// How many fields Msg declares.
template <typename Msg>
[[nodiscard]] constexpr std::size_t fieldCount() {
  return std::tuple_size_v<decltype(Msg::fields())>;
}

/// For exhaustiveness checks in `if constexpr` chains: an unsupported field
/// type becomes a compile error rather than a field that is silently skipped.
template <typename>
inline constexpr bool kAlwaysFalse = false;

}  // namespace ledger::proto
