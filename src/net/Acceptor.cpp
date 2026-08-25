#include <ledger/net/Acceptor.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace ledger::net {

Acceptor::Acceptor(EventLoop& loop, std::uint16_t port) : loop_(loop) {
  auto created = createListenSocket(port);
  if (!created.ok()) {
    error_ = created.error();
    return;
  }
  listenFd_ = FileDescriptor(created.value());

  // 傳 0 進來時由系統挑 port，這裡把實際挑到的問出來。
  // 測試靠這個功能避免固定 port 衝突。
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(listenFd_.get(), reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
    port_ = ntohs(addr.sin_port);
  }
}

Status Acceptor::start() {
  if (!valid()) {
    return error_ != ErrorCode::Ok ? error_ : ErrorCode::SocketError;
  }
  return loop_.addFd(
      listenFd_.get(), /*readable=*/true, /*writable=*/false, [this](const IoEvent&) {
        handleRead();
      });
}

void Acceptor::handleRead() {
  // ET 鐵律：一直接到 EAGAIN。
  while (true) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);

    // accept4 而不是 accept：可以在同一個系統呼叫裡設定 NONBLOCK 與 CLOEXEC。
    // 用 accept() 的話要再補兩次 fcntl，而且在那個空窗期裡
    // fd 是阻塞的、也會被 fork 繼承出去。
    const int connFd = ::accept4(
        listenFd_.get(), reinterpret_cast<sockaddr*>(&addr), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);

    if (connFd >= 0) {
      accepted_.fetch_add(1, std::memory_order_relaxed);
      std::string peer = peerAddress(connFd);
      if (newConnCb_) {
        newConnCb_(connFd, std::move(peer));
      } else {
        ::close(connFd);
      }
      continue;  // 繼續接 —— ET 鐵律
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;  // 佇列空了，正常出口
    }
    if (errno == EINTR || errno == ECONNABORTED) {
      // ECONNABORTED：client 在三次握手完成後、我們 accept 之前就跑掉了。
      // 這是完全正常的事，不該讓伺服器停止接受其他連線。
      continue;
    }
    if (errno == EMFILE || errno == ENFILE) {
      // fd 用完了。這裡不能直接 return —— 因為 ET 模式下，
      // 沒有把佇列清空就不會再收到通知，伺服器會永久停止接受新連線。
      //
      // 正規解法是「預留一個 fd」：事先開著一個空 fd，遇到 EMFILE 時
      // 關掉它、accept 之後立刻 close、再重新開回來，把那條連線正常拒絕掉。
      // Stage 5 會補上這個處理。現在先記錄並離開，避免無窮迴圈。
      return;
    }
    return;
  }
}

}  // namespace ledger::net
