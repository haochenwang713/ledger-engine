// ---------------------------------------------------------------------------
// LedgerServer 的端對端測試 —— 真的 socket、真的執行緒池、真的帳本。
//
// 到這裡為止每一層都各自測過了：協定層有 golden 表格，執行緒池有 TSan，
// 帳本核心有守恆測試。這個檔案測的是「接線」—— 三層黏起來之後，
// 一個位元組從 socket 進去、繞過 worker、再從同一個 socket 出來，
// 中間有沒有掉東西。
//
// 只在 Linux 上編譯 —— macOS 沒有 epoll。
//
// Test structure: Arrange-Act-Assert. The shared Arrange — a full server with
// two ports, two pools and a seeded ledger — lives in LedgerServerFixture, and
// TearDown() re-checks the invariants after every test. See instruction.md.
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
  // Arrange（共用）—— 每條測試都從「一台載入示範帳戶、在跑的伺服器」開始。
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
  // Assert only —— fixture 已經完成 Arrange，這裡只核對數字。
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
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act
  ASSERT_TRUE(client.sendAll("{\"id\":\"1\",\"type\":\"ping\"}\n"));
  const std::string line = client.recvLine();

  // Assert
  const auto resp = jsonCodec_.decodeResponse(line);
  ASSERT_TRUE(resp.ok()) << "收到的是: " << line;
  EXPECT_EQ(resp.value().reqId, 1U);
  EXPECT_TRUE(std::holds_alternative<proto::PongResp>(resp.value().body));
}

TEST_F(LedgerServerFixture, JsonGetAccountReturnsSeededBalance) {
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act
  ASSERT_TRUE(client.sendAll("{\"id\":\"2\",\"type\":\"get_account\",\"account_id\":\"1001\"}\n"));
  const auto resp = jsonCodec_.decodeResponse(client.recvLine());

  // Assert
  ASSERT_TRUE(resp.ok());
  const auto* account = std::get_if<proto::AccountResp>(&resp.value().body);
  ASSERT_TRUE(account != nullptr);
  EXPECT_EQ(account->id, 1001);
  EXPECT_EQ(account->balance, 115000);
  EXPECT_EQ(account->ccy, Currency::USD);
  EXPECT_EQ(account->status, proto::AccountStatus::Active);
}

TEST_F(LedgerServerFixture, JsonTransferMovesMoney) {
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act
  ASSERT_TRUE(
      client.sendAll(R"({"id":"3","type":"transfer","idem_key":"t1","from":"1001","to":"2002",)"
                     R"("amount":"2500","ccy":"USD"})"
                     "\n"));
  const auto resp = jsonCodec_.decodeResponse(client.recvLine());

  // Assert
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
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act
  ASSERT_TRUE(client.sendAll("{\"id\":\"4\",\"type\":\"get_account\",\"account_id\":\"1001\"}\n"));
  const std::string line = client.recvLine();

  // Assert —— 回應裡的 int64 必須是字串。
  // 裸數字會在瀏覽器裡靜默失去精度。
  EXPECT_NE(line.find(R"("balance":"115000")"), std::string::npos)
      << "balance 應該是帶引號的字串，實際收到: " << line;
  EXPECT_EQ(line.find(R"("balance":115000)"), std::string::npos) << "不該出現裸數字";
}

TEST_F(LedgerServerFixture, JsonBlankLinesDoNotDisconnect) {
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act —— 有人在 nc 裡多按了幾次 Enter。
  ASSERT_TRUE(client.sendAll("\n\n\n{\"id\":\"5\",\"type\":\"ping\"}\n"));
  const auto resp = jsonCodec_.decodeResponse(client.recvLine());

  // Assert —— 這是除錯 port，不該因此斷線。
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().reqId, 5U);
}

// === 錯誤路徑 ==============================================================

TEST_F(LedgerServerFixture, RejectsOverdraftWithoutMovingMoney) {
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act
  ASSERT_TRUE(
      client.sendAll(R"({"id":"6","type":"transfer","idem_key":"t2","from":"2002","to":"1001",)"
                     R"("amount":"999999","ccy":"USD"})"
                     "\n"));
  const auto resp = jsonCodec_.decodeResponse(client.recvLine());

  // Assert
  ASSERT_TRUE(resp.ok());
  const auto* err = std::get_if<proto::ErrorResp>(&resp.value().body);
  ASSERT_TRUE(err != nullptr);
  EXPECT_EQ(err->code, ErrorCode::InsufficientFunds);

  // 被拒絕的轉帳不能留下任何痕跡。
  EXPECT_EQ(balanceOf(1001), 115000);
  EXPECT_EQ(balanceOf(2002), 47000);
}

