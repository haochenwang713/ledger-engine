#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace ledger::concurrent {

// ---------------------------------------------------------------------------
// BlockingQueue<T> —— 有界的多生產者多消費者佇列。
//
// 這是 IO 執行緒與 worker 執行緒之間唯一的交界。
//
// ★ 為什麼有界（預設 8192）
//
//   無界佇列在 DB 變慢時有兩個後果：記憶體無上限成長，以及排隊時間爆炸
//   —— client 早就 timeout 了，worker 還在處理它三十秒前送的請求。
//   兩者都比「當場拒絕」更糟。8192 筆 ≈ 0.6 秒的積壓，超過就沒有意義。
//
//   有界佇列提供的是「背壓」：系統在過載時明確地說不，而不是假裝還撐得住。
//
// ★ 為什麼不用 lock-free MPMC
//
//   1. 優化錯地方 —— 一次 push/pop 約 100 ns，DB 等待約 1,500,000 ns，
//      差四個數量級。
//   2. 新 bug 來源 —— 要自己處理 ABA 與記憶體回收（hazard pointer / epoch），
//      而 TSan 對自訂原子演算法的判讀並不可靠。
//   3. 競爭不嚴重 —— 臨界區只推一個 Task，持鎖約 50 ns。
//
//   真正該換的時機是 Stage 8 用 perf 量到 futex 等待佔 CPU > 5%，
//   而正確的下一步是「每 worker 一個佇列 + 輪詢派發」，不是 lock-free。
//
// ★ 兩種結束方式，語意完全不同
//
//   close()      拒收新工作，但已在佇列裡的東西仍會被發完（drain）。
//                這是正常關機 —— 那些請求的 client 還在等回應。
//   stop_token   立刻放棄，佇列裡剩什麼都不管。這是強制中斷。
//
//   分成兩個機制而不是一個旗標，是因為它們回答不同的問題：
//   「還收不收新的」與「現在就走還是做完再走」。混在一起的話關機時
//   就只能二選一，而正常關機需要的恰好是「不收新的 ＋ 做完再走」。
//
// ★ 為什麼是 condition_variable_any 而不是 condition_variable
//
//   只有前者有 wait(lock, stop_token, pred) 這個多載。代價是每次阻塞
//   等待會多註冊一個 stop_callback —— 而這個成本只發生在「佇列空了、
//   worker 真的睡著」的時候，系統忙碌時 worker 根本不會睡。
//   拿閒置時的一點開銷，換掉「忘記 notify 就永遠 join 不回來」
//   這一整類 bug，很划算。
// ---------------------------------------------------------------------------
template <typename T>
class BlockingQueue {
 public:
  static constexpr std::size_t kDefaultCapacity = 8192;

  explicit BlockingQueue(std::size_t capacity = kDefaultCapacity)
      : capacity_(capacity == 0 ? 1 : capacity) {}

  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;

  ~BlockingQueue() { close(); }

  // -------------------------------------------------------------------------
  // 生產端
  // -------------------------------------------------------------------------

  /// 非阻塞推入。滿了或已關閉就立刻回 false。
  ///
  /// ⚠ IO 執行緒只能用這個，永遠不能用 push()。
  ///
  ///   event loop 執行緒一旦阻塞，所有連線一起停擺 —— 不只是送出這筆
  ///   請求的那一條。佇列滿時正確的行為是產生一個 SERVER_BUSY 回應。
  ///   這是整個伺服器最重要的一條紀律：違反它不會有編譯錯誤，
  ///   只會在壓測時看到「TPS 突然掉到零」。
  [[nodiscard]] bool tryPush(T value) {
    {
      std::unique_lock lock(mutex_);
      if (closed_ || queue_.size() >= capacity_) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      queue_.push_back(std::move(value));
      pushed_.fetch_add(1, std::memory_order_relaxed);
    }
    // 先解鎖再通知：被喚醒的執行緒不必立刻撞上一把還握著的鎖。
    notEmpty_.notify_one();
    return true;
  }

  /// 阻塞推入，等到有空位為止。已關閉則回 false。
  ///
  /// 只給測試與離線批次匯入使用。伺服器的熱路徑不該出現它。
  [[nodiscard]] bool push(T value) {
    {
      std::unique_lock lock(mutex_);
      notFull_.wait(lock, [this] { return closed_ || queue_.size() < capacity_; });
      if (closed_) {
        return false;
      }
      queue_.push_back(std::move(value));
      pushed_.fetch_add(1, std::memory_order_relaxed);
    }
    notEmpty_.notify_one();
    return true;
  }

