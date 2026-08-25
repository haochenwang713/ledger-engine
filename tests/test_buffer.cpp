// ---------------------------------------------------------------------------
// Buffer 的單元測試。
//
// Buffer 完全沒有系統呼叫，所以這個檔案在 macOS 上也能跑 ——
// 這是刻意的設計：TCP 串流最容易出錯的地方（半包、黏包、緩衝成長）
// 都可以在沒有網路的情況下測乾淨。
// ---------------------------------------------------------------------------

#include <ledger/net/Buffer.h>

#include <gtest/gtest.h>

#include <string>

namespace ledger::net {
namespace {

TEST(Buffer, StartsEmptyWithPrependSpace) {
  Buffer buf;
  EXPECT_EQ(buf.readableBytes(), 0u);
  EXPECT_EQ(buf.prependableBytes(), Buffer::kPrepend);
  EXPECT_TRUE(buf.empty());
}

TEST(Buffer, AppendThenRetrieve) {
  Buffer buf;
  buf.append("hello");
  EXPECT_EQ(buf.readableBytes(), 5u);
  EXPECT_EQ(buf.view(), "hello");

  EXPECT_EQ(buf.retrieveAsString(5), "hello");
  EXPECT_TRUE(buf.empty());
}

TEST(Buffer, PartialRetrieveLeavesRemainder) {
  Buffer buf;
  buf.append("hello world");

  EXPECT_EQ(buf.retrieveAsString(6), "hello ");
  EXPECT_EQ(buf.readableBytes(), 5u);
  EXPECT_EQ(buf.view(), "world");
}

// ★ 半包：資料分兩次到達，Buffer 必須把它們接起來。
TEST(Buffer, AccumulatesAcrossMultipleAppends) {
  Buffer buf;
  buf.append("hel");
  EXPECT_EQ(buf.view(), "hel");  // 還不完整，上層應該什麼都不做

  buf.append("lo");
  EXPECT_EQ(buf.view(), "hello");  // 現在完整了
}

// ★ 黏包：一次讀到多個訊息，上層要能逐一切出來。
TEST(Buffer, SupportsConsumingMessagesOneByOne) {
  Buffer buf;
  buf.append("msg1|msg2|msg3|");

  std::vector<std::string> messages;
  while (true) {
    const std::string_view v = buf.view();
    const std::size_t pos = v.find('|');
    if (pos == std::string_view::npos) {
      break;
    }
    messages.emplace_back(v.substr(0, pos));
    buf.retrieve(pos + 1);
  }

  ASSERT_EQ(messages.size(), 3u);
  EXPECT_EQ(messages[0], "msg1");
  EXPECT_EQ(messages[2], "msg3");
  EXPECT_TRUE(buf.empty());
}

TEST(Buffer, GrowsWhenNeeded) {
  Buffer buf(64);
  const std::size_t initialCapacity = buf.capacity();

  const std::string big(10000, 'x');
  buf.append(big);

  EXPECT_GT(buf.capacity(), initialCapacity);
  EXPECT_EQ(buf.readableBytes(), big.size());
  EXPECT_EQ(buf.retrieveAllAsString(), big);
}

// ★ 這條驗證的是「回收前面已消費空間」的策略真的有生效。
//   一條持續收發的連線應該重複使用同一塊記憶體，而不是無止盡成長。
TEST(Buffer, ReusesSpaceInsteadOfGrowingForever) {
  Buffer buf(1024);
  const std::size_t capacityAtStart = buf.capacity();

  // 模擬一條長時間收發的連線：寫入、消費、再寫入，重複幾千次。
  for (int i = 0; i < 5000; ++i) {
    buf.append(std::string(200, 'a'));
    EXPECT_EQ(buf.retrieveAsString(200).size(), 200u);
  }

  // 若 makeSpace 的回收策略沒生效，容量會膨脹到 5000 * 200 = 1MB。
  EXPECT_EQ(buf.capacity(), capacityAtStart) << "緩衝一直成長 —— 回收已消費空間的策略沒有生效";
  EXPECT_TRUE(buf.empty());
}

TEST(Buffer, EnsureWritableGuaranteesSpace) {
  Buffer buf(16);
  buf.ensureWritable(5000);
  EXPECT_GE(buf.writableBytes(), 5000u);

  // beginWrite / hasWritten 是 Connection 讀取 socket 時用的模式
  std::memset(buf.beginWrite(), 'z', 5000);
  buf.hasWritten(5000);
  EXPECT_EQ(buf.readableBytes(), 5000u);
}

TEST(Buffer, RetrieveMoreThanAvailableIsSafe) {
  Buffer buf;
  buf.append("abc");
  buf.retrieve(9999);  // 要求超過現有的量
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.prependableBytes(), Buffer::kPrepend);  // 游標重置了
}

}  // namespace
}  // namespace ledger::net