TEST_F(LedgerServerFixture, RejectsCrossCurrencyTransfer) {
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act —— 1001 是 USD，1003 是 JPY。
  ASSERT_TRUE(
      client.sendAll(R"({"id":"7","type":"transfer","idem_key":"t3","from":"1001","to":"1003",)"
                     R"("amount":"100","ccy":"USD"})"
                     "\n"));
  const auto resp = jsonCodec_.decodeResponse(client.recvLine());

  // Assert
  ASSERT_TRUE(resp.ok());
  const auto* err = std::get_if<proto::ErrorResp>(&resp.value().body);
  ASSERT_TRUE(err != nullptr);
  EXPECT_EQ(err->code, ErrorCode::CurrencyMismatch);
}

TEST_F(LedgerServerFixture, RejectsUnknownAccount) {
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act
  ASSERT_TRUE(
      client.sendAll("{\"id\":\"8\",\"type\":\"get_account\",\"account_id\":\"424242\"}\n"));
  const auto resp = jsonCodec_.decodeResponse(client.recvLine());

  // Assert
  ASSERT_TRUE(resp.ok());
  const auto* err = std::get_if<proto::ErrorResp>(&resp.value().body);
  ASSERT_TRUE(err != nullptr);
  EXPECT_EQ(err->code, ErrorCode::AccountNotFound);
}

// ★ 解碼失敗必須是可恢復的：回一個錯誤，連線繼續活著。
TEST_F(LedgerServerFixture, GarbageLineGetsAnErrorButKeepsTheConnection) {
  // Arrange
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  // Act & Assert（第一段：垃圾必須換來一則錯誤）
  ASSERT_TRUE(client.sendAll("this is not json\n"));
  const auto err = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(err.ok());
  EXPECT_TRUE(std::holds_alternative<proto::ErrorResp>(err.value().body));

  // Act & Assert（第二段：同一條連線上再送一則合法訊息，必須正常運作）
  ASSERT_TRUE(client.sendAll("{\"id\":\"9\",\"type\":\"ping\"}\n"));
  const auto pong = jsonCodec_.decodeResponse(client.recvLine());
  ASSERT_TRUE(pong.ok()) << "解碼失敗之後連線不該被關掉";
  EXPECT_EQ(pong.value().reqId, 9U);
}

// ★ 框架失敗必須是致命的：位元組流已經無法對齊，只能關閉。
TEST_F(LedgerServerFixture, FramingErrorClosesTheConnection) {
  // Arrange
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());

  // Act —— 宣稱後面有 4,294,967,295 個位元組。
  ASSERT_TRUE(client.sendAll(std::string("\xFF\xFF\xFF\xFF", 4)));
  const std::string frame = client.recvFrame();

  // Assert —— 應該先收到一則說明用的錯誤，然後連線被關閉。
  ASSERT_FALSE(frame.empty());
  const auto resp = binaryCodec_.decodeResponse(frame);
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(std::get<proto::ErrorResp>(resp.value().body).code, ErrorCode::FrameTooLarge);

  EXPECT_TRUE(client.waitForEof()) << "框架失敗之後連線必須被關閉";
}

// === Binary port ==========================================================

TEST_F(LedgerServerFixture, BinaryTransferRoundTrip) {
  // Arrange
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());
  const proto::RequestEnvelope req{42,
                                   proto::TransferReq{"bin-1", 1001, 2002, 1000, Currency::USD}};

  // Act
  ASSERT_TRUE(client.sendAll(binaryCodec_.encodeRequest(req)));
  const auto resp = binaryCodec_.decodeResponse(client.recvFrame());

  // Assert
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().reqId, 42U) << "reqId 必須原樣抄回";
  const auto* ok = std::get_if<proto::TransferOkResp>(&resp.value().body);
  ASSERT_TRUE(ok != nullptr);
  EXPECT_EQ(ok->fromBalance, 114000);
}

