// ---------------------------------------------------------------------------
// LedgerServer 的端對端測試 —— 真的 socket、真的執行緒池、真的帳本。
//
// 到這裡為止每一層都各自測過了：協定層有 golden 表格，執行緒池有 TSan，
// 帳本核心有守恆測試。這個檔案測的是「接線」—— 三層黏起來之後，
// 一個位元組從 socket 進去、繞過 worker、再從同一個 socket 出來，
// 中間有沒有掉東西。
//
// 只在 Linux 上編譯 —— macOS 沒有 epoll。
// ---------------------------------------------------------------------------

#include <ledger/core/AccountRegistry.h>
#include <ledger/core/Journal.h>
#include <ledger/core/LedgerCore.h>
#include <ledger/core/LedgerRequestHandler.h>
#include <ledger/net/EventLoop.h>
#include <ledger/net/LedgerServer.h>
#include <ledger/proto/BinaryCodec.h>
#include <ledger/proto/FrameSplitter.h>
#include <ledger/proto/JsonCodec.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace ledger::net {
namespace {

using namespace std::chrono_literals;

/// 同步阻塞 client。刻意跟伺服器用不同的實作方式 ——
/// 兩邊共用同一套程式碼的話，共同的錯誤會互相抵銷而測不出來。
class TestClient {
 public:
  explicit TestClient(std::uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return;
    }

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
      if (n <= 0) {
        return false;
      }
      sent += static_cast<std::size_t>(n);
    }
    return true;
  }

  /// 讀到換行為止（NDJSON 用）。回傳不含換行的那一行。
  std::string recvLine() {
    for (;;) {
      const std::size_t nl = inbox_.find('\n');
      if (nl != std::string::npos) {
        std::string line = inbox_.substr(0, nl);
        inbox_.erase(0, nl + 1);
        return line;
      }
      if (!fill()) {
        return {};
      }
    }
  }

  /// 讀一個完整的長度前綴 frame（binary 用）。回傳不含長度前綴的部分。
  std::string recvFrame() {
    for (;;) {
      if (inbox_.size() >= 4) {
        const auto* b = reinterpret_cast<const unsigned char*>(inbox_.data());
        const std::size_t len =
            (static_cast<std::size_t>(b[0]) << 24) | (static_cast<std::size_t>(b[1]) << 16) |
            (static_cast<std::size_t>(b[2]) << 8) | static_cast<std::size_t>(b[3]);
        if (inbox_.size() >= 4 + len) {
          std::string frame = inbox_.substr(4, len);
          inbox_.erase(0, 4 + len);
          return frame;
        }
      }
      if (!fill()) {
        return {};
      }
    }
  }

  /// 對端是否已經關閉連線。
  bool waitForEof() {
    for (;;) {
      char chunk[4096];
      const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (n == 0) {
        return true;  // 乾淨的 FIN
      }
      if (n < 0) {
        return false;  // 逾時或錯誤
      }
      inbox_.append(chunk, static_cast<std::size_t>(n));
    }
  }

  void closeNow() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  bool fill() {
    char chunk[16384];
    const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return false;
    }
    inbox_.append(chunk, static_cast<std::size_t>(n));
    return true;
  }

  int fd_ = -1;
  std::string inbox_;
};

