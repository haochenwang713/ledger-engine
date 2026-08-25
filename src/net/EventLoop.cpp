#include <ledger/net/EventLoop.h>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <utility>

namespace ledger::net {
namespace {

constexpr int kMaxEventsPerWait = 256;

/// 把我們的旗標翻譯成 epoll 的位元遮罩。
/// EPOLLET 一律加上 —— 整個設計都建立在邊緣觸發之上。
std::uint32_t toEpollEvents(bool readable, bool writable) {
  std::uint32_t events = EPOLLET;
  if (readable) events |= EPOLLIN;
  if (writable) events |= EPOLLOUT;
  // EPOLLRDHUP：對端呼叫 shutdown(SHUT_WR) 時通知。
  // 沒有它的話，半關閉的連線只能靠 read() 回 0 才發現。
  events |= EPOLLRDHUP;
  return events;
}

IoEvent fromEpollEvents(std::uint32_t events) {
  IoEvent out;
  out.readable = (events & EPOLLIN) != 0;
  out.writable = (events & EPOLLOUT) != 0;
  out.error = (events & EPOLLERR) != 0;
  out.hangup = (events & (EPOLLHUP | EPOLLRDHUP)) != 0;
  return out;
}

}  // namespace

EventLoop::EventLoop() {
  // EPOLL_CLOEXEC：exec 時自動關閉，避免洩漏給子行程。
  epollFd_ = FileDescriptor(::epoll_create1(EPOLL_CLOEXEC));

  // EFD_NONBLOCK：讀取沒有計數時回 EAGAIN 而不是阻塞在 loop 執行緒上。
  wakeupFd_ = FileDescriptor(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));

  if (!valid()) {
    return;
  }

  // 把 eventfd 自己也註冊進 epoll。這樣 runInLoop() 敲它時，
  // 正在睡覺的 epoll_wait 會立刻醒過來。
  (void)addFd(wakeupFd_.get(), /*readable=*/true, /*writable=*/false, [this](const IoEvent&) {
    handleWakeup();
  });
}

EventLoop::~EventLoop() = default;

Status EventLoop::addFd(int fd, bool readable, bool writable, EventCallback cb) {
  if (fd < 0) {
    return ErrorCode::SocketError;
  }

  // 用 fd 當索引直接查表。fd 是小的、由 kernel 重複使用的整數，
  // 所以 vector 的空間不會失控，而查找是 O(1) 且沒有雜湊成本。
  if (static_cast<std::size_t>(fd) >= callbacks_.size()) {
    callbacks_.resize(static_cast<std::size_t>(fd) + 1);
  }
  callbacks_[static_cast<std::size_t>(fd)] = std::move(cb);

  epoll_event ev{};
  ev.events = toEpollEvents(readable, writable);
  ev.data.fd = fd;

  if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_ADD, fd, &ev) < 0) {
    callbacks_[static_cast<std::size_t>(fd)] = nullptr;
    return ErrorCode::EpollError;
  }
  return Status{};
}

Status EventLoop::modifyFd(int fd, bool readable, bool writable) {
  epoll_event ev{};
  ev.events = toEpollEvents(readable, writable);
  ev.data.fd = fd;

  if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_MOD, fd, &ev) < 0) {
    return ErrorCode::EpollError;
  }
  return Status{};
}

Status EventLoop::removeFd(int fd) {
  if (fd >= 0 && static_cast<std::size_t>(fd) < callbacks_.size()) {
    callbacks_[static_cast<std::size_t>(fd)] = nullptr;
  }
  if (::epoll_ctl(epollFd_.get(), EPOLL_CTL_DEL, fd, nullptr) < 0) {
    return ErrorCode::EpollError;
  }
  return Status{};
}

