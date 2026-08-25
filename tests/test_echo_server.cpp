// ---------------------------------------------------------------------------
// echo server 的整合測試 —— 用真的 socket，不是 mock。
//
// 為什麼堅持用真的 socket：
//   Stage 4 要驗的東西（ET 有沒有讀乾、部分寫入有沒有處理、EAGAIN 的
//   時機對不對）全都是 kernel 的行為。用 mock 把 kernel 換掉，
//   就等於把要測的東西測掉了。
//
// 只在 Linux 上編譯 —— macOS 沒有 epoll。
// ---------------------------------------------------------------------------

#include <ledger/net/EchoServer.h>
#include <ledger/net/EventLoop.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

namespace ledger::net {
namespace {

/// 測試用的同步（阻塞）client。刻意跟伺服器用不同的實作方式 ——
/// 兩邊都用同一套程式碼的話，共同的錯誤會互相抵銷而測不出來。
class TestClient {
 public:
  explicit TestClient(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      ::close(fd_);
      fd_ = -1;
      return;
    }
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    // 收不到資料時不要無限期卡住 —— 讓測試失敗，而不是掛住。
    timeval tv{};
    tv.tv_sec = 10;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  }

  ~TestClient() { closeNow(); }

  TestClient(const TestClient&) = delete;
  TestClient& operator=(const TestClient&) = delete;

  [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }

  bool sendAll(const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
      const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) return false;
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  /// 收滿 expected 個位元組，或逾時。
  std::string recvExactly(std::size_t expected) {
    std::string out;
    out.reserve(expected);
    char chunk[16384];
    while (out.size() < expected) {
      const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (n <= 0) break;
      out.append(chunk, static_cast<std::size_t>(n));
    }
    return out;
  }

  void closeNow() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

/// 在背景執行緒跑 event loop，測試結束時乾淨關閉。
class ServerFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(loop_.valid()) << "epoll 或 eventfd 建立失敗";

    // port 傳 0 —— 讓系統挑一個沒被占用的。
    // 寫死 port 的測試會在 CI 上互相打架，或撞到開發機上跑著的服務。
    server_ = std::make_unique<EchoServer>(loop_, 0);
    ASSERT_TRUE(server_->valid()) << "監聽失敗: " << toString(server_->error());
    ASSERT_TRUE(server_->start().ok());

    port_ = server_->port();
    ASSERT_GT(port_, 0);

    loopThread_ = std::thread([this] { loop_.run(); });