/// 啟動一台完整的伺服器：帳本 + 兩個 port + 兩組執行緒池。
class LedgerServerFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(loop_.valid()) << "epoll 或 eventfd 建立失敗";

    core_ = std::make_unique<LedgerCore>(registry_, journal_);
    ASSERT_TRUE(seedDemoAccounts(*core_, registry_).ok());

    LedgerServer::Options options;
    // port 傳 0 —— 讓系統挑。寫死 port 的測試會在 CI 上互相打架。
    options.binaryPort = 0;
    options.jsonPort = 0;
    // 測試用小一點的池，啟動快、關閉也快。
    options.binaryWorkers = 4;
    options.jsonWorkers = 2;

    server_ = std::make_unique<LedgerServer>(loop_, *core_, registry_, options);
    ASSERT_TRUE(server_->valid()) << "監聽失敗: " << toString(server_->error());
    ASSERT_TRUE(server_->start().ok());

    binaryPort_ = server_->binaryPort();
    jsonPort_ = server_->jsonPort();
    ASSERT_GT(binaryPort_, 0);
    ASSERT_GT(jsonPort_, 0);
    ASSERT_NE(binaryPort_, jsonPort_);

    loopThread_ = std::thread([this] { loop_.run(); });

    // 等 loop 真的跑起來再讓測試開始連線。
    for (int i = 0; i < 200 && loop_.loopIterations() == 0; ++i) {
      loop_.runInLoop([] {});
      std::this_thread::sleep_for(1ms);
    }
  }

  void TearDown() override {
    // ⚠ 順序：先停 loop，再關執行緒池。
    //   反過來的話 pool 已關但 loop 還在收請求，新進來的全部拿到
    //   SERVER_BUSY —— 而那不是我們想測的行為。
    loop_.stop();
    if (loopThread_.joinable()) {
      loopThread_.join();
    }
    server_->shutdown();

    // 每個測試結束都檢查一次帳本是平的。跑完一整輪流量之後
    // 不變式仍然成立，才算真的通過。
    EXPECT_TRUE(core_->verifyInvariants()) << "測試結束時帳本不平";
  }

  [[nodiscard]] Amount balanceOf(AccountId id) const {
    Account* account = registry_.find(id);
    return account == nullptr ? -1 : account->balance();
  }

  EventLoop loop_;
  AccountRegistry registry_;
  Journal journal_;
  std::unique_ptr<LedgerCore> core_;
  std::unique_ptr<LedgerServer> server_;
  std::uint16_t binaryPort_ = 0;
  std::uint16_t jsonPort_ = 0;
  std::thread loopThread_;

  const proto::BinaryCodec binaryCodec_;
  const proto::JsonCodec jsonCodec_;
};

// === 種子資料 ==============================================================

TEST_F(LedgerServerFixture, SeedMatchesTheDesignDocument) {
  // 設計文件第 4 節與 db/seeds/dev_seed.sql 的數字。
  // Stage 6 接上 DB 之後，同一組請求應該得到一模一樣的回應。
  EXPECT_EQ(balanceOf(1001), 115000);  // Alice USD $1,150.00
  EXPECT_EQ(balanceOf(2002), 47000);   // Bob   USD   $470.00
  EXPECT_EQ(balanceOf(1003), 5000);    // Alice JPY ¥5,000（exponent = 0）

  // 錢不會憑空出現：系統帳戶的負餘額絕對值 = 流通中的錢。
  EXPECT_EQ(balanceOf(9001), -162000);
  EXPECT_EQ(core_->totalBalance(Currency::USD), 0);
  EXPECT_EQ(core_->totalBalance(Currency::JPY), 0);
}

// === JSON port（給人用的那個）===============================================

TEST_F(LedgerServerFixture, JsonPingRoundTrip) {
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(client.sendAll("{\"id\":\"1\",\"type\":\"ping\"}\n"));

  const std::string line = client.recvLine();
  const auto resp = jsonCodec_.decodeResponse(line);
  ASSERT_TRUE(resp.ok()) << "收到的是: " << line;
  EXPECT_EQ(resp.value().reqId, 1U);
  EXPECT_TRUE(std::holds_alternative<proto::PongResp>(resp.value().body));
}

TEST_F(LedgerServerFixture, JsonGetAccountReturnsSeededBalance) {
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(client.sendAll("{\"id\":\"2\",\"type\":\"get_account\",\"account_id\":\"1001\"}\n"));

  const auto resp = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(resp.ok());
  const auto* account = std::get_if<proto::AccountResp>(&resp.value().body);
  ASSERT_TRUE(account != nullptr);
  EXPECT_EQ(account->id, 1001);
  EXPECT_EQ(account->balance, 115000);
  EXPECT_EQ(account->ccy, Currency::USD);
  EXPECT_EQ(account->status, proto::AccountStatus::Active);
}

