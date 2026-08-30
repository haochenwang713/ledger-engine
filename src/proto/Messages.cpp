#include <ledger/proto/Messages.h>

#include <utility>
#include <variant>

namespace ledger::proto {

std::string_view nameOfMsgType(MsgType type) noexcept {
  for (const auto& entry : kMsgTypeNames) {
    if (entry.type == type) {
      return entry.name;
    }
  }
  return "unknown";
}

Result<MsgType> msgTypeFromName(std::string_view name) noexcept {
  for (const auto& entry : kMsgTypeNames) {
    if (entry.name == name) {
      return entry.type;
    }
  }
  return ErrorCode::UnknownMessageType;
}

MsgType typeOf(const Request& req) noexcept {
  // std::visit 走訪 variant 的實際型別，從它的 kType 常數取代碼。
  // 好處是「訊息型別 → 代碼」的對應寫在訊息 struct 自己身上，
  // 不需要在這裡再維護一個 switch。
  return std::visit([](const auto& msg) { return std::decay_t<decltype(msg)>::kType; }, req);
}

MsgType typeOf(const Response& resp) noexcept {
  return std::visit([](const auto& msg) { return std::decay_t<decltype(msg)>::kType; }, resp);
}

ResponseEnvelope makeError(std::uint32_t reqId, ErrorCode code, std::string message) {
  return ResponseEnvelope{reqId, ErrorResp{code, std::move(message)}};
}

}  // namespace ledger::proto