TEST_F(LedgerServerFixture, BinaryHandlesGluedRequests) {
  // Arrange —— 三則訊息一次送出。TCP 很可能把它們合併成一次 read。
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());
  std::string wire;
  for (std::uint32_t i = 1; i <= 3; ++i) {
    wire += binaryCodec_.encodeRequest({i, proto::PingReq{}});
  }

  // Act
  ASSERT_TRUE(client.sendAll(wire));

  // Assert —— 三則各自回一則，順序不變。
  for (std::uint32_t i = 1; i <= 3; ++i) {
    const auto resp = binaryCodec_.decodeResponse(client.recvFrame());
    ASSERT_TRUE(resp.ok());
    EXPECT_EQ(resp.value().reqId, i);
  }
}

TEST_F(LedgerServerFixture, BinaryHandlesSplitRequests) {
  // Arrange
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());
  const std::string wire = binaryCodec_.encodeRequest({77, proto::GetAccountReq{2002}});

  // Act —— 一則訊息被拆成兩次送，中間隔一段時間。
  // 半包處理錯的話，伺服器會永遠等下去或切出垃圾。
  ASSERT_TRUE(client.sendAll(wire.substr(0, 6)));
  std::this_thread::sleep_for(50ms);
  ASSERT_TRUE(client.sendAll(wire.substr(6)));
  const auto resp = binaryCodec_.decodeResponse(client.recvFrame());

  // Assert
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().reqId, 77U);
  EXPECT_EQ(std::get<proto::AccountResp>(resp.value().body).balance, 47000);
}

TEST_F(LedgerServerFixture, TwoPortsShareOneLedger) {
  // Arrange —— 兩個 port 有各自的執行緒池，但看到的是同一本帳。
  TestClient jsonClient(jsonPort_);
  TestClient binaryClient(binaryPort_);
  ASSERT_TRUE(jsonClient.connected());
  ASSERT_TRUE(binaryClient.connected());

  // Act —— 從 json port 轉帳，再從 binary port 查餘額。
  ASSERT_TRUE(jsonClient.sendAll(
      R"({"id":"1","type":"transfer","idem_key":"cross-1","from":"1001","to":"2002",)"
      R"("amount":"3000","ccy":"USD"})"
      "\n"));
  ASSERT_TRUE(jsonCodec_.decodeResponse(jsonClient.recvLine()).ok());

  ASSERT_TRUE(binaryClient.sendAll(binaryCodec_.encodeRequest({1, proto::GetAccountReq{1001}})));
  const auto resp = binaryCodec_.decodeResponse(binaryClient.recvFrame());

  // Assert
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(std::get<proto::AccountResp>(resp.value().body).balance, 112000)
      << "binary port 應該看得到 json port 造成的變動";
}

// === 統計（Step 10）========================================================

// ★ 這一條是「引擎能不能報告自己」的端對端證明。
//
//   數字必須是活的：先製造一些流量，再問一次，看它有沒有動。
//   一個永遠回 0 的 stats 端點會通過任何「格式正確」的測試。
TEST_F(LedgerServerFixture, StatsReportsLiveCounters) {
  // Arrange —— 先跑一筆成功、一筆失敗的轉帳，讓計數器有東西可報。
  TestClient client(jsonPort_);
  ASSERT_TRUE(client.connected());

  ASSERT_TRUE(
      client.sendAll(R"({"id":"1","type":"transfer","idem_key":"s1","from":"1001","to":"2002",)"
                     R"("amount":"100","ccy":"USD"})"
                     "\n"));
  ASSERT_TRUE(jsonCodec_.decodeResponse(client.recvLine()).ok());

  ASSERT_TRUE(
      client.sendAll(R"({"id":"2","type":"transfer","idem_key":"s2","from":"2002","to":"1001",)"
                     R"("amount":"99999999","ccy":"USD"})"
                     "\n"));
  ASSERT_TRUE(jsonCodec_.decodeResponse(client.recvLine()).ok());

  // Act
  ASSERT_TRUE(client.sendAll(R"({"id":"3","type":"get_stats"})"
                             "\n"));
  const auto resp = jsonCodec_.decodeResponse(client.recvLine());

  // Assert
  ASSERT_TRUE(resp.ok());
  const auto* stats = std::get_if<proto::StatsResp>(&resp.value().body);
  ASSERT_TRUE(stats != nullptr) << "get_stats 應該回一則 stats";
  EXPECT_EQ(resp.value().reqId, 3U);

  // 帳本側：示範帳戶是 5 個（1001、2002、1003、9001、9002）。
  EXPECT_EQ(stats->accounts, 5);
  EXPECT_GE(stats->transfersCommitted, 1) << "剛剛那筆成功的轉帳沒被算到";
  EXPECT_GE(stats->transfersRejected, 1) << "剛剛那筆被拒的轉帳沒被算到";

  // 伺服器側：fixture 用 4 binary / 2 json worker。
  // 這兩個數字證明 stats 真的問到了 LedgerServer，不是回一組零。
  EXPECT_EQ(stats->binaryWorkers, 4);
  EXPECT_EQ(stats->jsonWorkers, 2);
  EXPECT_GT(stats->binaryQueueCapacity, 0);
  EXPECT_GT(stats->jsonQueueCapacity, 0);

  EXPECT_GE(stats->connectionsActive, 1) << "問問題的這條連線自己也算";
  EXPECT_GE(stats->connectionsTotal, 1);

  // 這個請求本身是走 json pool 的，所以它至少被提交過一次。
  EXPECT_GE(stats->jsonSubmitted, 3);
  EXPECT_GE(stats->jsonCompleted, 2) << "前面兩筆轉帳一定已經做完了";

  // binary pool 完全沒被用到，計數必須是零 —— 兩個池是分開的。
  EXPECT_EQ(stats->binarySubmitted, 0) << "binary pool 不該因為 json 的流量而增加";
}

