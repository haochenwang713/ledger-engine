#pragma once

#include <ledger/common/Result.h>
#include <ledger/proto/Messages.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ledger::proto {

// ---------------------------------------------------------------------------
// FrameSplitter —— 從一串位元組裡切出「一則完整訊息」。
//
// 職責邊界：它只認得「一則訊息從哪到哪」，完全不知道訊息裡面是什麼。
// Codec 剛好相反。這是兩個 port 能自由組合 framing 與編碼的原因。
//
// ★ 刻意設計成純函式：輸入 string_view，輸出「切到哪」，不持有狀態、
//   不碰 Buffer。好處有三個：
//     1. 不需要 Buffer 就能單元測試，連 net/ 都不用連結
//     2. 沒有狀態就沒有「上次呼叫留下的殘渣」這類 bug
//     3. 呼叫端自己決定何時消費，生命週期一目了然
//
// 呼叫端的標準迴圈（Stage 5c 會用到）：
//
//   for (;;) {
//     const FrameView fv = splitter.next(buffer.view());
//     if (fv.status == FrameStatus::NeedMore) break;         // 半包，等下次
//     if (fv.status == FrameStatus::Error)   { conn->close(); break; }
//     handle(fv.frame);                                       // frame 仍有效
//     buffer.retrieve(fv.consumed);                           // 這行之後才失效
//   }
//
// ⚠ fv.frame 指向呼叫端傳進來的那塊記憶體。它在 buffer 下一次被修改
//   之前有效 —— 也就是說 handle() 必須在 retrieve() 之前做完。
//   上面的迴圈順序就是這個契約的具體樣子。
// ---------------------------------------------------------------------------

enum class FrameStatus : std::uint8_t {
  Ok,        ///< 切出了一則完整訊息
  NeedMore,  ///< 資料不足（半包）。原封不動留著，等更多位元組
  Error,     ///< 協定違規（超長、長度欄位不合理）。連線必須關閉
};

struct FrameView {
  FrameStatus status{FrameStatus::NeedMore};

  /// 一則訊息的位元組，不含框架開銷（長度前綴／換行都已剝掉）。
  /// 只在來源緩衝被修改之前有效。
  std::string_view frame{};

  /// 應該從緩衝消費掉多少位元組（含框架開銷）。
  /// status == Ok 時才有意義。
  std::size_t consumed{0};

  ErrorCode error{ErrorCode::Ok};
};

class FrameSplitter {
 public:
  explicit FrameSplitter(std::size_t maxFrameSize = kMaxFrameSize) noexcept
      : maxFrameSize_(maxFrameSize) {}

  FrameSplitter(const FrameSplitter&) = delete;
  FrameSplitter& operator=(const FrameSplitter&) = delete;
  virtual ~FrameSplitter() = default;

  [[nodiscard]] virtual FrameView next(std::string_view input) const = 0;

  [[nodiscard]] std::size_t maxFrameSize() const noexcept { return maxFrameSize_; }

 protected:
  std::size_t maxFrameSize_;
};

// ---------------------------------------------------------------------------
// 長度前綴（binary port，:9000）
//
//   [len:u32 big-endian][ ... len 個位元組 ... ]
//
//   len 涵蓋它後面的全部內容，不含自己。
//   frame 就是那 len 個位元組（也就是 binary 表頭 + payload）。
//
// 為什麼 len 不含自己：因為讀到 len 之後，「還要再讀幾個位元組」
// 就是 len 本身，不用再減 4。少一次算術就少一次 off-by-one 的機會。
// ---------------------------------------------------------------------------
class LengthPrefixSplitter final : public FrameSplitter {
 public:
  static constexpr std::size_t kPrefixSize = 4;

  explicit LengthPrefixSplitter(std::size_t maxFrameSize = kMaxFrameSize) noexcept
      : FrameSplitter(maxFrameSize) {}

  [[nodiscard]] FrameView next(std::string_view input) const override;
};

// ---------------------------------------------------------------------------
// 換行分隔（JSON port，:9001 —— NDJSON，一行一個 JSON 物件）
//
// 選換行而不是長度前綴的理由只有一個，但很重要：
//   nc localhost 9001 之後直接打字就能測。
// 這是 JSON port 存在的主要價值 —— 它是給人用的，不是給機器用的。
//
// ⚠ 代價是沒有長度上界，所以 maxFrameSize 的檢查在這裡不是防禦性程式碼，
//   而是必要條件。一個永遠不送換行的 client 會讓緩衝無限成長。
// ---------------------------------------------------------------------------
class NewlineSplitter final : public FrameSplitter {
 public:
  explicit NewlineSplitter(std::size_t maxFrameSize = kMaxFrameSize) noexcept
      : FrameSplitter(maxFrameSize) {}

  [[nodiscard]] FrameView next(std::string_view input) const override;
};

}  // namespace ledger::proto
