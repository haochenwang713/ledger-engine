#pragma once

#include <ledger/concurrent/ThreadPool.h>
#include <ledger/core/LedgerCore.h>
#include <ledger/net/Acceptor.h>
#include <ledger/net/Connection.h>
#include <ledger/net/EventLoop.h>
#include <ledger/proto/BinaryCodec.h>
#include <ledger/proto/FrameSplitter.h>
#include <ledger/proto/JsonCodec.h>
#include <ledger/proto/Session.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace ledger::net {

// ---------------------------------------------------------------------------
// ConnectionContext —— 一條連線的協定狀態，以及它的回程通道。
//
// ★ 生命週期與所有權（這一段是整個 Stage 5c 最容易寫錯的地方）
//
//     LedgerServer::connections_  ──shared_ptr──▶  Connection
//                                                      │
//                                        messageCb_ 捕捉 shared_ptr
//                                                      ▼
//                                              ConnectionContext
//                                                      │
//                                                 weak_ptr（★）
//                                                      ▼
//                                                 Connection
//
//   context 對 Connection 只能持 weak_ptr。持 shared_ptr 的話就形成
//   循環參照 —— 兩者互相持有，引用計數永遠不歸零，每一條斷掉的連線
//   都會永久洩漏它的 fd 與緩衝。這種洩漏不會崩潰，只會讓行程慢慢腫大，
//   壓測跑幾分鐘後才發現。
//
//   反過來，Task 對 context 持 weak_ptr（ResponseSinkWeakPtr）。
//   client 斷線 → server 從 connections_ 移除 → Connection 銷毀
//   → 它的回呼銷毀 → context 銷毀 → worker 手上的 weak_ptr 失效
//   → 結果被安靜丟棄。這就是 W2 不變式的完整鏈條。
//
// ⚠ 執行緒歸屬：
//     onMessage()  只在 IO 執行緒上執行
//     deliver()    只在 worker 執行緒上執行
//   兩者之間唯一的共享是 Connection::send()，而它本身是執行緒安全的
//   （內部小 mutex + runInLoop）。context 自己沒有任何可變狀態需要保護。
// ---------------------------------------------------------------------------
class ConnectionContext : public concurrent::ResponseSink,
                          public std::enable_shared_from_this<ConnectionContext> {
 public:
  ConnectionContext(const ConnectionPtr& conn,
                    const proto::FrameSplitter& splitter,
                    const proto::Codec& codec,
                    concurrent::ThreadPool& pool)
      : conn_(conn), session_(splitter, codec), codec_(codec), pool_(pool) {}

  /// 在 IO 執行緒上被呼叫。切包、解碼、丟進 worker 佇列。
  void onMessage(Buffer& buffer);

  /// 在 worker 執行緒上被呼叫。編碼並交給 Connection 送出。
  void deliver(const proto::ResponseEnvelope& resp, proto::CodecTag codec) override;

 private:
  /// 編碼一則回應並送出。conn 已經失效就安靜丟棄。
  void sendResponse(const proto::ResponseEnvelope& resp);

  std::weak_ptr<Connection> conn_;  ///< ★ 必須是 weak，見上方說明
  proto::Session session_;
  const proto::Codec& codec_;
  concurrent::ThreadPool& pool_;
};

// ---------------------------------------------------------------------------
// LedgerServer —— 把網路層、協定層、執行緒池、帳本核心黏起來。
//
// ★ 兩個 port，兩組完全獨立的執行緒池
//
//     :9000  長度前綴 + binary   →  ThreadPool("binary", 20 workers)
//     :9001  換行分隔 + NDJSON   →  ThreadPool("json",    4 workers)
//
//   隔離的理由：JSON port 是給人用的除錯與前端入口，binary port 是
//   壓測與正式流量。共用一個佇列的話，前端有人狂點就會排擠壓測流量，
//   讓 Stage 8 的數字變髒。
//
//   獨立的池比獨立的佇列更強：不只佇列隔離，連執行緒都隔離，
//   所以「前端一個慢查詢卡住壓測 worker」在結構上不可能發生。
//   代價是多 4 條執行緒與 4 條 DB 連線（Stage 6），可以忽略。
//
// ★ 一個 EventLoop 服務兩個 port
//
//   兩個 Acceptor 註冊在同一個 epoll 上。IO 完全不是瓶頸
//   （每筆請求在 IO 執行緒上只花約 0.05 ms 的解析時間），
//   多開一條 loop 執行緒只會增加除錯難度。
// ---------------------------------------------------------------------------
class LedgerServer {
 public:
  struct Options {
    std::uint16_t binaryPort{9000};
    std::uint16_t jsonPort{9001};
    std::size_t binaryWorkers{20};
    std::size_t jsonWorkers{4};
    std::size_t binaryQueueCapacity{8192};
    std::size_t jsonQueueCapacity{1024};
  };

  LedgerServer(EventLoop& loop, LedgerCore& core, AccountRegistry& registry, Options options);

  LedgerServer(const LedgerServer&) = delete;
  LedgerServer& operator=(const LedgerServer&) = delete;

  ~LedgerServer();

  [[nodiscard]] bool valid() const noexcept {
    return binaryAcceptor_.valid() && jsonAcceptor_.valid();
  }
  [[nodiscard]] ErrorCode error() const noexcept {
    return binaryAcceptor_.valid() ? jsonAcceptor_.error() : binaryAcceptor_.error();
  }

  Status start();

  /// 停止兩個執行緒池，等待佇列排空。
  /// 必須在 EventLoop::run() 返回之後呼叫。
  void shutdown();

  [[nodiscard]] std::uint16_t binaryPort() const noexcept { return binaryAcceptor_.port(); }
  [[nodiscard]] std::uint16_t jsonPort() const noexcept { return jsonAcceptor_.port(); }

  [[nodiscard]] std::size_t activeConnections() const noexcept {
    return activeCount_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t totalConnections() const noexcept {
    return binaryAcceptor_.acceptedCount() + jsonAcceptor_.acceptedCount();
  }

  [[nodiscard]] const concurrent::ThreadPool& binaryPool() const noexcept { return binaryPool_; }
  [[nodiscard]] const concurrent::ThreadPool& jsonPool() const noexcept { return jsonPool_; }

 private:
  enum class Protocol : std::uint8_t { Binary, Json };

  void onNewConnection(int fd, std::string peer, Protocol protocol);
  void onClose(const ConnectionPtr& conn);

  EventLoop& loop_;

  Acceptor binaryAcceptor_;
  Acceptor jsonAcceptor_;

  // 切包器與 codec 都是無狀態的純函式物件，全連線共用一份。
  proto::LengthPrefixSplitter lengthSplitter_;
  proto::NewlineSplitter newlineSplitter_;
  proto::BinaryCodec binaryCodec_;
  proto::JsonCodec jsonCodec_;

  concurrent::ThreadPool binaryPool_;
  concurrent::ThreadPool jsonPool_;

  /// 伺服器持有每條連線的 shared_ptr。從這裡移除，就是它被銷毀的時刻。
  /// 只有 loop 執行緒會碰，不需要鎖。
  std::unordered_map<int, ConnectionPtr> connections_;

  /// connections_.size() 的 atomic 鏡像，供其他執行緒安全讀取。
  /// 直接讀 map 的 size 是資料競爭，TSan 會抓（Stage 4 就踩過）。
  std::atomic<std::size_t> activeCount_{0};
};

}  // namespace ledger::net