  // -------------------------------------------------------------------------
  // 消費端
  // -------------------------------------------------------------------------

  /// 阻塞取出。回 nullopt 表示「已關閉且已排空」，worker 該收工了。
  [[nodiscard]] std::optional<T> pop() {
    std::unique_lock lock(mutex_);

    // 述詞包含 closed_ 是關機能否完成的關鍵。只等 !queue_.empty() 的話，
    // 關機時沒有新資料進來，這些執行緒會永遠睡著，join() 永遠回不來。
    notEmpty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    return popLocked(lock);
  }

  /// 可被 stop_token 中斷的阻塞取出。
  ///
  /// 回 nullopt 的三種情況：
  ///   1. 進來時已被請求停止（強制中斷，佇列裡剩什麼都不管）
  ///   2. 已關閉且已排空（正常收工）
  ///   3. 等待期間被請求停止
  [[nodiscard]] std::optional<T> pop(std::stop_token stopToken) {
    std::unique_lock lock(mutex_);

    // 先檢查一次：已經被要求停止就別再拿新工作。
    if (stopToken.stop_requested()) {
      return std::nullopt;
    }

    // 這個多載會在 request_stop() 時自動醒來並回傳 false ——
    // 不需要任何人記得去 notify。這正是換成 stop_token 的理由。
    if (!notEmpty_.wait(lock, stopToken, [this] { return closed_ || !queue_.empty(); })) {
      return std::nullopt;
    }
    return popLocked(lock);
  }

  /// 非阻塞取出。空的就回 nullopt。
  [[nodiscard]] std::optional<T> tryPop() {
    std::unique_lock lock(mutex_);
    return popLocked(lock);
  }

  // -------------------------------------------------------------------------
  // 生命週期
  // -------------------------------------------------------------------------

  /// 關閉佇列：拒絕新的推入，並喚醒所有等待中的執行緒。
  /// 已在佇列裡的元素仍然拿得出來（drain）。
  ///
  /// 必須 notify_all 而不是 notify_one —— 每一條被阻塞的執行緒都要醒來
  /// 看到 closed_。漏掉任何一條，join() 就會永遠卡住，而這種 bug
  /// 只在關機路徑出現，正是最少被測到的地方。
  void close() {
    {
      std::lock_guard lock(mutex_);
      if (closed_) {
        return;
      }
      closed_ = true;
    }
    notEmpty_.notify_all();
    notFull_.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
  }

  // -------------------------------------------------------------------------
  // 觀測
  // -------------------------------------------------------------------------

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  [[nodiscard]] bool empty() const { return size() == 0; }

  /// 計數器用 relaxed：只要最終數字正確，不需要與其他資料建立順序關係。
  /// 這正是 atomic 適合的場合 —— 不需要跟別人一起變的東西。
  [[nodiscard]] std::uint64_t totalPushed() const noexcept {
    return pushed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t totalPopped() const noexcept {
    return popped_.load(std::memory_order_relaxed);
  }
  /// 因佇列滿或已關閉而被拒絕的次數 —— 背壓的直接指標，Stage 8 會畫成曲線。
  [[nodiscard]] std::uint64_t totalRejected() const noexcept {
    return rejected_.load(std::memory_order_relaxed);
  }

 private:
  /// 三個 pop 多載共同的尾巴。呼叫時必須已持有 lock。
  [[nodiscard]] std::optional<T> popLocked(std::unique_lock<std::mutex>& lock) {
    if (queue_.empty()) {
      return std::nullopt;
    }
    T value = std::move(queue_.front());
    queue_.pop_front();
    popped_.fetch_add(1, std::memory_order_relaxed);

    lock.unlock();
    notFull_.notify_one();
    return value;
  }

  mutable std::mutex mutex_;
  std::condition_variable_any notEmpty_;
  std::condition_variable_any notFull_;

  std::deque<T> queue_;
  const std::size_t capacity_;
  bool closed_{false};

  std::atomic<std::uint64_t> pushed_{0};
  std::atomic<std::uint64_t> popped_{0};
  std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace ledger::concurrent
