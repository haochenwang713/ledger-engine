#pragma once

#include <ledger/concurrent/BlockingQueue.h>
#include <ledger/concurrent/Task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace ledger::concurrent {

// ---------------------------------------------------------------------------
// RequestHandler —— 處理一個請求並產生回應。在 worker 執行緒上執行。
//
// ★ 為什麼是抽象介面，而不是 std::function
//
//   Stage 6 的 handler 會持有一條 pqxx::connection，而它是 move-only。
//   std::function 要求目標可複製：
//
//     error: static assertion failed:
//            std::function target must be copy-constructible
//
//   三個選項裡這是唯一沒有代價的：
//     std::move_only_function  C++23，但 libc++ 至今未實作 → macOS 建不起來
//     shared_ptr<connection>   能動，但用共享指標表達一個絕不共享的東西
//     unique_ptr<介面>         純 C++20、可攜、語意最準  ← 選這個
//
//   unique_ptr 本身就是 move-only，worker 獨佔持有它。
//   「每個 worker 擁有自己的 handler，絕不共享」直接寫在型別上，
//   不需要靠註解提醒後人。
//
// ⚠ handle() 只會被它所屬的那一條 worker 執行緒呼叫，所以實作
//   **不需要**是執行緒安全的。這正是重點：Stage 6 的 DB 連線從建立、
//   使用到銷毀全都在同一條執行緒上，沒有共享就不需要鎖。
// ---------------------------------------------------------------------------
class RequestHandler {
 public:
  RequestHandler() = default;
  RequestHandler(const RequestHandler&) = delete;
  RequestHandler& operator=(const RequestHandler&) = delete;
  virtual ~RequestHandler() = default;

  virtual proto::ResponseEnvelope handle(const proto::RequestEnvelope& req) = 0;
};

/// 為第 workerIndex 條 worker 建立它專屬的 handler。
///
/// ★ 這個工廠在 **worker 執行緒內** 被呼叫一次。
///   Stage 6 會在這裡開一條 pqxx::connection，所以那條連線的
///   建立與使用天生就在同一條執行緒上。
///
/// 工廠本身可複製（它只負責製造，不持有連線），所以放在 std::function
/// 裡沒有問題 —— move-only 的是它「產生的東西」。
using HandlerFactory = std::function<std::unique_ptr<RequestHandler>(std::size_t workerIndex)>;

// ---------------------------------------------------------------------------
// ThreadPool —— N 條 worker 執行緒 + 一個有界佇列。
//
// ★ 為什麼要有這一層（數字）
//
//   一筆轉帳：純 CPU 約 0.05 ms，等 Postgres（含 COMMIT fsync）約 1.5 ms。
//
//     全部塞在 event loop：1 ÷ 1.55 ms ≈ 645 TPS
//     IO 執行緒 + 20 workers：20 ÷ 1.55 ms ≈ 12,900 TPS
//
//   差 20 倍。而且第一種做法還會讓「一筆慢交易停住所有連線」。
//
// ★ 為什麼是 20 而不是理論值 124
//
//   N ≈ 核心數 × (1 + 等待/計算) = 4 × (1 + 1.5/0.05) = 124。
//   但 Postgres 的 max_connections 預設 100，每條連線是一個獨立行程
//   （5–10 MB）；而且 124 條執行緒在 4 核上輪轉，context switch 與
//   快取汙染會吃掉增益。實務落點 20–32。
//
//   Stage 8 會把 worker 數當可調參數，畫 TPS-vs-worker 曲線找轉折點。
//
// ★ 兩種關機方式
//
//   shutdown()   拒收新工作 → 把佇列裡剩下的做完 → join。正常關機。
//   abort()      request_stop → 立刻放棄佇列裡的東西 → join。強制中斷。
//
//   解構子呼叫 shutdown()（優雅版本）。這是刻意的預設：佇列裡的請求
//   是「已經收下、client 正在等回應」的工作，直接丟掉會讓那些 client
//   只能等自己 timeout。排空的成本是關機多花幾十毫秒。
//
// ★ 為什麼用 std::jthread
//
//   worker 阻塞在 pop(stop_token) 上，request_stop() 會讓它自動醒來 ——
//   不需要任何人記得去 notify。而 jthread 的解構子會自動
//   request_stop() + join()，所以就算 shutdown() 因為例外而沒被呼叫到，
//   程式也不會 hang。手寫 std::thread + 旗標的版本沒有這個保險。
// ---------------------------------------------------------------------------
class ThreadPool {
 public:
  /// 建構即啟動所有 worker。
  ///
  /// name 用於除錯與統計輸出（例如 "binary" / "json"）。依 Stage 5c 的
  /// 設計，兩個 port 各有一個獨立的 ThreadPool，所以會有兩個名字。
  ThreadPool(std::string name,
             std::size_t workerCount,
             std::size_t queueCapacity,
             HandlerFactory factory);

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;

