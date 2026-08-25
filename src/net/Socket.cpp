#include <ledger/net/Socket.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

namespace ledger::net {

void FileDescriptor::reset() noexcept {
  if (fd_ >= 0) {
    // close() 也可能失敗（EINTR、EIO），但在解構子裡我們無能為力，
    // 而且 Linux 上就算回 EINTR，fd 也已經被釋放了 —— 重試會關到
    // 別的執行緒剛開的同號 fd。所以這裡刻意忽略回傳值。
    ::close(fd_);
    fd_ = -1;
  }
}

Status setNonBlocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return ErrorCode::SocketError;
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return ErrorCode::SocketError;
  }
  return Status{};
}

Status setTcpNoDelay(int fd) {
  const int one = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
    return ErrorCode::SocketError;
  }
  return Status{};
}

Status setReuseAddr(int fd) {
  const int one = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
    return ErrorCode::SocketError;
  }
  return Status{};
}

Result<int> createListenSocket(std::uint16_t port, int backlog) {
  // SOCK_NONBLOCK | SOCK_CLOEXEC 直接在建立時設定，省掉之後兩次 fcntl，
  // 也避免「建立完到設定完」那個空窗期被 fork 繼承出去。
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return ErrorCode::SocketError;
  }

  // 用 RAII 接管，這樣底下任何一步失敗都會自動 close。
  FileDescriptor guard(fd);

  if (!setReuseAddr(fd)) {
    return ErrorCode::SocketError;
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
    return ErrorCode::BindFailed;
  }
  if (::listen(fd, backlog) < 0) {
    return ErrorCode::ListenFailed;
  }

  // 一切成功，把 fd 的所有權交出去。
  return guard.release();
}

int takeSocketError(int fd) {
  int optval = 0;
  socklen_t optlen = sizeof(optval);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0) {
    return errno;
  }
  return optval;
}

std::string peerAddress(int fd) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    return "<unknown>";
  }
  char ip[INET_ADDRSTRLEN]{};
  ::inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ':' + std::to_string(ntohs(addr.sin_port));
}

}  // namespace ledger::net
