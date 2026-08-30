#include <ledger/concurrent/ThreadPool.h>

#include <chrono>
#include <utility>

namespace ledger::concurrent {

ThreadPool::ThreadPool(std::string name,
                       std::size_t workerCount,
                       std::size_t queueCapacity,
                       HandlerFactory factory)
    : name_(std::move(name)), factory_(std::move(factory)), queue_(queueCapacity) {
  workers_.reserve(workerCount);
  for (std::size_t i = 0; i < workerCount; ++i) {
    // jthread 會把 stop_token 當第一個參數傳進來。
    workers_.emplace_back([this, i](std::stop_token st) { workerLoop(std::move(st), i); });
  }
}

ThreadPool::~ThreadPool() {
  shutdown();
}

bool ThreadPool::submit(Task task) {
  // 只用 tryPush，永不阻塞。
  //
  // 這個函式的呼叫者是 IO 執行緒。它一旦阻塞，整個 event loop 停擺，
  // 所有連線一起死 —— 不只是送出這筆請求的那一條。
  return queue_.tryPush(std::move(task));
}

void ThreadPool::shutdown() {
  // 只 close 佇列，不 request_stop。
  //
  // close() 會拒收新工作並喚醒所有睡著的 worker；醒來之後它們會發現
  // 佇列裡還有東西，於是繼續處理，直到真的排空才收到 nullopt 退出。
  // 這就是 drain 語意 —— 已收下的請求都會拿到回應。
  queue_.close();
  joinAll();
}

void ThreadPool::abort() {
  // request_stop() 讓阻塞在 pop(stop_token) 的 worker 立刻醒來並回 nullopt，
  // 佇列裡剩下什麼都不再處理。
  for (std::jthread& t : workers_) {
    t.request_stop();
  }
  // 一併 close，避免生產者還在往一個沒人消費的佇列裡塞東西。
  queue_.close();
  joinAll();
}

void ThreadPool::joinAll() {
  for (std::jthread& t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
}

void ThreadPool::workerLoop(std::stop_token stopToken, std::size_t index) {
  // ★ handler 在 worker 執行緒內建立，而不是在建構子裡。
  //
  //   Stage 6 的工廠會在這裡開一條 pqxx::connection。那條連線從建立、
  //   使用到銷毀全都在同一條執行緒上，所以完全不需要保護它 ——
  //   沒有共享就沒有競爭，這比「加一把鎖」乾淨得多。
  std::unique_ptr<RequestHandler> handler = factory_ ? factory_(index) : nullptr;

  for (;;) {
    std::optional<Task> task = queue_.pop(stopToken);
    if (!task) {
      break;  // 已關閉且排空，或被 abort() 中斷 —— 收工
    }

    // 排隊等待時間與處理時間是兩回事。過載時前者才是延遲的主因，
    // 分開量才不會把「排隊排很久」誤診成「處理很慢」。
    const auto waitMicros =
        std::chrono::duration_cast<std::chrono::microseconds>(task->queueWait()).count();
    queueWaitMicros_.fetch_add(static_cast<std::uint64_t>(waitMicros), std::memory_order_relaxed);

    // ★ W2：先確認連線還在，再花力氣處理。
    //
    //   client 可能在我們排隊的這段時間就斷線了。weak_ptr 升級失敗代表
    //   Connection 已被銷毀 —— 此時做完也沒有人收，直接丟棄。
    //   若這裡用的是裸指標，那是 use-after-free；若是 shared_ptr，
    //   則會把死掉的連線與它的 fd 硬留到現在。
    ResponseSinkPtr sink = task->sink.lock();
    if (!sink) {
      droppedNoSink_.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    if (!handler) {
      continue;
    }

    const proto::ResponseEnvelope response = handler->handle(task->request);
    completed_.fetch_add(1, std::memory_order_relaxed);

    // 交棒回去。deliver() 的實作負責把資料交給 IO 執行緒，
    // worker 自己永遠不碰 socket。
    sink->deliver(response, task->codec);
  }
}

}  // namespace ledger::concurrent
