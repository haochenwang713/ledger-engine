#include <ledger/net/Buffer.h>

#include <algorithm>
#include <cassert>

namespace ledger::net {

void Buffer::ensureWritable(std::size_t len) {
  if (writableBytes() < len) {
    makeSpace(len);
  }
  assert(writableBytes() >= len);
}

void Buffer::retrieve(std::size_t len) noexcept {
  if (len >= readableBytes()) {
    retrieveAll();
    return;
  }
  // 只把讀游標往前推 —— O(1)，不搬移任何資料。
  // 這就是為什麼要有「已讀完」那一段的原因。
  readIdx_ += len;
}

std::string Buffer::retrieveAsString(std::size_t len) {
  const std::size_t n = std::min(len, readableBytes());
  std::string out(peek(), n);
  retrieve(n);
  return out;
}

void Buffer::append(const void* data, std::size_t len) {
  ensureWritable(len);
  std::memcpy(beginWrite(), data, len);
  hasWritten(len);
}

void Buffer::makeSpace(std::size_t len) {
  // 策略一：回收前面已消費的空間。
  //
  // 條件是「前面空出來的 + 後面剩下的」總和夠用。若成立，把待處理的資料
  // 搬回開頭就好，不必重新配置記憶體。
  //
  // 一條持續收發的連線幾乎永遠走這條路徑：讀游標一路往前推到接近尾端，
  // 然後整理一次，如此循環，同一塊記憶體重複使用。
  if (prependableBytes() + writableBytes() >= len + kPrepend) {
    const std::size_t readable = readableBytes();
    std::copy(storage_.begin() + static_cast<std::ptrdiff_t>(readIdx_),
              storage_.begin() + static_cast<std::ptrdiff_t>(writeIdx_),
              storage_.begin() + static_cast<std::ptrdiff_t>(kPrepend));
    readIdx_ = kPrepend;
    writeIdx_ = readIdx_ + readable;
    return;
  }

  // 策略二：真的不夠，擴充。交給 vector 決定成長倍率。
  storage_.resize(writeIdx_ + len);
}

}  // namespace ledger::net
