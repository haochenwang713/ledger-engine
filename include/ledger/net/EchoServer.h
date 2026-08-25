#pragma once

#include <ledger/common/Result.h>
#include <ledger/net/Acceptor.h>
#include <ledger/net/Connection.h>
#include <ledger/net/EventLoop.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ledger::net {

// ---------------------------------------------------------------------------
// EchoServer —— Stage 4 的驗收目標：收到什麼就原樣送回去。
//
// 刻意做成「沒有協定」的版本。Stage 4 要證明的是網路層本身正確：
//   ET 讀乾了、部分寫入處理了、連線生命週期管好了。
// 把協定解析也塞進來的話，出問題時分不清是網路層還是解析器的錯。
//
// Stage 5 會把 messageCallback 從「原樣回送」換成
// 「切出 frame → 丟進 BlockingQueue → worker 處理」。
// 那時候這個類別會變成 LedgerServer，但網路層一行都不用改。
// ---------------------------------------------------------------------------
class EchoServer {
 public:
  EchoServer(EventLoop& loop, std::uint16_t port);

  EchoServer(const EchoServer&) = delete;
  EchoServer& operator=(const EchoServer&) = delete;

  [[nodiscard]] bool valid() const noexcept { return acceptor_.valid(); }
  [[nodiscard]] ErrorCode error() const noexcept { return acceptor_.error(); }
  [[nodiscard]] std::uint16_t port() const noexcept { return acceptor_.port(); }

  Status start();

  /// 目前存活的連線數。
  ///
  /// 不能直接回傳 connections_.size() —— 那個 map 由 loop 執行緒
  /// 邊讀邊改，從外部執行緒讀它的 size 是資料競爭（TSan 會抓）。
  /// 另外維護一個 atomic 計數器是最省事的解法。
  [[nodiscard]] std::size_t activeConnections() const noexcept {
    return activeCount_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t totalConnections() const noexcept {
    return acceptor_.acceptedCount();
  }

 private:
  void onNewConnection(int fd, std::string peer);
  void onMessage(const ConnectionPtr& conn, Buffer& buffer);
  void onClose(const ConnectionPtr& conn);

  EventLoop& loop_;
  Acceptor acceptor_;

  /// 伺服器持有每條連線的 shared_ptr。這是連線活著的原因 ——
  /// 從這裡移除，就是它被銷毀的時刻。
  /// 只有 loop 執行緒會碰，不需要鎖。
  std::unordered_map<int, ConnectionPtr> connections_;

  /// connections_.size() 的 atomic 鏡像，供其他執行緒安全讀取。
  std::atomic<std::size_t> activeCount_{0};
};

}  // namespace ledger::net