  /// 優雅關機（呼叫 shutdown()）。
  ~ThreadPool();

  /// 提交一個工作。非阻塞 —— 佇列滿時立刻回 false。
  ///
  /// ⚠ 這是 IO 執行緒唯一該呼叫的提交方法。回 false 時呼叫端的責任是
  ///   產生一個 SERVER_BUSY 回應，而不是重試或等待。
  [[nodiscard]] bool submit(Task task);

  /// 優雅關機：拒收新工作、把佇列裡剩下的處理完、join。可重複呼叫。
  void shutdown();

  /// 強制中斷：放棄佇列裡尚未處理的工作、join。可重複呼叫。
  ///
  /// 只在「已經在關機、而且不想再等」時使用。正常路徑一律用 shutdown()。
  void abort();

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] std::size_t workerCount() const noexcept { return workers_.size(); }
  [[nodiscard]] std::size_t queueSize() const { return queue_.size(); }
  [[nodiscard]] std::size_t queueCapacity() const noexcept { return queue_.capacity(); }

  /// 成功進入佇列的工作數。
  [[nodiscard]] std::uint64_t submitted() const noexcept { return queue_.totalPushed(); }
  /// 因佇列滿而被拒絕的次數 —— 背壓的直接指標。
  [[nodiscard]] std::uint64_t rejected() const noexcept { return queue_.totalRejected(); }
  /// handler 實際跑完的次數。
  [[nodiscard]] std::uint64_t completed() const noexcept {
    return completed_.load(std::memory_order_relaxed);
  }
  /// 做完才發現連線已經不在、結果被丟棄的次數（W2）。
  [[nodiscard]] std::uint64_t droppedNoSink() const noexcept {
    return droppedNoSink_.load(std::memory_order_relaxed);
  }
  /// 累計排隊等待時間（微秒）。除以 completed() 就是平均排隊延遲。
  [[nodiscard]] std::uint64_t totalQueueWaitMicros() const noexcept {
    return queueWaitMicros_.load(std::memory_order_relaxed);
  }

 private:
  void workerLoop(std::stop_token stopToken, std::size_t index);
  void joinAll();

  const std::string name_;
  HandlerFactory factory_;

  BlockingQueue<Task> queue_;

  /// ⚠ 宣告順序有意義：workers_ 必須在 queue_ 之後宣告。
  ///
  ///   成員的銷毀順序與宣告順序相反，所以 workers_ 會先被銷毀
  ///   （jthread 解構子會 request_stop + join），queue_ 才跟著走。
  ///   反過來的話，worker 會在佇列已被銷毀之後還在存取它 ——
  ///   典型的 use-after-free，而且只在關機時偶發。
  std::vector<std::jthread> workers_;

  std::atomic<std::uint64_t> completed_{0};
  std::atomic<std::uint64_t> droppedNoSink_{0};
  std::atomic<std::uint64_t> queueWaitMicros_{0};
};

}  // namespace ledger::concurrent
