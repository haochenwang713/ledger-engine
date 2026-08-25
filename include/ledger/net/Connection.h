#pragma once

#include <ledger/common/Result.h>
#include <ledger/net/Buffer.h>
#include <ledger/net/EventLoop.h>
#include <ledger/net/Socket.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace ledger::net {

class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;

/// 收到資料時的回呼。在 loop 執行緒上執行。
/// 回呼負責從 buffer 裡切出完整訊息並消費掉；切不出來就原封不動留著。
using MessageCallback = std::function<void(const ConnectionPtr&, Buffer&)>;

/// 連線關閉時的回呼。
using CloseCallback = std::function<void(const ConnectionPtr&)>;

// ---------------------------------------------------------------------------
// Connection —— 一條 TCP 連線的完整狀態。
//
// ===========================================================================
// 為什麼是 shared_ptr
// ===========================================================================
// Stage 5 之後，worker 執行緒處理完交易要把回應送回來，但那可能是
// 1.5 毫秒之後的事 —— client 早就斷線、Connection 早就該被銷毀了。
//
// 所以 Task 裡存的是 weak_ptr：worker 完成後嘗試 lock() 升級成 shared_ptr，
// 失敗就代表連線沒了，把結果丟掉。存裸指標的話那裡就是 use-after-free。
//
// ===========================================================================
// 為什麼 send() 可以跨執行緒呼叫，卻不會違反「單一寫入者」
// ===========================================================================
// send() 並不直接 write()。它把資料塞進 outputBuffer_（用小 mutex 保護），
// 然後透過 loop_->runInLoop() 請 IO 執行緒去寫。
//
// 實際的 write() 永遠只發生在 IO 執行緒上。這條規則不能破 ——
// 兩個執行緒同時 write() 同一個 socket，兩個回應的位元組會交織在一起，
// client 收到的長度前綴對不上，協定直接崩潰。
// 而且那不是 data race（write 本身是執行緒安全的系統呼叫），TSan 抓不到。
// ---------------------------------------------------------------------------
class Connection : public std::enable_shared_from_this<Connection> {
 public:
  Connection(EventLoop& loop, int fd, std::string peer);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  [[nodiscard]] int fd() const noexcept { return fd_.get(); }
  [[nodiscard]] const std::string& peer() const noexcept { return peer_; }
  [[nodiscard]] bool connected() const noexcept {
    return state_.load(std::memory_order_acquire) == State::Connected;
  }

  void setMessageCallback(MessageCallback cb) { messageCb_ = std::move(cb); }
  void setCloseCallback(CloseCallback cb) { closeCb_ = std::move(cb); }

  /// 把連線註冊進 event loop，開始收資料。
  Status start();

  /// 送資料。可以從任何執行緒呼叫。
  void send(std::string_view data);

  /// 關閉連線。可以從任何執行緒呼叫。
  void close();

  /// 統計，給測試觀察。
  [[nodiscard]] std::uint64_t bytesRead() const noexcept {
    return bytesRead_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t bytesWritten() const noexcept {
    return bytesWritten_.load(std::memory_order_relaxed);
  }
  /// 因為 kernel 送出緩衝滿而必須註冊 EPOLLOUT 的次數。
  /// 這個數字大於 0，就證明部分寫入的處理路徑真的被走到了。
  [[nodiscard]] std::uint64_t partialWrites() const noexcept {
    return partialWrites_.load(std::memory_order_relaxed);
  }

 private:
  enum class State { Connected, Disconnecting, Disconnected };

  void handleEvent(const IoEvent& event);
  void handleRead();
  void handleWrite();
  void handleClose();

  /// 在 loop 執行緒上把 outputBuffer_ 的內容寫出去。
  void flushOutput();

  EventLoop& loop_;
  FileDescriptor fd_;
  std::string peer_;

  std::atomic<State> state_{State::Connected};

  /// 只有 loop 執行緒會碰，不需要鎖。
  Buffer inputBuffer_;

  /// 會被跨執行緒碰（send 從 worker 呼叫、flushOutput 在 loop 執行緒）。
  /// 臨界區只有 append 或 retrieve，極短，用普通 mutex。
  std::mutex outputMutex_;
  Buffer outputBuffer_;

  /// EPOLLOUT 目前有沒有註冊。避免每次寫入都白呼叫一次 epoll_ctl。
  bool watchingWritable_ = false;

  MessageCallback messageCb_;
  CloseCallback closeCb_;

  std::atomic<std::uint64_t> bytesRead_{0};
  std::atomic<std::uint64_t> bytesWritten_{0};
  std::atomic<std::uint64_t> partialWrites_{0};
};

}  // namespace ledger::net
