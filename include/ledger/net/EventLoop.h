#pragma once

#include <ledger/common/Result.h>
#include <ledger/net/Socket.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ledger::net {

/// epoll 回報的事件種類，包成我們自己的型別，
/// 這樣上層不必直接處理 EPOLLIN 那些位元旗標。
struct IoEvent {
  bool readable = false;
  bool writable = false;
  bool error = false;   ///< EPOLLERR：真正的 errno 要用 takeSocketError() 撈
  bool hangup = false;  ///< EPOLLHUP / EPOLLRDHUP：對端關閉了
};

/// 事件回呼。回呼在 event loop 執行緒上執行，所以絕對不能阻塞。
using EventCallback = std::function<void(const IoEvent&)>;

/// 跨執行緒丟進來、要在 loop 執行緒上執行的工作。
using Task = std::function<void()>;

// ---------------------------------------------------------------------------
// EventLoop —— epoll 主迴圈。
//
// 全系統只有這個類別碰 epoll，也只有它被允許對 socket 呼叫 write()。
//
// ===========================================================================
// 邊緣觸發（edge-triggered, ET）
// ===========================================================================
//
// epoll 有兩種模式：
//
//   水平觸發 LT：只要「還有資料可讀」，每次 epoll_wait 都會回報。
//                沒讀完？下次還會通知你。寬容，但事件會重複觸發。
//
//   邊緣觸發 ET：只在「狀態改變的那一瞬間」通知一次。
//                從沒資料變成有資料 → 通知。你沒讀完 → 不會再通知。
//
// 我們用 ET，因為它 syscall 更少、驚群效應更輕。代價是一條鐵律：
//
//   ★ 收到可讀事件，就必須一直 read() 到回傳 EAGAIN 為止。
//
//   少讀一次會怎樣？剩下的資料躺在 kernel 緩衝區裡，而狀態沒有再「改變」，
//   所以 epoll 永遠不會再通知你。那條連線就此永久靜默 ——
//   client 在等回應，server 以為沒有請求。
//
//   這個 bug 只在「一次送的資料超過單次 read 的量」時才出現，
//   所以小量測試永遠正常，壓測一開才爆。這是 epoll 最經典的坑。
//
// ===========================================================================
// 跨執行緒喚醒（eventfd）
// ===========================================================================
//
// worker 執行緒做完交易之後，需要讓 IO 執行緒把回應寫出去。
// 但 IO 執行緒此刻正卡在 epoll_wait 裡睡覺 —— 怎麼叫醒它？
//
// 答案是 eventfd：一個只有計數器的、可以放進 epoll 監看的 fd。
// worker 對它寫一個 8 byte 的數字，它就變成可讀，epoll_wait 立刻返回。
//
// 這就是 runInLoop() 的機制：把工作放進佇列，然後敲 eventfd。
// 沒有它的話，worker 只能直接寫 socket，而那會違反「一個 fd 只有一個
// 寫入者」的不變式 —— 兩個回應的位元組會交織在一起，協定直接崩潰。
// （而且那不是 data race，TSan 抓不到。）
// ---------------------------------------------------------------------------
class EventLoop {
 public:
  EventLoop();
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  /// 建構是否成功。失敗通常是 epoll_create1 或 eventfd 出問題。
  [[nodiscard]] bool valid() const noexcept { return epollFd_.valid() && wakeupFd_.valid(); }

  /// 開始監看一個 fd。events 只在註冊時決定 EPOLLIN/EPOLLOUT 的初值。
  Status addFd(int fd, bool readable, bool writable, EventCallback cb);

  /// 改變某個 fd 關注的事件。回應寫不完要註冊 EPOLLOUT 時用。
  Status modifyFd(int fd, bool readable, bool writable);

  /// 停止監看。呼叫端負責 close 那個 fd。
  Status removeFd(int fd);

  /// 主迴圈。會一直跑到 stop() 被呼叫。
  void run();

  /// 要求停止。可以從任何執行緒呼叫。
  void stop();

  /// 把工作丟到 loop 執行緒上執行。
  ///
  /// 若呼叫端本身就是 loop 執行緒，直接執行；否則進佇列並敲 eventfd。
  /// 這是 worker 執行緒把回應交回來的唯一途徑。
  void runInLoop(Task task);

  /// 呼叫端是不是 loop 執行緒？
  ///
  /// threadId_ 是 atomic 的，因為它由 loop 執行緒在 run() 裡寫入，
  /// 卻會被任何呼叫 runInLoop() 的執行緒讀取 —— 沒有同步就是資料競爭。
  /// 這一條是 ThreadSanitizer 抓出來的，靠讀程式碼很難發現：
  /// 寫入只發生一次，看起來人畜無害。
  [[nodiscard]] bool inLoopThread() const noexcept {
    return threadId_.load(std::memory_order_acquire) == std::this_thread::get_id();
  }

  [[nodiscard]] std::uint64_t loopIterations() const noexcept {
    return iterations_.load(std::memory_order_relaxed);
  }

  /// eventfd 寫入失敗的次數。正常情況恆為 0；
  /// 不為 0 代表 loop 執行緒長時間沒有排空喚醒計數。
  [[nodiscard]] std::uint64_t wakeupFailures() const noexcept {
    return wakeupFailures_.load(std::memory_order_relaxed);
  }

 private:
  /// 敲 eventfd，把睡在 epoll_wait 裡的 loop 執行緒叫醒。
  void wakeup();
  void handleWakeup();
  void runPendingTasks();

  FileDescriptor epollFd_;
  FileDescriptor wakeupFd_;  ///< eventfd，用來把 epoll_wait 從睡眠中叫醒

  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> iterations_{0};
  std::atomic<std::uint64_t> wakeupFailures_{0};
  std::atomic<std::thread::id> threadId_{};

  /// fd → 回呼。只有 loop 執行緒會碰它，所以不需要鎖。
  std::vector<EventCallback> callbacks_;

  /// 待執行的跨執行緒工作。這個「會」被多執行緒碰，所以要鎖。
  /// 臨界區極短（只是搬移一個 vector），用普通 mutex 就好 ——
  /// shared_mutex 在這裡沒有讀者，只會更慢。
  std::mutex taskMutex_;
  std::vector<Task> pendingTasks_;
};

}  // namespace ledger::net
