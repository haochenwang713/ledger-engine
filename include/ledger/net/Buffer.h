#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ledger::net {

// ---------------------------------------------------------------------------
// Buffer —— 可成長的位元組緩衝，雙游標設計。
//
// 這個類別完全沒有系統呼叫，所以在 macOS 上也能編譯與測試 ——
// 只有真正碰 socket 的部分才需要 Linux。
//
// 為什麼需要它：TCP 是位元組串流，沒有訊息邊界。一次 read() 可能拿到
//   半個封包（半包）、剛好一個、或三個半（黏包）。所以必須有個地方
//   把讀到的位元組累積起來，等湊滿一個完整的 frame 再處理。
//
// 記憶體佈局：
//
//   +----------------+------------------+------------------+
//   |   已讀完可丟   |   待處理的資料   |   可寫入的空間   |
//   +----------------+------------------+------------------+
//   0            readIdx_          writeIdx_          size()
//
//   readIdx_  —— 資料從這裡開始（前面的已經被消費掉了）
//   writeIdx_ —— 新資料寫到這裡
//
// 為什麼要留「已讀完」那一段而不是每次都 memmove 到開頭：
//   每消費一個 frame 就搬移整個緩衝是 O(n) 的浪費。讓 readIdx_ 往前走
//   是 O(1)。等到空間真的不夠時才一次整理（見 makeSpace）。
// ---------------------------------------------------------------------------
class Buffer {
 public:
  /// 預留在最前面的空間。之後 Stage 5 要在資料前面補上長度前綴時，
  /// 可以直接往前寫而不用搬移整塊資料。
  static constexpr std::size_t kPrepend = 8;
  static constexpr std::size_t kInitialSize = 1024;

  explicit Buffer(std::size_t initialSize = kInitialSize)
      : storage_(kPrepend + initialSize), readIdx_(kPrepend), writeIdx_(kPrepend) {}

  /// 還沒被消費的位元組數。
  [[nodiscard]] std::size_t readableBytes() const noexcept { return writeIdx_ - readIdx_; }

  /// 尾端還能寫入多少而不需要重新配置。
  [[nodiscard]] std::size_t writableBytes() const noexcept { return storage_.size() - writeIdx_; }

  /// 前面被消費掉、可以回收的空間。
  [[nodiscard]] std::size_t prependableBytes() const noexcept { return readIdx_; }

  [[nodiscard]] bool empty() const noexcept { return readableBytes() == 0; }

  /// 指向待處理資料的開頭。只在下一次修改緩衝之前有效。
  [[nodiscard]] const char* peek() const noexcept { return storage_.data() + readIdx_; }

  /// 指向可寫入區的開頭。呼叫端寫完之後要呼叫 hasWritten()。
  [[nodiscard]] char* beginWrite() noexcept { return storage_.data() + writeIdx_; }

  /// 確保尾端至少有 len 個位元組可寫。必要時整理或擴充。
  void ensureWritable(std::size_t len);

  /// 告訴緩衝「我剛剛往 beginWrite() 寫了 len 個位元組」。
  void hasWritten(std::size_t len) noexcept { writeIdx_ += len; }

  /// 消費掉前面 len 個位元組。
  void retrieve(std::size_t len) noexcept;

  /// 全部消費掉，游標回到起點。
  void retrieveAll() noexcept {
    readIdx_ = kPrepend;
    writeIdx_ = kPrepend;
  }

  /// 取出前 len 個位元組並消費掉。
  [[nodiscard]] std::string retrieveAsString(std::size_t len);

  /// 取出全部並消費掉。
  [[nodiscard]] std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }

  /// 不消費，只看一眼。Stage 5 的切包會用到。
  [[nodiscard]] std::string_view view() const noexcept {
    return std::string_view(peek(), readableBytes());
  }

  /// 附加資料到尾端。
  void append(const void* data, std::size_t len);
  void append(std::string_view sv) { append(sv.data(), sv.size()); }

  /// 目前配置的總容量（含前置空間），給測試觀察成長行為用。
  [[nodiscard]] std::size_t capacity() const noexcept { return storage_.size(); }

 private:
  /// 讓尾端至少有 len 個位元組可寫。
  ///
  /// 兩種策略，先試便宜的那個：
  ///   1. 如果「已讀完的空間 + 尾端空間」加起來就夠了 → 把資料搬回開頭，
  ///      不用重新配置。連線持續收發時，這條路徑會一直重複使用同一塊記憶體。
  ///   2. 真的不夠 → resize，讓 vector 自己去成長。
  void makeSpace(std::size_t len);

  std::vector<char> storage_;
  std::size_t readIdx_;
  std::size_t writeIdx_;
};

}  // namespace ledger::net
