#pragma once

#include <ledger/proto/Codec.h>
#include <ledger/proto/FrameSplitter.h>
#include <ledger/proto/Messages.h>

#include <cstddef>
#include <string_view>
#include <vector>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// Session —— 把一串位元組變成一串請求。
//
// 這是 Stage 5c 唯一有邏輯的地方，也刻意做成平台無關：它不知道 socket、
// 不知道執行緒、不持有狀態。輸入是「目前累積的位元組」，輸出是
// 「解出了哪些請求、要消費幾個位元組、有沒有要立刻回的錯誤」。
//
// 為什麼把這個迴圈單獨抽出來，而不是寫在 ConnectionContext 裡：
// 半包、黏包、解碼失敗、框架失敗這四種情況的組合是這一層最容易寫錯的
// 地方，而 ConnectionContext 依賴 Connection（Linux 專屬）。抽出來之後
// 這段邏輯在 macOS 上也能完整測試，而且測試不需要任何 socket。
//
// ★ 兩種失敗的處理方式完全不同
//
//   解碼失敗（frame 切得出來，但內容看不懂）
//     → 框架邊界是已知的，可以跳過這一則繼續下一則。
//       回一個 ERROR 給 client，連線繼續活著。
//
//   框架失敗（長度欄位不合理、單則訊息超長）
//     → 我們不知道下一則訊息從哪裡開始，位元組流已經無法對齊。
//       繼續讀下去只會產生垃圾。唯一正確的做法是關閉連線。
//
//   混淆這兩者的後果：把框架失敗當成可恢復的，會讓連線陷入
//   「解出垃圾 → 回錯誤 → 再解出垃圾」的無限迴圈。
// ---------------------------------------------------------------------------
class Session {
 public:
  struct Outcome {
    /// 成功解出、應該送去 worker 處理的請求。
    std::vector<RequestEnvelope> requests;

    /// 應該立刻回給 client 的錯誤（不經過 worker）。
    std::vector<ResponseEnvelope> errors;

    /// 應該從緩衝消費掉多少位元組。半包的部分不算在內。
    std::size_t consumed{0};

    /// 位元組流已經無法對齊，連線必須關閉。
    bool fatal{false};
  };

  /// splitter 與 codec 都是無狀態的，由伺服器持有一份共用。
  Session(const FrameSplitter& splitter, const Codec& codec) noexcept
      : splitter_(splitter), codec_(codec) {}

  /// 從累積的位元組裡盡可能多切出請求。
  ///
  /// ⚠ 呼叫端必須在處理完 outcome 之後才 retrieve(consumed)。
  ///   Outcome 裡的 RequestEnvelope 是值型別（已經複製過了），
  ///   所以它們不依賴輸入緩衝的存活 —— 這是刻意的，因為那些請求
  ///   會被丟進佇列，可能在幾毫秒後才被別的執行緒讀到。
  [[nodiscard]] Outcome drain(std::string_view input) const;

  [[nodiscard]] CodecTag codecTag() const noexcept { return codec_.tag(); }

 private:
  const FrameSplitter& splitter_;
  const Codec& codec_;
};

}  // namespace ledger::proto
