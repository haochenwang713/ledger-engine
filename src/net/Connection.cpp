#include <ledger/net/Connection.h>

#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace ledger::net {
namespace {

/// 每次 read() 要的量。64 KB 是個常見的折衷：
/// 大到一次系統呼叫能搬很多資料，又小到不會為每條閒置連線佔掉太多記憶體。
constexpr std::size_t kReadChunk = 64 * 1024;

}  // namespace

Connection::Connection(EventLoop& loop, int fd, std::string peer)
    : loop_(loop), fd_(fd), peer_(std::move(peer)) {}

Connection::~Connection() = default;

Status Connection::start() {
  (void)setTcpNoDelay(fd_.get());

  // 一開始只關注可讀。EPOLLOUT 要等到真的寫不完時才註冊 ——
  // 一直開著的話，只要 socket 可寫（幾乎永遠可寫）就會不停觸發事件，
  // event loop 會空轉燒 CPU。這叫 busy-loop，是新手最常見的 epoll 錯誤之一。
  auto self = shared_from_this();
  return loop_.addFd(fd_.get(), /*readable=*/true, /*writable=*/false, [self](const IoEvent& ev) {
    self->handleEvent(ev);
  });
}

void Connection::handleEvent(const IoEvent& event) {
  // 錯誤與掛斷優先處理 —— 這時再去讀寫沒有意義。
  if (event.error) {
    handleClose();
    return;
  }

  if (event.readable) {
    handleRead();
  }

  // handleRead 可能已經把連線關掉了，要再確認一次。
  if (event.writable && connected()) {
    handleWrite();
  }

  // EPOLLRDHUP：對端關閉了寫入端。
  // 注意這裡的順序 —— 必須先 handleRead() 把剩下的資料讀完，
  // 才處理關閉。反過來的話，對端「送完資料立刻關閉」時，
  // 那批資料會被丟掉。這是很容易寫反的地方。
  if (event.hangup && connected()) {
    handleClose();
  }
}

void Connection::handleRead() {
  // ★★★ 邊緣觸發的鐵律：一直讀到 EAGAIN 為止。★★★
  //
  // 少讀一次，剩下的資料會留在 kernel 緩衝區裡，而 epoll 的「狀態」
  // 沒有再改變，所以不會再通知。這條連線就此永久靜默。
  //
  // 這個 bug 只在單次傳輸量大於 kReadChunk 時才會出現，
  // 所以手動測試永遠正常，一壓測就爆。
  while (true) {
    inputBuffer_.ensureWritable(kReadChunk);

    const ssize_t n = ::read(fd_.get(), inputBuffer_.beginWrite(), kReadChunk);

    if (n > 0) {
      inputBuffer_.hasWritten(static_cast<std::size_t>(n));
      bytesRead_.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);

      // 收到多少就交給上層看一次。上層負責切出完整訊息；
      // 切不出來就原封不動留在 buffer 裡等下一批資料（半包）。
      if (messageCb_) {
        messageCb_(shared_from_this(), inputBuffer_);
      }
      continue;  // 繼續讀 —— 這個 continue 就是 ET 的鐵律
    }

    if (n == 0) {
      // 對端正常關閉了連線。
      handleClose();
      return;
    }

    // n < 0
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // 讀乾了。這才是唯一可以離開迴圈的正常出口。
      return;
    }
    if (errno == EINTR) {
      continue;  // 被訊號打斷，重試
    }

    // 真的錯誤。
    handleClose();
    return;
  }
}

void Connection::send(std::string_view data) {
  if (!connected()) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(outputMutex_);
    outputBuffer_.append(data);
  }

  // ★ 不在這裡直接 write()。
  //   實際寫入必須發生在 IO 執行緒上，否則兩個 worker 同時寫同一個 socket
  //   會讓回應的位元組交織在一起。
  //
  //   runInLoop 會判斷：如果呼叫端本來就是 loop 執行緒，直接執行（零成本）；
  //   否則進佇列並敲 eventfd 把 loop 叫醒。
  auto self = shared_from_this();
  loop_.runInLoop([self] { self->flushOutput(); });
}

void Connection::flushOutput() {
  // 這個函式只會在 loop 執行緒上執行 —— 由 runInLoop 保證。
  if (!connected()) {
    return;
  }

  while (true) {
    std::size_t pending = 0;
    const char* data = nullptr;
    {
      std::lock_guard<std::mutex> lock(outputMutex_);
      pending = outputBuffer_.readableBytes();
      data = outputBuffer_.peek();
    }

    if (pending == 0) {
      // 全部寫完了。如果之前為了等可寫而註冊了 EPOLLOUT，現在要取消 ——
      // 留著的話 socket 一可寫就觸發事件，event loop 會空轉。
      if (watchingWritable_) {
        (void)loop_.modifyFd(fd_.get(), /*readable=*/true, /*writable=*/false);
        watchingWritable_ = false;
      }
      return;
    }

    const ssize_t n = ::write(fd_.get(), data, pending);

    if (n > 0) {
      {
        std::lock_guard<std::mutex> lock(outputMutex_);
        outputBuffer_.retrieve(static_cast<std::size_t>(n));
      }
      bytesWritten_.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
      continue;  // 可能還沒寫完，再試
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // ★ kernel 的送出緩衝滿了 —— 這是正常情況，不是錯誤。
      //
      //   client 讀得比我們寫得慢時就會發生。剩下的資料留在 outputBuffer_，
      //   註冊 EPOLLOUT，等 kernel 通知「現在可以寫了」再繼續。
      //
      //   沒有處理這條路徑的程式，在慢速 client 或大量回應時會靜默地
      //   丟掉資料 —— 又是一個小測試看不出來、壓測才爆的 bug。
      if (!watchingWritable_) {
        (void)loop_.modifyFd(fd_.get(), /*readable=*/true, /*writable=*/true);
        watchingWritable_ = true;
        partialWrites_.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }

    if (n < 0 && errno == EINTR) {
      continue;
    }

    // 真的錯誤（對端已經關閉等等）。
    handleClose();
    return;
  }
}

void Connection::handleWrite() {
  // kernel 說現在可以寫了 —— 把剩下的接著寫出去。
  flushOutput();
}

void Connection::close() {
  if (loop_.inLoopThread()) {
    handleClose();
    return;
  }
  auto self = shared_from_this();
  loop_.runInLoop([self] { self->handleClose(); });
}

void Connection::handleClose() {
  State expected = State::Connected;
  // compare_exchange 保證這裡只會執行一次，就算兩個路徑
  // （讀到 0、收到 EPOLLERR）同時想關閉也一樣。
  if (!state_.compare_exchange_strong(expected, State::Disconnected, std::memory_order_acq_rel)) {
    return;
  }

  (void)loop_.removeFd(fd_.get());

  if (closeCb_) {
    // 傳 shared_ptr 進去，確保回呼執行期間物件不會被銷毀 ——
    // 回呼裡通常會把這條連線從伺服器的表格裡移除，那正是最後一個
    // 持有者放手的時刻。
    closeCb_(shared_from_this());
  }
}

}  // namespace ledger::net
