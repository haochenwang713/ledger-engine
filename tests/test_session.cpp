// ---------------------------------------------------------------------------
// Session 測試 —— 半包、黏包、兩種失敗。
//
// 這一層平台無關，所以 macOS 上也跑得到。它涵蓋的是 Stage 5c 裡
// 唯一有分支邏輯的地方：位元組流怎麼變成一串請求，以及兩種失敗
// 分別該怎麼處理。
// ---------------------------------------------------------------------------

#include <ledger/proto/BinaryCodec.h>
#include <ledger/proto/FrameSplitter.h>
#include <ledger/proto/JsonCodec.h>
#include <ledger/proto/Session.h>

#include <gtest/gtest.h>

#include <string>
#include <variant>

using namespace ledger;
using namespace ledger::proto;

namespace {

const LengthPrefixSplitter kLengthSplitter;
const NewlineSplitter kNewlineSplitter;
const BinaryCodec kBinaryCodec;
const JsonCodec kJsonCodec;

Session binarySession() {
  return Session{kLengthSplitter, kBinaryCodec};
}
Session jsonSession() {
  return Session{kNewlineSplitter, kJsonCodec};
}

TransferReq sampleTransfer() {
  return TransferReq{"req-a3f9-01", 1001, 2002, 5000, Currency::USD};
}

}  // namespace

// === 正常路徑 ==============================================================

TEST(Session, DecodesOneBinaryRequest) {
  const Session session = binarySession();
  const std::string wire = kBinaryCodec.encodeRequest({7, sampleTransfer()});

  const Session::Outcome out = session.drain(wire);

  EXPECT_FALSE(out.fatal);
  EXPECT_EQ(out.consumed, wire.size());
  EXPECT_TRUE(out.errors.empty());
  ASSERT_EQ(out.requests.size(), 1U);
  EXPECT_EQ(out.requests[0].reqId, 7U);
  EXPECT_EQ(std::get<TransferReq>(out.requests[0].body).amount, 5000);
}

TEST(Session, DecodesOneJsonRequest) {
  const Session session = jsonSession();
  const std::string wire = kJsonCodec.encodeRequest({7, sampleTransfer()});

  const Session::Outcome out = session.drain(wire);

  EXPECT_FALSE(out.fatal);
  EXPECT_EQ(out.consumed, wire.size());
  ASSERT_EQ(out.requests.size(), 1U);
  EXPECT_EQ(out.requests[0].reqId, 7U);
}

TEST(Session, SplitsGluedRequestsInOneCall) {
  // TCP 是位元組流。三則訊息擠在同一次 read 是常態，不是例外。
  const Session session = binarySession();
  const std::string wire = kBinaryCodec.encodeRequest({1, PingReq{}}) +
                           kBinaryCodec.encodeRequest({2, sampleTransfer()}) +
                           kBinaryCodec.encodeRequest({3, GetAccountReq{1001}});

  const Session::Outcome out = session.drain(wire);

  EXPECT_EQ(out.consumed, wire.size());
  ASSERT_EQ(out.requests.size(), 3U);
  EXPECT_EQ(out.requests[0].reqId, 1U);
  EXPECT_EQ(out.requests[1].reqId, 2U);
  EXPECT_EQ(out.requests[2].reqId, 3U);
}

// ★ 半包：consumed 絕對不能包含還沒收齊的那一則。
//
//   多算一個位元組，下一次讀取事件就會從錯誤的位置開始切包，
//   之後的所有訊息全部錯位 —— 而且錯誤會出現在「後來」的訊息上，
//   讓人往完全錯誤的方向查。
TEST(Session, PartialRequestIsLeftInTheBuffer) {
  const Session session = binarySession();
  const std::string complete = kBinaryCodec.encodeRequest({1, PingReq{}});
  const std::string wire = complete + kBinaryCodec.encodeRequest({2, sampleTransfer()});

  // 只給第一則加上第二則的前 5 個位元組。
  const Session::Outcome out = session.drain(std::string_view{wire}.substr(0, complete.size() + 5));

  EXPECT_FALSE(out.fatal);
  EXPECT_EQ(out.consumed, complete.size()) << "半包的位元組不該被消費";
  ASSERT_EQ(out.requests.size(), 1U);
  EXPECT_EQ(out.requests[0].reqId, 1U);
}

TEST(Session, EveryPrefixOfAFrameIsSafe) {
  // 每一種切法都不該產生請求，也不該消費任何位元組。
  const Session session = binarySession();
  const std::string wire = kBinaryCodec.encodeRequest({1, sampleTransfer()});

  for (std::size_t cut = 0; cut < wire.size(); ++cut) {
    const Session::Outcome out = session.drain(std::string_view{wire}.substr(0, cut));
    EXPECT_TRUE(out.requests.empty()) << "在 " << cut << " 位元組時不該切出請求";
    EXPECT_EQ(out.consumed, 0U) << "在 " << cut << " 位元組時不該消費";
    EXPECT_FALSE(out.fatal);
  }
}