TEST_F(LedgerServerFixture, JsonTransferMovesMoney) {
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(
      client.sendAll(R"({"id":"3","type":"transfer","idem_key":"t1","from":"1001","to":"2002",)"
                     R"("amount":"2500","ccy":"USD"})"
                     "\n"));

  const auto resp = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(resp.ok());
  const auto* ok = std::get_if<proto::TransferOkResp>(&resp.value().body);
  ASSERT_TRUE(ok != nullptr);
  EXPECT_EQ(ok->fromBalance, 112500);
  EXPECT_EQ(ok->toBalance, 49500);

  // 回應說的餘額必須跟帳本真正的餘額一致。
  EXPECT_EQ(balanceOf(1001), 112500);
  EXPECT_EQ(balanceOf(2002), 49500);
}

TEST_F(LedgerServerFixture, JsonIntegersAreStringsOnTheWire) {
  // 回應裡的 int64 必須是字串。裸數字會在瀏覽器裡靜默失去精度。
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(client.sendAll("{\"id\":\"4\",\"type\":\"get_account\",\"account_id\":\"1001\"}\n"));

  const std::string line = client.recvLine();
  EXPECT_NE(line.find(R"("balance":"115000")"), std::string::npos)
      << "balance 應該是帶引號的字串，實際收到: " << line;
  EXPECT_EQ(line.find(R"("balance":115000)"), std::string::npos) << "不該出現裸數字";
}

TEST_F(LedgerServerFixture, JsonBlankLinesDoNotDisconnect) {
  // 有人在 nc 裡多按了幾次 Enter。這是除錯 port，不該因此斷線。
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(client.sendAll("\n\n\n{\"id\":\"5\",\"type\":\"ping\"}\n"));

  const auto resp = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().reqId, 5U);
}

// === 錯誤路徑 ==============================================================

TEST_F(LedgerServerFixture, RejectsOverdraftWithoutMovingMoney) {
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(
      client.sendAll(R"({"id":"6","type":"transfer","idem_key":"t2","from":"2002","to":"1001",)"
                     R"("amount":"999999","ccy":"USD"})"
                     "\n"));

  const auto resp = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(resp.ok());
  const auto* err = std::get_if<proto::ErrorResp>(&resp.value().body);
  ASSERT_TRUE(err != nullptr);
  EXPECT_EQ(err->code, ErrorCode::InsufficientFunds);

  // 被拒絕的轉帳不能留下任何痕跡。
  EXPECT_EQ(balanceOf(1001), 115000);
  EXPECT_EQ(balanceOf(2002), 47000);
}

TEST_F(LedgerServerFixture, RejectsCrossCurrencyTransfer) {
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // 1001 是 USD，1003 是 JPY。
  ASSERT_TRUE(
      client.sendAll(R"({"id":"7","type":"transfer","idem_key":"t3","from":"1001","to":"1003",)"
                     R"("amount":"100","ccy":"USD"})"
                     "\n"));

  const auto resp = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(resp.ok());
  const auto* err = std::get_if<proto::ErrorResp>(&resp.value().body);
  ASSERT_TRUE(err != nullptr);
  EXPECT_EQ(err->code, ErrorCode::CurrencyMismatch);
}

TEST_F(LedgerServerFixture, RejectsUnknownAccount) {
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(
      client.sendAll("{\"id\":\"8\",\"type\":\"get_account\",\"account_id\":\"424242\"}\n"));

  const auto resp = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(resp.ok());
  const auto* err = std::get_if<proto::ErrorResp>(&resp.value().body);
  ASSERT_TRUE(err != nullptr);
  EXPECT_EQ(err->code, ErrorCode::AccountNotFound);
}

// ★ 解碼失敗必須是可恢復的：回一個錯誤，連線繼續活著。
TEST_F(LedgerServerFixture, GarbageLineGetsAnErrorButKeepsTheConnection) {
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(client.sendAll("this is not json\n"));
  const auto err = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(err.ok());
  EXPECT_TRUE(std::holds_alternative<proto::ErrorResp>(err.value().body));

  // 同一條連線上再送一則合法訊息，必須正常運作。
  ASSERT_TRUE(client.sendAll("{\"id\":\"9\",\"type\":\"ping\"}\n"));
  const auto pong = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(pong.ok()) << "解碼失敗之後連線不該被關掉";
  EXPECT_EQ(pong.value().reqId, 9U);
}

