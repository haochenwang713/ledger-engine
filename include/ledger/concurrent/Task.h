#pragma once

#include <ledger/proto/Codec.h>
#include <ledger/proto/Messages.h>

#include <chrono>
#include <memory>
#include <utility>

namespace ledger::concurrent {

// ---------------------------------------------------------------------------
// ResponseSink —— worker 把結果交回去的地方。
//
// ★ 為什麼是一個抽象介面，而不是直接持有 net::Connection
//
//   Connection 屬於 net/，而 net/ 是 Linux 專屬（epoll）。Task 若直接
//   依賴它，整個 concurrent/ 也會變成 Linux-only —— 而執行緒池恰好是
//   最需要在各種環境反覆跑 TSan 的地方。
//
//   插一層純虛介面之後：
//     - concurrent/ 保持平台無關，macOS 上也能完整測試
//     - 測試可以用假的 sink，不需要真的 socket
//     - weak_ptr 的語意完全保留（見下方 W2）
//
// ⚠ deliver() 會被 worker 執行緒呼叫，所以實作必須是執行緒安全的。
//
//   Stage 5c 的實作只做兩件事：把位元組塞進 Connection 的 outputBuffer_
//   （小 mutex 保護），然後 runInLoop 請 IO 執行緒去 write()。
//   ★ worker 絕對不能自己 write(fd) —— 兩個 worker 同時寫同一個 socket，
//     位元組會交織、長度前綴對不上、協定崩潰。而這不是 data race
//     （write() 本身執行緒安全），所以 TSan 永遠抓不到。
//     只能靠架構規則排除。
// ---------------------------------------------------------------------------
class ResponseSink {
 public:
  ResponseSink() = default;
  ResponseSink(const ResponseSink&) = delete;
  ResponseSink& operator=(const ResponseSink&) = delete;
  virtual ~ResponseSink() = default;

  virtual void deliver(const proto::ResponseEnvelope& resp, proto::CodecTag codec) = 0;
};

using ResponseSinkPtr = std::shared_ptr<ResponseSink>;
using ResponseSinkWeakPtr = std::weak_ptr<ResponseSink>;

// ---------------------------------------------------------------------------
// Task —— 從 IO 執行緒交給 worker 的一個工作單元。
//
// ★ W2 不變式：worker 完成時連線可能已經不在。
//
//   client 可以在 worker 還在等 DB 的那 1.5 毫秒裡斷線。若 Task 持有
//   shared_ptr，連線物件會被硬留到 worker 做完 —— 對已死的連線做事、
//   而且延後釋放 fd。若持有裸指標，那就是 use-after-free。
//
//   weak_ptr 是唯一正確的選擇：worker 完成後 lock() 升級，
//   失效就安靜丟棄結果。
// ---------------------------------------------------------------------------
struct Task {
  using Clock = std::chrono::steady_clock;

  ResponseSinkWeakPtr sink;
  proto::RequestEnvelope request;
  proto::CodecTag codec{proto::CodecTag::Binary};

  /// 進佇列的時刻。
  ///
  /// worker 取出時的 now() 減掉它，就是「排隊等了多久」——
  /// 這個數字跟「處理花了多久」是兩回事，而且在過載時它才是主角。
  /// Stage 8 要分開量這兩段，否則會把排隊延遲誤認為處理慢。
  Clock::time_point enqueuedAt{Clock::now()};

  Task() = default;

  Task(ResponseSinkWeakPtr s, proto::RequestEnvelope req, proto::CodecTag tag)
      : sink(std::move(s)), request(std::move(req)), codec(tag) {}

  [[nodiscard]] Clock::duration queueWait() const { return Clock::now() - enqueuedAt; }
};

}  // namespace ledger::concurrent