    // 等 loop 真的跑起來再讓測試開始連線。
    for (int i = 0; i < 200 && loop_.loopIterations() == 0; ++i) {
      loop_.runInLoop([] {});
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void TearDown() override {
    loop_.stop();
    if (loopThread_.joinable()) {
      loopThread_.join();
    }
  }

  EventLoop loop_;
  std::unique_ptr<EchoServer> server_;
  std::uint16_t port_ = 0;
  std::thread loopThread_;
};

// === 基本連通 ==============================================================

TEST_F(ServerFixture, EchoesASmallMessage) {
  TestClient client(port_);
  ASSERT_TRUE(client.connected());

  const std::string message = "hello ledger";
  ASSERT_TRUE(client.sendAll(message));

  EXPECT_EQ(client.recvExactly(message.size()), message);
}

TEST_F(ServerFixture, HandlesManySequentialMessages) {
  TestClient client(port_);
  ASSERT_TRUE(client.connected());

  for (int i = 0; i < 500; ++i) {
    const std::string message = "msg-" + std::to_string(i);
    ASSERT_TRUE(client.sendAll(message));
    ASSERT_EQ(client.recvExactly(message.size()), message) << "第 " << i << " 筆";
  }
}

// ===========================================================================
// ★ 這是 Stage 4 最重要的一條測試：邊緣觸發有沒有讀乾。
//
// 送一筆遠大於單次 read 量（64 KB）的資料。
//
// 如果 Connection::handleRead 少了那個 while 迴圈、只讀一次就 return，
// 剩下的位元組會留在 kernel 緩衝區裡，而 epoll 的狀態沒有再改變，
// 所以永遠不會再通知 —— 這條連線就此永久靜默，測試會逾時失敗。
//
// 這個 bug 用小訊息測永遠測不出來。這就是為什麼要刻意送 1 MB。
// ===========================================================================
TEST_F(ServerFixture, EdgeTriggeredReadDrainsTheEntireSocket) {
  TestClient client(port_);
  ASSERT_TRUE(client.connected());

  // 1 MB，遠超過 kReadChunk（64 KB），保證需要迴圈讀很多次
  const std::string big(1024 * 1024, 'A');
  ASSERT_TRUE(client.sendAll(big));

  const std::string echoed = client.recvExactly(big.size());
  ASSERT_EQ(echoed.size(), big.size()) << "只收到 " << echoed.size() << " / " << big.size()
                                       << " 位元組 —— ET 模式沒有讀到 EAGAIN，連線靜默了";
  EXPECT_EQ(echoed, big);
}

// ===========================================================================
// ★ 部分寫入：kernel 的送出緩衝滿了，而且不會有新的讀取事件來救。
//
// 這個測試的關鍵在那個 sleep。
//
// 第一版沒有 sleep，結果「拿掉 EPOLLOUT 註冊」的破壞版本竟然也通過 ——
// 因為 client 還在持續送資料，每個讀取事件都會再呼叫一次 flushOutput()，
// 等於順手把上次沒寫完的補寫掉了。測試被讀取事件「順便」救了回來，
// 完全沒有測到要測的東西。
//
// 加上 sleep 之後：client 送完就停手且不讀，伺服器的送出緩衝塞爆，
// write() 回 EAGAIN。此刻已經沒有新的讀取事件了 —— 唯一能讓剩下的
// 資料寫出去的機制，就是 EPOLLOUT。沒註冊它，資料就永遠留在
// outputBuffer_ 裡，client 等到逾時。
//
// 教訓：測試「通過」不等於它有在測東西。要驗證它抓不抓得到才算數。
// ===========================================================================
TEST_F(ServerFixture, HandlesPartialWritesWhenClientStopsReading) {
  TestClient client(port_);
  ASSERT_TRUE(client.connected());

  const std::string big(4 * 1024 * 1024, 'B');
  ASSERT_TRUE(client.sendAll(big));

  // ★ 送完就停手。此刻伺服器手上還有一大堆寫不出去的回應，
  //   而且不會再有任何讀取事件進來驅動它。
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  // 現在才開始讀。若 EPOLLOUT 有正確註冊，伺服器會被 kernel 叫醒續寫，
  // 一個位元組都不會少。
  const std::string echoed = client.recvExactly(big.size());
  ASSERT_EQ(echoed.size(), big.size()) << "只收到 " << echoed.size() << " / " << big.size()
                                       << " 位元組 —— 部分寫入沒有處理，剩下的回應石沉大海";
  EXPECT_EQ(echoed, big);
}

// === 併發連線 ==============================================================

TEST_F(ServerFixture, ServesManyConcurrentClients) {
  constexpr int kClients = 50;
  constexpr int kMessagesEach = 20;

  std::atomic<int> failures{0};
  std::vector<std::thread> threads;

  for (int c = 0; c < kClients; ++c) {
    threads.emplace_back([&, c] {
      TestClient client(port_);
      if (!client.connected()) {
        failures.fetch_add(1);
        return;
      }
      for (int i = 0; i < kMessagesEach; ++i) {
        const std::string message = "c" + std::to_string(c) + "-m" + std::to_string(i);
        if (!client.sendAll(message) || client.recvExactly(message.size()) != message) {
          failures.fetch_add(1);
          return;
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(failures.load(), 0);
  EXPECT_GE(server_->totalConnections(), static_cast<std::uint64_t>(kClients));
}

// === 生命週期 ==============================================================

TEST_F(ServerFixture, CleansUpWhenClientDisconnects) {
  {
    TestClient client(port_);
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.sendAll("bye"));
    ASSERT_EQ(client.recvExactly(3), "bye");
  }  // client 解構，socket 關閉

  // 等伺服器察覺到並清掉。輪詢而不是 sleep 固定時間 ——
  // 前者在快的機器上瞬間結束，慢的機器上也不會誤判。
  for (int i = 0; i < 500 && server_->activeConnections() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(server_->activeConnections(), 0u) << "連線關閉後沒有被清掉 —— fd 洩漏";
}

// ★ 對端「送完資料立刻關閉」—— 那批資料不能被丟掉。
//
// 這測的是 handleEvent 裡的順序：必須先 handleRead() 把資料讀完，
// 才處理 hangup。寫反的話最後一批資料會消失。
TEST_F(ServerFixture, ReadsDataSentImmediatelyBeforeClose) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    TestClient client(port_);
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.sendAll("last words"));
    EXPECT_EQ(client.recvExactly(10), "last words") << "第 " << attempt << " 次嘗試";
  }
}

TEST_F(ServerFixture, SurvivesAbruptDisconnects) {
  // client 連上就馬上斷，反覆很多次。伺服器不能崩、不能洩漏 fd。
  for (int i = 0; i < 200; ++i) {
    TestClient client(port_);
    ASSERT_TRUE(client.connected()) << "第 " << i << " 次連線失敗 —— 可能是 fd 洩漏";
    client.closeNow();
  }

  // 還活著嗎？
  TestClient client(port_);
  ASSERT_TRUE(client.connected());
  ASSERT_TRUE(client.sendAll("still alive"));
  EXPECT_EQ(client.recvExactly(11), "still alive");
}

// === EventLoop ============================================================

TEST_F(ServerFixture, RunInLoopExecutesTasksOnLoopThread) {
  std::atomic<bool> ran{false};
  std::atomic<bool> onLoopThread{false};

  loop_.runInLoop([&] {
    onLoopThread.store(loop_.inLoopThread());
    ran.store(true);
  });

  for (int i = 0; i < 500 && !ran.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  EXPECT_TRUE(ran.load()) << "eventfd 喚醒機制沒有生效";
  EXPECT_TRUE(onLoopThread.load()) << "工作沒有在 loop 執行緒上執行";
}

TEST_F(ServerFixture, RunInLoopHandlesManyCrossThreadTasks) {
  constexpr int kTasks = 2000;
  std::atomic<int> counter{0};

  std::vector<std::thread> producers;
  for (int t = 0; t < 8; ++t) {
    producers.emplace_back([&] {
      for (int i = 0; i < kTasks / 8; ++i) {
        loop_.runInLoop([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
      }
    });
  }
  for (auto& t : producers) {
    t.join();
  }

  for (int i = 0; i < 1000 && counter.load() < kTasks; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_EQ(counter.load(), kTasks) << "有工作遺失 —— eventfd 喚醒或佇列有問題";
}

}  // namespace
}  // namespace ledger::net