// ★ 框架失敗必須是致命的：位元組流已經無法對齊，只能關閉。
TEST_F(LedgerServerFixture, FramingErrorClosesTheConnection) {
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());

  // 宣稱後面有 4,294,967,295 個位元組。
  ASSERT_TRUE(client.sendAll(std::string("\xFF\xFF\xFF\xFF", 4)));

  // 應該先收到一則說明用的錯誤，然後連線被關閉。
  const std::string frame = client.recvFrame();
  ASSERT_FALSE(frame.empty());
  const auto resp = binaryCodec_.decodeResponse(frame);
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(std::get<proto::ErrorResp>(resp.value().body).code, ErrorCode::FrameTooLarge);

  EXPECT_TRUE(client.waitForEof()) << "框架失敗之後連線必須被關閉";
}

// === Binary port ==========================================================

TEST_F(LedgerServerFixture, BinaryTransferRoundTrip) {
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());

  const proto::RequestEnvelope req{42,
                                   proto::TransferReq{"bin-1", 1001, 2002, 1000, Currency::USD}};
  ASSERT_TRUE(client.sendAll(binaryCodec_.encodeRequest(req)));

  const auto resp = binaryCodec_.decodeResponse(client.recvFrame());
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().reqId, 42U) << "reqId 必須原樣抄回";
  const auto* ok = std::get_if<proto::TransferOkResp>(&resp.value().body);
  ASSERT_TRUE(ok != nullptr);
  EXPECT_EQ(ok->fromBalance, 114000);
}

TEST_F(LedgerServerFixture, BinaryHandlesGluedRequests) {
  // 三則訊息一次送出。TCP 很可能把它們合併成一次 read。
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());

  std::string wire;
  for (std::uint32_t i = 1; i <= 3; ++i) {
    wire += binaryCodec_.encodeRequest({i, proto::PingReq{}});
  }
  ASSERT_TRUE(client.sendAll(wire));

  for (std::uint32_t i = 1; i <= 3; ++i) {
    const auto resp = binaryCodec_.decodeResponse(client.recvFrame());
    ASSERT_TRUE(resp.ok());
    EXPECT_EQ(resp.value().reqId, i);
  }
}

TEST_F(LedgerServerFixture, BinaryHandlesSplitRequests) {
  // 反過來：一則訊息被拆成兩次送，中間隔一段時間。
  // 半包處理錯的話，伺服器會永遠等下去或切出垃圾。
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());

  const std::string wire = binaryCodec_.encodeRequest({77, proto::GetAccountReq{2002}});
  ASSERT_TRUE(client.sendAll(wire.substr(0, 6)));
  std::this_thread::sleep_for(50ms);
  ASSERT_TRUE(client.sendAll(wire.substr(6)));

  const auto resp = binaryCodec_.decodeResponse(client.recvFrame());
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().reqId, 77U);
  EXPECT_EQ(std::get<proto::AccountResp>(resp.value().body).balance, 47000);
}

TEST_F(LedgerServerFixture, TwoPortsShareOneLedger) {
  // 兩個 port 有各自的執行緒池，但看到的是同一本帳。
  TestClient jsonClient(jsonPort_);
  TestClient binaryClient(binaryPort_);
  ASSERT_TRUE(jsonClient.connected());
  ASSERT_TRUE(binaryClient.connected());

  ASSERT_TRUE(jsonClient.sendAll(
      R"({"id":"1","type":"transfer","idem_key":"cross-1","from":"1001","to":"2002",)"
      R"("amount":"3000","ccy":"USD"})"
      "\n"));
  ASSERT_TRUE(jsonCodec_.decodeResponse(jsonClient.recvLine()).ok());

  ASSERT_TRUE(binaryClient.sendAll(binaryCodec_.encodeRequest({1, proto::GetAccountReq{1001}})));
  const auto resp = binaryCodec_.decodeResponse(binaryClient.recvFrame());
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(std::get<proto::AccountResp>(resp.value().body).balance, 112000)
      << "binary port 應該看得到 json port 造成的變動";
}

// === 併發 ==================================================================