void EventLoop::run() {
  threadId_.store(std::this_thread::get_id(), std::memory_order_release);
  running_.store(true, std::memory_order_release);

  std::vector<epoll_event> events(kMaxEventsPerWait);

  while (running_.load(std::memory_order_acquire)) {
    // timeout 設 -1 會無限期睡眠，全靠 eventfd 叫醒。
    // 這裡刻意用 1000ms：即使喚醒機制出了問題，迴圈每秒也會檢查一次
    // running_ 旗標，關機不會卡住。這一秒的延遲只影響關機，不影響請求。
    const int n =
        ::epoll_wait(epollFd_.get(), events.data(), static_cast<int>(events.size()), 1000);

    if (n < 0) {
      // EINTR 只是被訊號打斷，不是錯誤，重試即可。
      // 沒處理它的話，程式會在收到 SIGWINCH（改變終端機大小）之類的
      // 無害訊號時莫名其妙結束。
      if (errno == EINTR) {
        continue;
      }
      break;
    }

    iterations_.fetch_add(1, std::memory_order_relaxed);

    for (int i = 0; i < n; ++i) {
      const int fd = events[static_cast<std::size_t>(i)].data.fd;
      const IoEvent io = fromEpollEvents(events[static_cast<std::size_t>(i)].events);

      if (static_cast<std::size_t>(fd) < callbacks_.size()) {
        // 複製一份回呼再呼叫。回呼有可能在執行中把自己從 loop 移除
        // （例如連線關閉），那會清掉 callbacks_[fd]，
        // 而我們正在執行的就是它 —— 直接呼叫等於在物件死掉之後還在跑。
        EventCallback cb = callbacks_[static_cast<std::size_t>(fd)];
        if (cb) {
          cb(io);
        }
      }
    }

    // 事件處理完才處理跨執行緒工作。
    // 順序不影響正確性，但這樣 IO 事件的延遲比較低。
    runPendingTasks();

    // 全部 slot 都用滿了，表示可能還有事件沒取完，下次多要一點。
    if (n == static_cast<int>(events.size())) {
      events.resize(events.size() * 2);
    }
  }

  running_.store(false, std::memory_order_release);
}

void EventLoop::stop() {
  running_.store(false, std::memory_order_release);
  // 敲一下 eventfd，讓卡在 epoll_wait 的執行緒立刻醒來，
  // 而不是等那 1000ms 逾時。
  wakeup();
}

void EventLoop::wakeup() {
  const std::uint64_t one = 1;
  const ssize_t n = ::write(wakeupFd_.get(), &one, sizeof(one));

  // write() 標了 warn_unused_result，而且這個回傳值確實值得看一眼。
  //
  // 什麼時候會失敗：eventfd 的內部計數器是 64 位元，累加到接近上限時
  // write 會回 EAGAIN。那代表 loop 執行緒已經天文數字般久沒有排空它 ——
  // 實務上等於 loop 卡死了。
  //
  // 這裡刻意「不」重試也不拋錯：epoll_wait 有 1000ms 的逾時作為後備，
  // 待辦工作最遲一秒內一定會被執行到。喚醒只是延遲最佳化，不是正確性依賴。
  // 記一筆計數讓問題可觀測就夠了。
  if (n != static_cast<ssize_t>(sizeof(one))) {
    wakeupFailures_.fetch_add(1, std::memory_order_relaxed);
  }
}

void EventLoop::runInLoop(Task task) {
  if (inLoopThread()) {
    // 已經在 loop 執行緒上了，直接做，省一次往返。
    task();
    return;
  }

  {
    std::lock_guard<std::mutex> lock(taskMutex_);
    pendingTasks_.push_back(std::move(task));
  }

  // 敲 eventfd 把 loop 叫醒。
  wakeup();
}

void EventLoop::handleWakeup() {
  // eventfd 是邊緣觸發的，必須把計數讀掉，否則它會一直處於可讀狀態……
  // 實際上 eventfd 讀取後計數歸零，讀一次就夠。但仍要迴圈到 EAGAIN，
  // 因為在我們讀的同時可能又有人寫進來。
  std::uint64_t count = 0;
  while (::read(wakeupFd_.get(), &count, sizeof(count)) > 0) {
    // 內容不重要，我們只在乎「被叫醒了」這件事。
  }
}

void EventLoop::runPendingTasks() {
  std::vector<Task> tasks;
  {
    std::lock_guard<std::mutex> lock(taskMutex_);
    tasks.swap(pendingTasks_);
  }

  // ★ 注意鎖已經放掉了才執行這些工作。
  //
  //   若在持有 taskMutex_ 的情況下執行，而某個工作又呼叫了 runInLoop()，
  //   就會對同一把非遞迴 mutex 鎖兩次 —— 死鎖。
  //   這跟 LedgerCore 裡「自己轉給自己」是同一類的錯誤。
  for (Task& task : tasks) {
    task();
  }
}

}  // namespace ledger::net
