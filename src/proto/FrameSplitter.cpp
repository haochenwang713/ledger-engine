#include <ledger/proto/FrameSplitter.h>

#include <cstring>

namespace ledger::proto {
namespace {

/// 從 4 個位元組讀出 big-endian 的 u32。
///
/// 為什麼是 big-endian：這是網路位元組序，hexdump 和 Wireshark 讀起來
/// 跟寫下來的順序一致。x86 是 little-endian，所以一定要明確轉換 ——
/// 直接 memcpy 一個 u32 在 x86 上「看起來會動」，換到別的架構就壞掉。
[[nodiscard]] std::uint32_t readU32BE(const char* p) noexcept {
  const auto* b = reinterpret_cast<const unsigned char*>(p);
  return (static_cast<std::uint32_t>(b[0]) << 24) | (static_cast<std::uint32_t>(b[1]) << 16) |
         (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]);
}

}  // namespace

FrameView LengthPrefixSplitter::next(std::string_view input) const {
  FrameView out;

  // 連長度欄位都還沒收齊 —— 這是最常見的半包情況。
  if (input.size() < kPrefixSize) {
    out.status = FrameStatus::NeedMore;
    return out;
  }

  const std::uint32_t len = readU32BE(input.data());

  // ⚠ 這個檢查必須在「等更多資料」之前做。
  //
  //   如果先判斷 input.size() < len 就 return NeedMore，那麼一個惡意
  //   client 送 [len=0xFFFFFFFF] 之後就閉嘴，我們會永遠等待，而緩衝
  //   會一直被要求成長。長度前綴協定最經典的資源耗盡攻擊。
  //
  //   順序是：先驗證長度合不合理，再判斷資料夠不夠。
  if (len > maxFrameSize_) {
    out.status = FrameStatus::Error;
    out.error = ErrorCode::FrameTooLarge;
    return out;
  }

  // len 至少要裝得下 binary 表頭（type + ver + reqId = 8 位元組）。
  // 這個下界檢查交給 BinaryCodec 做 —— 這裡只管 framing，
  // 不該知道表頭有多大。但長度為 0 是明確的協定違規，擋在這裡。
  if (len == 0) {
    out.status = FrameStatus::Error;
    out.error = ErrorCode::MalformedFrame;
    return out;
  }

  const std::size_t total = kPrefixSize + len;
  if (input.size() < total) {
    out.status = FrameStatus::NeedMore;
    return out;
  }

  out.status = FrameStatus::Ok;
  out.frame = input.substr(kPrefixSize, len);
  out.consumed = total;
  return out;
}

FrameView NewlineSplitter::next(std::string_view input) const {
  FrameView out;

  const std::size_t nl = input.find('\n');

  if (nl == std::string_view::npos) {
    // 還沒看到換行。此時累積的位元組已經超過上限的話，就永遠不可能
    // 成為一則合法訊息了 —— 繼續等只是讓記憶體白白成長。當場切斷。
    if (input.size() > maxFrameSize_) {
      out.status = FrameStatus::Error;
      out.error = ErrorCode::FrameTooLarge;
      return out;
    }
    out.status = FrameStatus::NeedMore;
    return out;
  }

  if (nl > maxFrameSize_) {
    out.status = FrameStatus::Error;
    out.error = ErrorCode::FrameTooLarge;
    return out;
  }

  std::string_view line = input.substr(0, nl);

  // 相容 CRLF。用 telnet 或 Windows 的 client 測試時會送 \r\n，
  // 沒有這一行的話 JSON 解析會因為結尾多一個 \r 而失敗，
  // 而錯誤訊息會指向 JSON 語法，讓人往完全錯誤的方向查。
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }

  out.status = FrameStatus::Ok;
  out.frame = line;
  out.consumed = nl + 1;  // 換行本身也要消費掉
  return out;
}

}  // namespace ledger::proto