// ★ 這是 Stage 5c 最重要的一條測試。
//
//   八個 client 同時對同一對帳戶來回轉帳。它同時驗證：
//     · 多條連線的 IO 沒有互相干擾
//     · 20 條 worker 平行呼叫 LedgerCore 沒有 lost update
//     · 排序取鎖在 A→B 與 B→A 同時發生時不死鎖
//     · 每個回應都回到正確的那條連線上
TEST_F(LedgerServerFixture, ConcurrentClientsPreserveTotalMoney) {
  constexpr int kClients = 8;
  constexpr int kPerClient = 50;

  const Amount before = balanceOf(1001) + balanceOf(2002);
  std::atomic<int> succeeded{0};
  std::atomic<int> answered{0};

  std::vector<std::thread> threads;
  threads.reserve(kClients);

  for (int c = 0; c < kClients; ++c) {
    threads.emplace_back([&, c] {
      TestClient client(jsonPort_);
      if (!client.connected()) {
        return;
      }
      // 偶數 client 走 1001→2002，奇數走 2002→1001 —— 方向相反，
      // 這正是天真的「先鎖來源」實作會死鎖的情況。
      const char* from = (c % 2 == 0) ? "1001" : "2002";
      const char* to = (c % 2 == 0) ? "2002" : "1001";

      for (int i = 0; i < kPerClient; ++i) {
        const std::string body = std::string(R"({"id":")") + std::to_string(i) +
                                 R"(","type":"transfer","idem_key":"c)" + std::to_string(c) + "-" +
                                 std::to_string(i) + R"(","from":")" + from + R"(","to":")" + to +
                                 R"(","amount":"100","ccy":"USD"})" + "\n";
        if (!client.sendAll(body)) {
          return;
        }
        const std::string line = client.recvLine();
        if (line.empty()) {
          return;
        }
        answered.fetch_add(1, std::memory_order_relaxed);

        const auto resp = jsonCodec_.decodeResponse(line);
        if (resp.ok() && std::holds_alternative<proto::TransferOkResp>(resp.value().body)) {
          succeeded.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }

  for (auto& t : threads) {
    t.join();
  }

  EXPECT_EQ(answered.load(), kClients * kPerClient) << "每一筆請求都必須有回應";
  EXPECT_GT(succeeded.load(), 0);

  // ★ 守恆定律：不管成功幾筆、順序如何，兩個帳戶的總額必須分毫不差。
  //   少一分錢就是 lost update —— 而 lost update 不會報錯，只會讓錢消失。
  EXPECT_EQ(balanceOf(1001) + balanceOf(2002), before);
  EXPECT_EQ(core_->totalBalance(Currency::USD), 0);
}

TEST_F(LedgerServerFixture, SurvivesClientsDisconnectingMidFlight) {
  // client 送出請求後立刻斷線。worker 完成時 weak_ptr 已經失效，
  // 結果應該被安靜丟棄，而不是崩潰（W2 不變式）。
  for (int i = 0; i < 50; ++i) {
    TestClient client(jsonPort_);
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.sendAll("{\"id\":\"1\",\"type\":\"ping\"}\n"));
    client.closeNow();  // 不等回應
  }

  // 伺服器必須還活著。
  TestClient survivor(jsonPort_);
  ASSERT_TRUE(survivor.connected());
  ASSERT_TRUE(survivor.sendAll("{\"id\":\"99\",\"type\":\"ping\"}\n"));
  const auto resp = jsonCodec_.decodeResponse(survivor.recvLine());
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().reqId, 99U);
}

TEST_F(LedgerServerFixture, ManyConnectionsAtOnce) {
  constexpr int kClients = 30;
  std::vector<std::unique_ptr<TestClient>> clients;
  clients.reserve(kClients);

  for (int i = 0; i < kClients; ++i) {
    clients.push_back(std::make_unique<TestClient>(jsonPort_));
    ASSERT_TRUE(clients.back()->connected()) << "第 " << i << " 條連線失敗";
  }

  for (int i = 0; i < kClients; ++i) {
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)]->sendAll("{\"id\":\"" + std::to_string(i) +
                                                              "\",\"type\":\"ping\"}\n"));
  }

  for (int i = 0; i < kClients; ++i) {
    const auto resp = jsonCodec_.decodeResponse(clients[static_cast<std::size_t>(i)]->recvLine());
    ASSERT_TRUE(resp.ok()) << "第 " << i << " 條連線沒收到回應";
    EXPECT_EQ(resp.value().reqId, static_cast<std::uint32_t>(i)) << "回應跑到錯誤的連線上了";
  }
}

}  // namespace
}  // namespace ledger::net
