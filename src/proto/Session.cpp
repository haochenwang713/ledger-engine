#include <ledger/proto/Session.h>

#include <utility>

namespace ledger::proto {

Session::Outcome Session::drain(std::string_view input) const {
  Outcome out;
  std::string_view rest = input;

  for (;;) {
    const FrameView fv = splitter_.next(rest);

    // 半包：剩下的位元組留在緩衝裡，等下一次讀取事件補齊。
    // 這裡直接跳出，consumed 不包含它們。
    if (fv.status == FrameStatus::NeedMore) {
      break;
    }

    // 框架失敗：位元組流已經無法對齊，沒有辦法找到下一則訊息的開頭。
    // 回一個錯誤讓 client 知道原因，然後由呼叫端關閉連線。
    //
    // reqId 用 0，因為我們根本沒能讀到表頭 —— 這則錯誤不對應任何
    // 一個具體的請求，它是關於「這條連線」的。
    if (fv.status == FrameStatus::Error) {
      out.errors.push_back(makeError(0, fv.error, "framing error, closing connection"));
      out.fatal = true;
      break;
    }

    // 空的 frame：NDJSON 上就是使用者按了 Enter。
    //
    // 嚴格來說這是一則不合法的訊息，但 JSON port 的存在意義就是
    // 讓人用 nc 手動測試，因為多按一次 Enter 就被斷線很沒有道理。
    // 靜靜跳過，不回錯誤也不算進統計。
    if (fv.frame.empty()) {
      rest.remove_prefix(fv.consumed);
      out.consumed += fv.consumed;
      continue;
    }

    Result<RequestEnvelope> decoded = codec_.decodeRequest(fv.frame);
    if (decoded) {
      out.requests.push_back(std::move(decoded).value());
    } else {
      // 解碼失敗但框架完整 —— 這一則丟掉，下一則照常處理。
      //
      // reqId 同樣是 0：解碼失敗代表我們連表頭都不能信任。
      // 這是一個已知的取捨 —— binary 協定其實可以在 payload 解析
      // 失敗時仍然回報表頭裡的 reqId，但那需要把解碼拆成兩階段，
      // 而換來的只是錯誤訊息更好對應。目前不值得。
      out.errors.push_back(makeError(0, decoded.error(), "could not decode message"));
    }

    rest.remove_prefix(fv.consumed);
    out.consumed += fv.consumed;
  }

  return out;
}

}  // namespace ledger::proto
