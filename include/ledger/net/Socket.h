#pragma once

#include <ledger/common/Result.h>

#include <cstdint>
#include <string>
#include <utility>

namespace ledger::net {

// ---------------------------------------------------------------------------
// FileDescriptor —— 檔案描述元的 RAII 包裝。
//
// 為什麼需要：fd 是一種資源，用完必須 close()。手寫 close() 的問題是
// 中途 return 或丟例外時很容易漏掉，而 fd 洩漏的症狀是
// 「跑幾小時之後 accept() 開始回 EMFILE」—— 極難追。
//
// 讓解構子負責關閉，漏不掉。可移動但不可複製：
// 複製 fd 會讓兩個物件都以為自己擁有它，解構時 close 兩次。
// ---------------------------------------------------------------------------
class FileDescriptor {
 public:
  FileDescriptor() = default;
  explicit FileDescriptor(int fd) noexcept : fd_(fd) {}

  ~FileDescriptor() { reset(); }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  FileDescriptor(FileDescriptor&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  /// 交出所有權，之後解構子不再關閉它。
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

  void reset() noexcept;

 private:
  int fd_ = -1;
};

// ---------------------------------------------------------------------------
// socket 相關的小工具。全部只在 Linux 上編譯。
// ---------------------------------------------------------------------------

/// 把 fd 設成 non-blocking。
///
/// ★ 這是 epoll 事件驅動架構的前提。
///   若 fd 是阻塞的，read() 在沒資料時會把整個 event loop 執行緒掛住 ——
///   一條連線就能讓所有連線一起停擺。
///   non-blocking 之下 read() 會立刻回 -1 並把 errno 設成 EAGAIN，
///   意思是「現在沒有資料，等下次事件通知」。
Status setNonBlocking(int fd);

/// 關閉 Nagle 演算法。
///
/// Nagle 會把小封包攢起來一起送，以減少網路上的小封包數量。
/// 對請求-回應型的協定（我們正是）這會憑空加上數十毫秒的延遲，
/// 因為回應在等一個永遠不會來的「下一個小封包」。
Status setTcpNoDelay(int fd);

/// SO_REUSEADDR：讓伺服器重啟時不必等 TIME_WAIT 結束。
///
/// 沒有這個的話，剛關掉的伺服器要等最多兩分鐘才能重新 bind 同一個 port，
/// 開發時每改一行就要等兩分鐘，完全不可行。
Status setReuseAddr(int fd);

/// 建立一個已 bind 並 listen 的 non-blocking TCP socket。
Result<int> createListenSocket(std::uint16_t port, int backlog = 512);

/// 取出並清除 socket 上的暫存錯誤（SO_ERROR）。
///
/// epoll 回報 EPOLLERR 時只說「出事了」，不說是什麼事。
/// 真正的 errno 要用這個函式去 socket 裡撈。
[[nodiscard]] int takeSocketError(int fd);

/// "127.0.0.1:54321"，給日誌用。
[[nodiscard]] std::string peerAddress(int fd);

}  // namespace ledger::net