// === 兩種失敗 ==============================================================

// 解碼失敗：框架完整，內容看不懂。跳過這一則，繼續下一則。
TEST(Session, DecodeErrorIsRecoverable) {
  const Session session = jsonSession();
  const std::string wire = "not json at all\n" + kJsonCodec.encodeRequest({9, GetAccountReq{1001}});

  const Session::Outcome out = session.drain(wire);

  EXPECT_FALSE(out.fatal) << "解碼失敗不該關閉連線 —— 框架邊界是已知的";
  EXPECT_EQ(out.consumed, wire.size());
  ASSERT_EQ(out.errors.size(), 1U);
  ASSERT_EQ(out.requests.size(), 1U) << "壞的那一則之後，好的那一則仍要被處理";
  EXPECT_EQ(out.requests[0].reqId, 9U);
}

// ★ 框架失敗：位元組流無法對齊，唯一正確的做法是關閉連線。
//
//   把它當成可恢復的錯誤，會讓連線陷入
//   「解出垃圾 → 回錯誤 → 再解出垃圾」的無限迴圈。
TEST(Session, FramingErrorIsFatal) {
  const Session session = binarySession();
  const std::string wire = "\xFF\xFF\xFF\xFF";  // len = 4294967295

  const Session::Outcome out = session.drain(wire);

  EXPECT_TRUE(out.fatal);
  ASSERT_EQ(out.errors.size(), 1U);
  const auto* err = std::get_if<ErrorResp>(&out.errors[0].body);
  ASSERT_TRUE(err != nullptr);
  EXPECT_EQ(err->code, ErrorCode::FrameTooLarge);
  EXPECT_TRUE(out.requests.empty());
}

TEST(Session, FatalErrorStopsProcessingLaterFrames) {
  // 框架壞掉之後的位元組完全不能信任 —— 就算它們「看起來」像合法訊息。
  const Session session = binarySession();
  const std::string wire =
      std::string("\xFF\xFF\xFF\xFF", 4) + kBinaryCodec.encodeRequest({1, PingReq{}});

  const Session::Outcome out = session.drain(wire);

  EXPECT_TRUE(out.fatal);
  EXPECT_TRUE(out.requests.empty()) << "框架失敗之後不該再解析任何東西";
}

TEST(Session, UnterminatedJsonLineIsFatalOnceTooLong) {
  const NewlineSplitter tinySplitter{64};
  const Session session{tinySplitter, kJsonCodec};

  const Session::Outcome out = session.drain(std::string(200, 'x'));  // 沒有換行

  EXPECT_TRUE(out.fatal);
  ASSERT_EQ(out.errors.size(), 1U);
  EXPECT_EQ(std::get<ErrorResp>(out.errors[0].body).code, ErrorCode::FrameTooLarge);
}

// === 給人用的細節 ==========================================================

// 空行是使用者在 nc 裡多按了一次 Enter。JSON port 的存在意義就是
// 讓人手動測試，因為多按一次就被斷線很沒有道理。
TEST(Session, BlankJsonLinesAreIgnored) {
  const Session session = jsonSession();
  const std::string wire = "\n\n" + kJsonCodec.encodeRequest({5, PingReq{}}) + "\n";

  const Session::Outcome out = session.drain(wire);

  EXPECT_FALSE(out.fatal);
  EXPECT_EQ(out.consumed, wire.size());
  EXPECT_TRUE(out.errors.empty()) << "空行不該產生錯誤回應";
  ASSERT_EQ(out.requests.size(), 1U);
  EXPECT_EQ(out.requests[0].reqId, 5U);
}

TEST(Session, HandlesCrLfLineEndings) {
  const Session session = jsonSession();
  const std::string wire = R"({"id":"3","type":"ping"})"
                           "\r\n";

  const Session::Outcome out = session.drain(wire);

  EXPECT_EQ(out.consumed, wire.size());
  ASSERT_EQ(out.requests.size(), 1U);
  EXPECT_EQ(out.requests[0].reqId, 3U);
}

TEST(Session, EmptyInputIsANoOp) {
  const Session session = binarySession();
  const Session::Outcome out = session.drain("");

  EXPECT_FALSE(out.fatal);
  EXPECT_EQ(out.consumed, 0U);
  EXPECT_TRUE(out.requests.empty());
  EXPECT_TRUE(out.errors.empty());
}

TEST(Session, ReportsItsCodecTag) {
  EXPECT_EQ(binarySession().codecTag(), CodecTag::Binary);
  EXPECT_EQ(jsonSession().codecTag(), CodecTag::Json);
}
