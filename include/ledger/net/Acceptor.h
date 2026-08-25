#pragma once

#include <ledger/common/Result.h>
#include <ledger/net/EventLoop.h>
#include <ledger/net/Socket.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace ledger::net {

/// 有新連線進來時呼叫。fd 的所有權交給回呼。
using NewConnectionCallback = std::function<void(int fd, std::string peer)>;

// ---------------------------------------------------------------------------
// Acceptor —— 負責 listen socket 與接受新連線。
//
// ★ 同樣受制於邊緣觸發的鐵律：收到可讀事件就要一直 accept() 到 EAGAIN。
//
//   少接一個會怎樣？那條連線會留在 kernel 的 accept 佇列裡，而狀態沒有
//   再改變，epoll 不會再通知。client 以為連上了（三次握手其實已經完成），
//   送出請求之後永遠等不到回應。
//
//   跟 Connection::handleRead 是同一個坑，只是換了個地方。
// ---------------------------------------------------------------------------
class Acceptor {
 public:
  Acceptor(EventLoop& loop, std::uint16_t port);

  Acceptor(const Acceptor&) = delete;
  Acceptor& operator=(const Acceptor&) = delete;

  [[nodiscard]] bool valid() const noexcept { return listenFd_.valid(); }
  [[nodiscard]] ErrorCode error() const noexcept { return error_; }

  /// 實際綁到的 port。傳 0 給建構子時由系統挑一個，
  /// 測試用這個避免固定 port 被占用而互相干擾。
  [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

  void setNewConnectionCallback(NewConnectionCallback cb) { newConnCb_ = std::move(cb); }

  /// 註冊進 event loop，開始接受連線。
  Status start();

  /// 累計接受過的連線數。atomic —— 由 loop 執行緒累加，可能被監控或
  /// 測試執行緒讀取。
  [[nodiscard]] std::uint64_t acceptedCount() const noexcept {
    return accepted_.load(std::memory_order_relaxed);
  }

 private:
  void handleRead();

  EventLoop& loop_;
  FileDescriptor listenFd_;
  std::uint16_t port_ = 0;
  ErrorCode error_ = ErrorCode::Ok;
  NewConnectionCallback newConnCb_;
  std::atomic<std::uint64_t> accepted_{0};
};

}  // namespace ledger::net