TEST_F(LedgerServerFixture, StatsIsAvailableOnTheBinaryPortToo) {
  // Arrange —— 同一則訊息，另一種編碼。
  TestClient client(binaryPort_);
  ASSERT_TRUE(client.connected());

  // Act
  ASSERT_TRUE(client.sendAll(binaryCodec_.encodeRequest({55, proto::GetStatsReq{}})));
  const auto resp = binaryCodec_.decodeResponse(client.recvFrame());

  // Assert
  ASSERT_TRUE(resp.ok());
  const auto* stats = std::get_if<proto::StatsResp>(&resp.value().body);
  ASSERT_TRUE(stats != nullptr);
  EXPECT_EQ(resp.value().reqId, 55U);
  EXPECT_EQ(stats->accounts, 5);

  // 這次是 binary pool 在服務，所以換成它有數字、json pool 是零。
  EXPECT_GE(stats->binarySubmitted, 1);
  EXPECT_EQ(stats->jsonSubmitted, 0);
}

// ★ 監控不能拖慢被監控的東西。
//
//   如果 get_stats 走的是 audit() 或 verifyInvariants()，它會拿
//   auditMutex_ 的 unique_lock —— 也就是停下所有轉帳。這條測試在
//   持續的轉帳流量下連問 200 次，時間必須維持在很小的範圍內。
TEST_F(LedgerServerFixture, StatsStaysCheapUnderTraffic) {
  // Arrange —— 先把帳本撐到一個「有意義的規模」，再製造流量。
  //
  //   ★ 這一步是這條測試能不能成立的關鍵，而且是實測出來的。
  //
  //   只有示範用的 5 個帳戶時，verifyInvariants() 快到量不出來 ——
  //   把它加進 get_stats 的路徑裡，200 次查詢仍然只要 24 ms，測試照過。
  //   那樣這條測試就是個永遠不會紅的裝飾品。
  //
  //   對帳的成本是隨帳戶數成長的（每個帳戶都要取鎖並從分錄重算），
  //   所以先開 20000 個帳戶，讓「停下全世界重算一遍」真的付得出代價。
  //   這也才對應到真實情況：正式環境的帳本不會只有五個帳戶。
  constexpr int kExtraAccounts = 20000;
  for (OwnerId owner = 0; owner < kExtraAccounts; ++owner) {
    ASSERT_TRUE(registry_.create(100000 + owner, Currency::USD).ok());
  }

  constexpr int kTrafficClients = 4;
  constexpr int kTransfersEach = 300;

  std::atomic<int> stillRunning{kTrafficClients};
  std::vector<std::thread> traffic;
  for (int t = 0; t < kTrafficClients; ++t) {
    traffic.emplace_back([&, t] {
      TestClient worker(jsonPort_);
      if (worker.connected()) {
        for (int i = 0; i < kTransfersEach; ++i) {
          const std::string body = std::string(R"({"id":"1","type":"transfer","idem_key":"load-)") +
                                   std::to_string(t) + "-" + std::to_string(i) +
                                   R"(","from":"1001","to":"2002","amount":"1","ccy":"USD"})" +
                                   "\n";
          if (!worker.sendAll(body) || worker.recvLine().empty()) {
            break;
          }
        }
      }
      stillRunning.fetch_sub(1, std::memory_order_relaxed);
    });
  }

  TestClient prober(jsonPort_);
  ASSERT_TRUE(prober.connected());

  // Act —— 在流量進行中連問 200 次統計，量它花多久。
  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 200; ++i) {
    ASSERT_TRUE(prober.sendAll(R"({"id":"1","type":"get_stats"})"
                               "\n"));
    ASSERT_FALSE(prober.recvLine().empty()) << "第 " << i << " 次查詢沒有回應";
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  for (auto& thread : traffic) {
    thread.join();
  }

  // Assert —— 這個上界是量出來的，不是猜的。
  //
  //   乾淨版本   ：約 38 ms（三次量測 37/38/39）
  //   加上對帳鎖 ：1637 ms
  //
  //   800 ms 落在中間，兩邊都有充裕的餘裕：比正常值高 20 倍（不會在
  //   忙碌的 CI 上偶發變紅），比破壞版本低一半（不會漏抓）。
  //   它抓的不是「慢了一點」，而是「每次查詢都停下全世界重算整本帳」
  //   那種數量級的差異。見 progress.md 附錄 B。
  EXPECT_LT(elapsed.count(), 800) << "200 次 get_stats 花了 " << elapsed.count()
                                  << " ms —— 統計路徑疑似在拿帳本的對帳鎖";
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
  // Arrange
  constexpr int kClients = 8;
  constexpr int kPerClient = 50;
  const Amount before = balanceOf(1001) + balanceOf(2002);
  std::atomic<int> succeeded{0};
  std::atomic<int> answered{0};

  // Act —— 八條連線同時打，全部 join 之後才判決。
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

  // Assert
  EXPECT_EQ(answered.load(), kClients * kPerClient) << "每一筆請求都必須有回應";
  EXPECT_GT(succeeded.load(), 0);

  // ★ 守恆定律：不管成功幾筆、順序如何，兩個帳戶的總額必須分毫不差。
  //   少一分錢就是 lost update —— 而 lost update 不會報錯，只會讓錢消失。
  EXPECT_EQ(balanceOf(1001) + balanceOf(2002), before);
  EXPECT_EQ(core_->totalBalance(Currency::USD), 0);
}

