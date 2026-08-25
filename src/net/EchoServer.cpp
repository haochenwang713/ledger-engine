#include <ledger/net/EchoServer.h>

#include <utility>

namespace ledger::net {

EchoServer::EchoServer(EventLoop& loop, std::uint16_t port) : loop_(loop), acceptor_(loop, port) {
  acceptor_.setNewConnectionCallback(
      [this](int fd, std::string peer) { onNewConnection(fd, std::move(peer)); });
}

Status EchoServer::start() {
  return acceptor_.start();
}

void EchoServer::onNewConnection(int fd, std::string peer) {
  auto conn = std::make_shared<Connection>(loop_, fd, std::move(peer));

  conn->setMessageCallback([this](const ConnectionPtr& c, Buffer& buf) { onMessage(c, buf); });
  conn->setCloseCallback([this](const ConnectionPtr& c) { onClose(c); });

  // 先存進表格再 start()。
  // 反過來的話，start() 之後到存進表格之前若立刻有事件進來，
  // 這個 shared_ptr 的唯一持有者還是區域變數 —— 邏輯上會很難推理。
  connections_[fd] = conn;
  activeCount_.store(connections_.size(), std::memory_order_relaxed);

  if (!conn->start()) {
    connections_.erase(fd);
    activeCount_.store(connections_.size(), std::memory_order_relaxed);
  }
}

void EchoServer::onMessage(const ConnectionPtr& conn, Buffer& buffer) {
  // Echo：收到多少就送回多少，然後把它從輸入緩衝消費掉。
  //
  // Stage 5 這裡會變成：試著從 buffer 切出一個完整 frame，
  // 切得出來就丟進 worker 佇列，切不出來就原封不動留著等下一批資料。
  if (buffer.empty()) {
    return;
  }
  conn->send(buffer.view());
  buffer.retrieveAll();
}

void EchoServer::onClose(const ConnectionPtr& conn) {
  // 從表格移除。這通常是最後一個 shared_ptr 持有者放手的時刻，
  // Connection 隨即被銷毀，解構子關閉 fd。
  connections_.erase(conn->fd());
  activeCount_.store(connections_.size(), std::memory_order_relaxed);
}

}  // namespace ledger::net