TEST_F(LedgerServerFixture, SurvivesClientsDisconnectingMidFlight) {
  // Act —— client 送出請求後立刻斷線。worker 完成時 weak_ptr 已經失效，
  // 結果應該被安靜丟棄，而不是崩潰（W2 不變式）。
  for (int i = 0; i < 50; ++i) {
    TestClient client(jsonPort_);
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.sendAll("{\"id\":\"1\",\"type\":\"ping\"}\n"));
    client.closeNow();  // 不等回應
  }

  // Assert —— 伺服器必須還活著。
  TestClient survivor(jsonPort_);
  ASSERT_TRUE(survivor.connected());
  ASSERT_TRUE(survivor.sendAll("{\"id\":\"99\",\"type\":\"ping\"}\n"));
  const auto resp = jsonCodec_.decodeResponse(survivor.recvLine());
  ASSERT_TRUE(resp.ok());
  EXPECT_EQ(resp.value().reqId, 99U);
}

TEST_F(LedgerServerFixture, ManyConnectionsAtOnce) {
  // Arrange —— 30 條連線同時存在。
  constexpr int kClients = 30;
  std::vector<std::unique_ptr<TestClient>> clients;
  clients.reserve(kClients);
  for (int i = 0; i < kClients; ++i) {
    clients.push_back(std::make_unique<TestClient>(jsonPort_));
    ASSERT_TRUE(clients.back()->connected()) << "第 " << i << " 條連線失敗";
  }

  // Act —— 每條各送一則帶著自己 id 的 ping。
  for (int i = 0; i < kClients; ++i) {
    ASSERT_TRUE(clients[static_cast<std::size_t>(i)]->sendAll("{\"id\":\"" + std::to_string(i) +
                                                              "\",\"type\":\"ping\"}\n"));
  }

  // Assert —— 每個回應都必須回到正確的那條連線上。
  for (int i = 0; i < kClients; ++i) {
    const auto resp = jsonCodec_.decodeResponse(clients[static_cast<std::size_t>(i)]->recvLine());
    ASSERT_TRUE(resp.ok()) << "第 " << i << " 條連線沒收到回應";
    EXPECT_EQ(resp.value().reqId, static_cast<std::uint32_t>(i)) << "回應跑到錯誤的連線上了";
  }
}

}  // namespace
}  // namespace ledger::net
