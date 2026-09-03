// ---------------------------------------------------------------------------
// Stage 5a —— 協定層測試。
//
// 這個檔案有一個比「測試」更重要的身分：★ 它是協定的規格書。
//
// 下面那張 golden 表格寫死了每則訊息的位元組級表示。任何人重排
// fields() 的順序、改欄位名字、動 MsgType 的數值，這裡就會紅 ——
// 而那正是我們要抓的東西。
//
// 為什麼 golden 表格不可省略：欄位描述表讓 binary 與 JSON 共用同一份
// 順序，好處是兩者不會互相漂移；但它也意味著「重排 fields()」會
// 同時改變 binary 的 wire format，而 JSON 完全不受影響（它靠名字對應）。
// round-trip 測試抓不到這件事 —— 編碼與解碼一起改了，自己跟自己永遠一致。
// 只有寫死的位元組能抓到。
//
// Test structure: Arrange-Act-Assert. Golden-table tests are "Arrange the
// expected bytes, Act by encoding, Assert they match" and then run the same
// comparison in reverse. See instruction.md for the convention.
// ---------------------------------------------------------------------------

#include <ledger/proto/BinaryCodec.h>
#include <ledger/proto/FrameSplitter.h>
#include <ledger/proto/JsonCodec.h>
#include <ledger/proto/Messages.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

using namespace ledger;
using namespace ledger::proto;

namespace {

std::string toHex(const std::string& raw) {
  static constexpr char kDigits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(raw.size() * 2);
  for (const char raw_c : raw) {
    const auto c = static_cast<unsigned char>(raw_c);
    out.push_back(kDigits[c >> 4]);
    out.push_back(kDigits[c & 0x0F]);
  }
  return out;
}

std::string fromHex(const std::string& hex) {
  std::string out;
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const auto hi = static_cast<char>(hex[i] <= '9' ? hex[i] - '0' : hex[i] - 'A' + 10);
    const auto lo = static_cast<char>(hex[i + 1] <= '9' ? hex[i + 1] - '0' : hex[i + 1] - 'A' + 10);
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

/// 剝掉長度前綴，得到 BinaryCodec 看得懂的那一段。
/// 正式流程裡這件事由 LengthPrefixSplitter 做。
std::string stripPrefix(const std::string& framed) {
  return framed.substr(LengthPrefixSplitter::kPrefixSize);
}

/// 剝掉 NDJSON 的結尾換行。
std::string stripNewline(const std::string& line) {
  return line.empty() ? line : line.substr(0, line.size() - 1);
}

const BinaryCodec kBin;
const JsonCodec kJson;

// 範例交易與 Stage 0 設計文件第 4 節、以及 db/seeds/dev_seed.sql 完全一致：
// Alice(1001) 轉 $50.00 給 Bob(2002)，結果 Alice=115000、Bob=47000。
TransferReq sampleTransfer() {
  return TransferReq{"req-a3f9-01", 1001, 2002, 5000, Currency::USD};
}

/// Step 10 的統計樣本。每個欄位都給不同的值 ——
/// 全給 0 或全給同一個數字的話，欄位順序寫錯也看不出來。
StatsResp sampleStats() {
  StatsResp stats;
  stats.uptimeMillis = 1234;
  stats.accounts = 5;
  stats.connectionsActive = 2;
  stats.connectionsTotal = 7;
  stats.transfersCommitted = 11;
  stats.transfersRejected = 3;
  stats.binaryWorkers = 20;
  stats.binaryQueueDepth = 1;
  stats.binaryQueueCapacity = 8192;
  stats.binarySubmitted = 100;
  stats.binaryCompleted = 99;
  stats.binaryRejected = 4;
  stats.binaryDropped = 2;
  stats.jsonWorkers = 4;
  stats.jsonQueueDepth = 0;
  stats.jsonQueueCapacity = 1024;
  stats.jsonSubmitted = 50;
  stats.jsonCompleted = 50;
  stats.jsonRejected = 0;
  stats.jsonDropped = 1;
  return stats;
}

}  // namespace

// ===========================================================================
// ★ Golden 表格 —— 協定的位元組級規格
// ===========================================================================

TEST(CodecGolden, TransferRequestBinaryLayout) {
  //  00000030            len = 48（表頭 8 + payload 40），不含自己
  //  0002                type = Transfer
  //  0001                ver  = 1
  //  00000007            reqId = 7
  //  000B 7265712D...    idemKey：長度 11 + "req-a3f9-01"
  //  00000000000003E9    from   = 1001
  //  00000000000007D2    to     = 2002
  //  0000000000001388    amount = 5000
  //  555344              ccy    = "USD"（三個 ASCII，不是 enum 值）
  // Arrange —— 寫死的期望位元組
  const std::string kGolden =
      "000000300002000100000007"
      "000B7265712D613366392D3031"
      "00000000000003E9"
      "00000000000007D2"
      "0000000000001388"
      "555344";

  // Act & Assert（正向：編碼必須產生這些位元組）
  const RequestEnvelope env{7, sampleTransfer()};
  EXPECT_EQ(toHex(kBin.encodeRequest(env)), kGolden);

  // Act & Assert（反向：寫死的位元組必須解回一模一樣的值）
  const auto decoded = kBin.decodeRequest(stripPrefix(fromHex(kGolden)));
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().reqId, 7U);
  const auto* req = std::get_if<TransferReq>(&decoded.value().body);
  ASSERT_TRUE(req != nullptr);
  EXPECT_EQ(req->idemKey, "req-a3f9-01");
  EXPECT_EQ(req->from, 1001);
  EXPECT_EQ(req->to, 2002);
  EXPECT_EQ(req->amount, 5000);
  EXPECT_EQ(req->ccy, Currency::USD);
}

TEST(CodecGolden, TransferRequestJsonLayout) {
  // key 的順序是字典序 —— nlohmann::json 預設用 std::map 存物件。
  // 這對協定沒有影響（JSON 靠名字對應），但 golden 字串必須照它寫。
  // Arrange
  const std::string kGolden = R"({"amount":"5000","ccy":"USD","from":"1001","id":"7",)"
                              R"("idem_key":"req-a3f9-01","to":"2002","type":"transfer","v":1})";

  // Act & Assert
  const RequestEnvelope env{7, sampleTransfer()};
  EXPECT_EQ(stripNewline(kJson.encodeRequest(env)), kGolden);
  EXPECT_EQ(kJson.encodeRequest(env).back(), '\n');  // NDJSON 必須有結尾換行

  // Act & Assert（反向）
  const auto decoded = kJson.decodeRequest(kGolden);
  ASSERT_TRUE(decoded.ok());
  const auto* req = std::get_if<TransferReq>(&decoded.value().body);
  ASSERT_TRUE(req != nullptr);
  EXPECT_EQ(req->from, 1001);
  EXPECT_EQ(req->amount, 5000);
}

TEST(CodecGolden, PingHasEmptyPayload) {
  // Arrange
  const RequestEnvelope env{1, PingReq{}};
  // len = 8 表示 payload 是空的 —— 只有表頭。
  // Act & Assert
  EXPECT_EQ(toHex(kBin.encodeRequest(env)), "000000080001000100000001");
  EXPECT_EQ(stripNewline(kJson.encodeRequest(env)), R"({"id":"1","type":"ping","v":1})");
}

TEST(CodecGolden, TransferOkResponseLayout) {
  // Arrange
  const ResponseEnvelope env{7, TransferOkResp{900001, 115000, 47000}};
  // Act & Assert
  EXPECT_EQ(toHex(kBin.encodeResponse(env)),
            "00000020800200010000000700000000000DBBA1000000000001C138000000000000B798");
  EXPECT_EQ(stripNewline(kJson.encodeResponse(env)),
            R"({"from_balance":"115000","id":"7","to_balance":"47000",)"
            R"("tx_id":"900001","type":"transfer_ok","v":1})");
}

TEST(CodecGolden, AccountResponseLayout) {
  // Arrange
  const ResponseEnvelope env{9, AccountResp{1001, 115000, Currency::JPY, AccountStatus::Closed}};
  //  ...4A5059  = "JPY"
  //  ...01      = AccountStatus::Closed
  // Act & Assert
  EXPECT_EQ(toHex(kBin.encodeResponse(env)),
            "0000001C800300010000000900000000000003E9000000000001C1384A505901");
  EXPECT_EQ(stripNewline(kJson.encodeResponse(env)),
            R"({"balance":"115000","ccy":"JPY","id":"1001","status":"CLOSED",)"
            R"("type":"account","v":1})");
}

TEST(CodecGolden, ErrorResponseLayout) {
  // Arrange
  const ResponseEnvelope env = makeError(7, ErrorCode::InsufficientFunds, "balance 4200 < 5000");
  //  0007 = ErrorCode::InsufficientFunds 的數值
  //  0013 = 訊息長度 19
  // Act & Assert
  EXPECT_EQ(toHex(kBin.encodeResponse(env)),
            "0000001F80FF0001000000070007001362616C616E63652034323030203C2035303030");
  // JSON 用名字而不是數字 —— 前端不需要知道 enum 的數值。
  EXPECT_EQ(stripNewline(kJson.encodeResponse(env)),
            R"({"code":"INSUFFICIENT_FUNDS","id":"7",)"
            R"("message":"balance 4200 < 5000","type":"error","v":1})");
}

TEST(CodecGolden, GetAccountLayout) {
  // Arrange, Act & Assert —— 這則訊息只有一個欄位，三個階段各一行。
  const RequestEnvelope env{3, GetAccountReq{2002}};
  EXPECT_EQ(toHex(kBin.encodeRequest(env)), "00000010000300010000000300000000000007D2");
  EXPECT_EQ(stripNewline(kJson.encodeRequest(env)),
            R"({"account_id":"2002","id":"3","type":"get_account","v":1})");
}

TEST(CodecGolden, GetStatsRequestLayout) {
  // Arrange, Act & Assert —— 沒有 payload，所以 len = 8（只有表頭）。
  const RequestEnvelope env{4, GetStatsReq{}};
  EXPECT_EQ(toHex(kBin.encodeRequest(env)), "000000080004000100000004");
  EXPECT_EQ(stripNewline(kJson.encodeRequest(env)), R"({"id":"4","type":"get_stats","v":1})");
}

TEST(CodecGolden, StatsResponseLayout) {
  // Arrange —— 20 個 int64 欄位，順序就是 fields() 的順序。
  //
  //   len = 0xA8 = 168 = 表頭 8 + 20 × 8。
  //   8004 = MsgType::Stats，0001 = ver，0000000D = reqId 13。
  //   接著依序是 uptime_ms=0x4D2(1234)、accounts=5、connections_active=2、
  //   connections_total=7、transfers_committed=0x0B(11)、
  //   transfers_rejected=3、binary_workers=0x14(20)、binary_queue_depth=1、
  //   binary_queue_capacity=0x2000(8192)、binary_submitted=0x64(100)、
  //   binary_completed=0x63(99)、binary_rejected=4、binary_dropped=2、
  //   json_workers=4、json_queue_depth=0、json_queue_capacity=0x400(1024)、
  //   json_submitted=0x32(50)、json_completed=0x32(50)、json_rejected=0、
  //   json_dropped=1。
  //
  // ⚠ 這張表是規格，不是實作的輸出。上面那串註解就是它的來源 ——
  //   任何人重排 fields()，這裡會紅，而註解說得出「本來該是什麼」。
  const std::string kGolden =
      "000000A8800400010000000D"
      "00000000000004D2"   // uptime_ms = 1234
      "0000000000000005"   // accounts = 5
      "0000000000000002"   // connections_active = 2
      "0000000000000007"   // connections_total = 7
      "000000000000000B"   // transfers_committed = 11
      "0000000000000003"   // transfers_rejected = 3
      "0000000000000014"   // binary_workers = 20
      "0000000000000001"   // binary_queue_depth = 1
      "0000000000002000"   // binary_queue_capacity = 8192
      "0000000000000064"   // binary_submitted = 100
      "0000000000000063"   // binary_completed = 99
      "0000000000000004"   // binary_rejected = 4
      "0000000000000002"   // binary_dropped = 2
      "0000000000000004"   // json_workers = 4
      "0000000000000000"   // json_queue_depth = 0
      "0000000000000400"   // json_queue_capacity = 1024
      "0000000000000032"   // json_submitted = 50
      "0000000000000032"   // json_completed = 50
      "0000000000000000"   // json_rejected = 0
      "0000000000000001";  // json_dropped = 1

  const ResponseEnvelope env{13, sampleStats()};

  // Act & Assert（正向）
  EXPECT_EQ(toHex(kBin.encodeResponse(env)), kGolden);

  // Act & Assert（反向）—— 寫死的位元組必須解回一模一樣的值。
  const auto decoded = kBin.decodeResponse(stripPrefix(fromHex(kGolden)));
  ASSERT_TRUE(decoded.ok());
  const auto* stats = std::get_if<StatsResp>(&decoded.value().body);
  ASSERT_TRUE(stats != nullptr);
  EXPECT_EQ(stats->uptimeMillis, 1234);
  EXPECT_EQ(stats->binaryQueueCapacity, 8192);
  EXPECT_EQ(stats->jsonDropped, 1);
}

TEST(CodecGolden, StatsResponseJsonLayout) {
  // Arrange —— key 是字典序（nlohmann::json 用 std::map），所以
  // 這串看起來跟 fields() 的順序不一樣。JSON 靠名字對應，順序無關。
  const std::string kGolden =
      R"({"accounts":"5","binary_completed":"99","binary_dropped":"2",)"
      R"("binary_queue_capacity":"8192","binary_queue_depth":"1","binary_rejected":"4",)"
      R"("binary_submitted":"100","binary_workers":"20","connections_active":"2",)"
      R"("connections_total":"7","id":"13","json_completed":"50","json_dropped":"1",)"
      R"("json_queue_capacity":"1024","json_queue_depth":"0","json_rejected":"0",)"
      R"("json_submitted":"50","json_workers":"4","transfers_committed":"11",)"
      R"("transfers_rejected":"3","type":"stats","uptime_ms":"1234","v":1})";

  const ResponseEnvelope env{13, sampleStats()};

  // Act & Assert —— ★ 每一個數字都是帶引號的字串。
  //   儀表板是瀏覽器，裸數字在那裡會靜默失去精度。
  EXPECT_EQ(stripNewline(kJson.encodeResponse(env)), kGolden);

  const auto decoded = kJson.decodeResponse(kGolden);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(std::get<StatsResp>(decoded.value().body).binarySubmitted, 100);
}

// ===========================================================================
// Round-trip —— 編碼再解碼必須拿回原值
// ===========================================================================

TEST(CodecRoundTrip, AllRequestTypesBinary) {
  // Arrange
  const RequestEnvelope cases[] = {
      {1, PingReq{}},
      {2, sampleTransfer()},
      {3, GetAccountReq{2002}},
      {4, GetStatsReq{}},
  };

  for (const auto& env : cases) {
    // Act
    const auto decoded = kBin.decodeRequest(stripPrefix(kBin.encodeRequest(env)));

    // Assert
    ASSERT_TRUE(decoded.ok()) << "type=" << nameOfMsgType(typeOf(env.body));
    EXPECT_EQ(decoded.value().reqId, env.reqId);
    EXPECT_EQ(typeOf(decoded.value().body), typeOf(env.body));
  }
}

TEST(CodecRoundTrip, AllRequestTypesJson) {
  // Arrange
  const RequestEnvelope cases[] = {
      {1, PingReq{}},
      {2, sampleTransfer()},
      {3, GetAccountReq{2002}},
      {4, GetStatsReq{}},
  };

  for (const auto& env : cases) {
    // Act
    const auto decoded = kJson.decodeRequest(stripNewline(kJson.encodeRequest(env)));

    // Assert
    ASSERT_TRUE(decoded.ok()) << "type=" << nameOfMsgType(typeOf(env.body));
    EXPECT_EQ(decoded.value().reqId, env.reqId);
    EXPECT_EQ(typeOf(decoded.value().body), typeOf(env.body));
  }
}

TEST(CodecRoundTrip, AllResponseTypesBothCodecs) {
  // Arrange
  const ResponseEnvelope cases[] = {
      {1, PongResp{}},
      {2, TransferOkResp{900001, 115000, 47000}},
      {3, AccountResp{1001, 115000, Currency::TWD, AccountStatus::Active}},
      {4, ErrorResp{ErrorCode::CurrencyMismatch, "USD vs JPY"}},
      {5, sampleStats()},
  };

  for (const auto& env : cases) {
    // Act & Assert —— 同一個值走兩種編碼都必須回到原型別。
    const auto bin = kBin.decodeResponse(stripPrefix(kBin.encodeResponse(env)));
    ASSERT_TRUE(bin.ok());
    EXPECT_EQ(typeOf(bin.value().body), typeOf(env.body));

    const auto js = kJson.decodeResponse(stripNewline(kJson.encodeResponse(env)));
    ASSERT_TRUE(js.ok());
    EXPECT_EQ(typeOf(js.value().body), typeOf(env.body));
  }
}

TEST(CodecRoundTrip, ErrorMessageTextSurvives) {
  // Arrange
  const ResponseEnvelope env = makeError(1, ErrorCode::InsufficientFunds, "餘額不足");

  // Act
  const auto decoded = kJson.decodeResponse(stripNewline(kJson.encodeResponse(env)));

  // Assert
  ASSERT_TRUE(decoded.ok());
  const auto* err = std::get_if<ErrorResp>(&decoded.value().body);
  ASSERT_TRUE(err != nullptr);
  EXPECT_EQ(err->code, ErrorCode::InsufficientFunds);
  EXPECT_EQ(err->message, "餘額不足");
}

// ===========================================================================
// ★ int64 精度 —— JSON 用字串的理由
// ===========================================================================

TEST(CodecInt64, ValuesBeyondDoublePrecisionSurvive) {
  // 2^53 + 1。這個數字無法用 IEEE-754 double 精確表示 ——
  // 如果 JSON 用裸數字送，瀏覽器 JSON.parse() 之後會變成 2^53，
  // 而且沒有任何錯誤訊息。這就是所有整數一律字串化的理由。
  constexpr std::int64_t kBeyondDouble = 9007199254740993LL;

  // Arrange
  const RequestEnvelope env{1, TransferReq{"k", kBeyondDouble, 2, kBeyondDouble, Currency::JPY}};

  // Act & Assert（JSON）
  const auto js = kJson.decodeRequest(stripNewline(kJson.encodeRequest(env)));
  ASSERT_TRUE(js.ok());
  const auto* req = std::get_if<TransferReq>(&js.value().body);
  ASSERT_TRUE(req != nullptr);
  EXPECT_EQ(req->from, kBeyondDouble);
  EXPECT_EQ(req->amount, kBeyondDouble);

  // Act & Assert（binary）
  const auto bin = kBin.decodeRequest(stripPrefix(kBin.encodeRequest(env)));
  ASSERT_TRUE(bin.ok());
  EXPECT_EQ(std::get<TransferReq>(bin.value().body).amount, kBeyondDouble);
}

TEST(CodecInt64, ExtremesRoundTripInBothCodecs) {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

  // Arrange —— 負餘額是合法的，系統帳戶的 allow_negative 就是為此存在。
  const ResponseEnvelope env{1, TransferOkResp{kMax, kMin, 0}};

  // Act & Assert（binary）
  const auto bin = kBin.decodeResponse(stripPrefix(kBin.encodeResponse(env)));
  ASSERT_TRUE(bin.ok());
  const auto* b = std::get_if<TransferOkResp>(&bin.value().body);
  ASSERT_TRUE(b != nullptr);
  EXPECT_EQ(b->txId, kMax);
  EXPECT_EQ(b->fromBalance, kMin);

  // Act & Assert（JSON）
  const auto js = kJson.decodeResponse(stripNewline(kJson.encodeResponse(env)));
  ASSERT_TRUE(js.ok());
  const auto* j = std::get_if<TransferOkResp>(&js.value().body);
  ASSERT_TRUE(j != nullptr);
  EXPECT_EQ(j->txId, kMax);
  EXPECT_EQ(j->fromBalance, kMin);
}

TEST(CodecInt64, JsonRejectsBareNumbers) {
  // Act —— 前端某天忘記加引號（from 是裸數字）。
  const auto decoded =
      kJson.decodeRequest(R"({"id":"1","type":"transfer","idem_key":"k","from":1001,"to":2002,)"
                          R"("amount":"5000","ccy":"USD"})");

  // Assert —— 當場報錯，而不是「剛好這次能動」。
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error(), ErrorCode::IntegerNotString);
}

TEST(CodecInt64, JsonRejectsGarbageInsideString) {
  // from_chars 會高高興興地解析出 123 並把游標停在 'a'。
  // 不檢查「整個字串是否被吃完」的話，垃圾會被當成合法數字。
  for (const char* bad : {R"("123abc")", R"("12 ")", R"("")", R"(" 12")", R"("0x10")"}) {
    // Arrange
    const std::string frame =
        std::string(R"({"id":"1","type":"get_account","account_id":)") + bad + "}";

    // Act
    const auto decoded = kJson.decodeRequest(frame);

    // Assert
    EXPECT_FALSE(decoded.ok()) << "應該被拒絕: " << bad;
  }
}

TEST(CodecInt64, JsonRejectsOverflow) {
  // Act —— 比 int64 上限多一位數。from_chars 會回 result_out_of_range。
  const auto decoded =
      kJson.decodeRequest(R"({"id":"1","type":"get_account","account_id":"92233720368547758070"})");

  // Assert
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error(), ErrorCode::MalformedFrame);
}

// ===========================================================================
// 拒絕路徑
// ===========================================================================

TEST(CodecReject, JsonMissingField) {
  // Act
  const auto decoded = kJson.decodeRequest(
      R"({"id":"1","type":"transfer","idem_key":"k","from":"1001","to":"2002","ccy":"USD"})");

  // Assert
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error(), ErrorCode::MissingField);  // 少了 amount
}

TEST(CodecReject, JsonUnknownTypeName) {
  // Act
  const auto decoded = kJson.decodeRequest(R"({"id":"1","type":"withdraw"})");

  // Assert
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error(), ErrorCode::UnknownMessageType);
}

TEST(CodecReject, JsonNotAnObject) {
  // Act & Assert —— 三種都不是 JSON 物件。
  EXPECT_FALSE(kJson.decodeRequest("[1,2,3]").ok());
  EXPECT_FALSE(kJson.decodeRequest("not json at all").ok());
  EXPECT_FALSE(kJson.decodeRequest("").ok());
}

TEST(CodecReject, VersionMismatchIsExplicit) {
  // Act & Assert（JSON）—— ver 欄位不是裝飾品：不相符就拒絕，
  // 而且錯誤碼要說得出是版本問題。
  const auto js = kJson.decodeRequest(R"({"v":2,"id":"1","type":"ping"})");
  ASSERT_FALSE(js.ok());
  EXPECT_EQ(js.error(), ErrorCode::UnsupportedVersion);

  // Arrange（binary）—— 把 ver 從 0001 改成 0002
  std::string frame = stripPrefix(kBin.encodeRequest(RequestEnvelope{1, PingReq{}}));
  frame[3] = 0x02;

  // Act & Assert（binary）
  const auto bin = kBin.decodeRequest(frame);
  ASSERT_FALSE(bin.ok());
  EXPECT_EQ(bin.error(), ErrorCode::UnsupportedVersion);
}

TEST(CodecReject, JsonVersionMayBeOmitted) {
  // Act —— 沒有 v 欄位。
  const auto decoded = kJson.decodeRequest(R"({"id":"1","type":"ping"})");

  // Assert —— 省略 v 是允許的，否則 nc 手打測試會很痛苦，
  // 而那正是 JSON port 的主要價值。
  EXPECT_TRUE(decoded.ok());
}

TEST(CodecReject, ResponseTypeInRequestDirection) {
  // Act & Assert —— 有人往 server 送一則回應。
  // 這是協定違規，不是「型別不認識」。
  const auto bin = kBin.decodeRequest(stripPrefix(kBin.encodeResponse({1, PongResp{}})));
  ASSERT_FALSE(bin.ok());
  EXPECT_EQ(bin.error(), ErrorCode::UnknownMessageType);

  const auto js = kJson.decodeRequest(R"({"id":"1","type":"pong"})");
  ASSERT_FALSE(js.ok());
  EXPECT_EQ(js.error(), ErrorCode::UnknownMessageType);
}

TEST(CodecReject, BinaryTruncatedPayload) {
  // Arrange —— 表頭說是 Transfer，但 payload 只剩一半。
  const std::string full = stripPrefix(kBin.encodeRequest({1, sampleTransfer()}));

  // Act & Assert —— 每一種截斷長度都必須被拒絕。
  for (std::size_t cut = 1; cut < full.size(); ++cut) {
    const auto decoded = kBin.decodeRequest(full.substr(0, cut));
    EXPECT_FALSE(decoded.ok()) << "截斷到 " << cut << " 位元組不該被接受";
  }
}

TEST(CodecReject, BinaryTrailingGarbage) {
  // Arrange —— 多出來的位元組代表雙方對欄位的認知不一致。
  const std::string frame = stripPrefix(kBin.encodeRequest({1, sampleTransfer()})) + "XX";

  // Act
  const auto decoded = kBin.decodeRequest(frame);

  // Assert —— 寧可拒絕也不要安靜忽略。
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error(), ErrorCode::MalformedFrame);
}

TEST(CodecReject, BinaryUnknownCurrency) {
  // Arrange —— 最後三個位元組是幣別代碼，改成不存在的 "XYZ"。
  std::string frame = stripPrefix(kBin.encodeRequest({1, sampleTransfer()}));
  frame[frame.size() - 3] = 'X';
  frame[frame.size() - 2] = 'Y';
  frame[frame.size() - 1] = 'Z';

  // Act
  const auto decoded = kBin.decodeRequest(frame);

  // Assert
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error(), ErrorCode::MalformedFrame);
}

TEST(CodecReject, JsonUnknownCurrency) {
  // Act
  const auto decoded =
      kJson.decodeRequest(R"({"id":"1","type":"transfer","idem_key":"k","from":"1","to":"2",)"
                          R"("amount":"5","ccy":"XYZ"})");

  // Assert
  ASSERT_FALSE(decoded.ok());
  EXPECT_EQ(decoded.error(), ErrorCode::UnknownCurrency);
}

TEST(CodecReject, IdempotencyKeyLengthCap) {
  // Arrange
  const std::string tooLong(kMaxIdemKeyLength + 1, 'k');
  const RequestEnvelope env{1, TransferReq{tooLong, 1, 2, 5, Currency::USD}};

  // Act & Assert —— 兩種編碼都必須拒絕。
  EXPECT_FALSE(kBin.decodeRequest(stripPrefix(kBin.encodeRequest(env))).ok());
  EXPECT_FALSE(kJson.decodeRequest(stripNewline(kJson.encodeRequest(env))).ok());
}

// ===========================================================================
// FrameSplitter —— 半包、黏包、資源保護
// ===========================================================================

TEST(FrameSplitterLengthPrefix, WholeFrame) {
  // Arrange
  const LengthPrefixSplitter splitter;
  const std::string wire = kBin.encodeRequest({1, sampleTransfer()});

  // Act
  const FrameView fv = splitter.next(wire);

  // Assert
  EXPECT_EQ(fv.status, FrameStatus::Ok);
  EXPECT_EQ(fv.consumed, wire.size());
  EXPECT_EQ(fv.frame.size(), wire.size() - LengthPrefixSplitter::kPrefixSize);
}

TEST(FrameSplitterLengthPrefix, PartialFrameWaits) {
  // Arrange
  const LengthPrefixSplitter splitter;
  const std::string wire = kBin.encodeRequest({1, sampleTransfer()});

  // Act & Assert —— 每一種切法都必須回 NeedMore，而且不消費任何位元組。
  // 少一個位元組就吐出半則訊息，是這一層最致命的 bug。
  for (std::size_t cut = 0; cut < wire.size(); ++cut) {
    const FrameView fv = splitter.next(std::string_view(wire).substr(0, cut));
    EXPECT_EQ(fv.status, FrameStatus::NeedMore) << "在 " << cut << " 位元組時不該切出 frame";
    EXPECT_EQ(fv.consumed, 0U);
  }
}

TEST(FrameSplitterLengthPrefix, GluedFramesAreSplitOneByOne) {
  // Arrange —— TCP 是位元組流，兩則訊息黏在同一次 read 是常態而非例外。
  const LengthPrefixSplitter splitter;
  const std::string a = kBin.encodeRequest({1, PingReq{}});
  const std::string b = kBin.encodeRequest({2, sampleTransfer()});
  std::string wire = a + b;

  std::string_view rest{wire};

  // Act & Assert —— 一則一則切出來，游標必須落在正確的位置。
  const FrameView f1 = splitter.next(rest);
  ASSERT_EQ(f1.status, FrameStatus::Ok);
  EXPECT_EQ(f1.consumed, a.size());
  EXPECT_TRUE(kBin.decodeRequest(f1.frame).ok());
  rest.remove_prefix(f1.consumed);

  const FrameView f2 = splitter.next(rest);
  ASSERT_EQ(f2.status, FrameStatus::Ok);
  EXPECT_EQ(f2.consumed, b.size());
  const auto decoded = kBin.decodeRequest(f2.frame);
  ASSERT_TRUE(decoded.ok());
  EXPECT_EQ(decoded.value().reqId, 2U);
  rest.remove_prefix(f2.consumed);

  EXPECT_TRUE(rest.empty());
  EXPECT_EQ(splitter.next(rest).status, FrameStatus::NeedMore);
}

TEST(FrameSplitterLengthPrefix, OversizedLengthIsRejectedImmediately) {
  // ★ 資源耗盡防護。
  //
  //   如果先判斷「資料夠不夠」再判斷「長度合不合理」，那麼送出
  //   [len=0xFFFFFFFF] 之後閉嘴的 client 會讓我們永遠等待，
  //   而 Buffer 會一直被要求成長。順序必須是先驗證再等待。
  // Arrange
  const LengthPrefixSplitter splitter{1024};
  const std::string wire = "\xFF\xFF\xFF\xFF";  // len = 4294967295

  // Act
  const FrameView fv = splitter.next(wire);

  // Assert
  EXPECT_EQ(fv.status, FrameStatus::Error);
  EXPECT_EQ(fv.error, ErrorCode::FrameTooLarge);
}

TEST(FrameSplitterLengthPrefix, ZeroLengthIsMalformed) {
  // Arrange
  const LengthPrefixSplitter splitter;
  const std::string wire(4, '\0');

  // Act
  const FrameView fv = splitter.next(wire);

  // Assert
  EXPECT_EQ(fv.status, FrameStatus::Error);
  EXPECT_EQ(fv.error, ErrorCode::MalformedFrame);
}

TEST(FrameSplitterNewline, SplitsLines) {
  // Arrange
  const NewlineSplitter splitter;
  std::string_view wire = "{\"a\":1}\n{\"b\":2}\n";

  // Act & Assert —— 一行一行切。
  const FrameView f1 = splitter.next(wire);
  ASSERT_EQ(f1.status, FrameStatus::Ok);
  EXPECT_EQ(f1.frame, R"({"a":1})");
  EXPECT_EQ(f1.consumed, 8U);  // 7 個字元 + 換行
  wire.remove_prefix(f1.consumed);

  const FrameView f2 = splitter.next(wire);
  ASSERT_EQ(f2.status, FrameStatus::Ok);
  EXPECT_EQ(f2.frame, R"({"b":2})");
}

TEST(FrameSplitterNewline, HandlesCrLf) {
  // Arrange —— telnet 與 Windows 的 client 會送 \r\n。不剝掉的話
  // JSON 解析會失敗，而錯誤訊息會指向 JSON 語法，讓人查錯方向。
  const NewlineSplitter splitter;

  // Act
  const FrameView fv = splitter.next("{\"a\":1}\r\n");

  // Assert
  ASSERT_EQ(fv.status, FrameStatus::Ok);
  EXPECT_EQ(fv.frame, R"({"a":1})");
  EXPECT_EQ(fv.consumed, 9U);  // \r 不在 frame 裡，但仍要被消費掉
}

TEST(FrameSplitterNewline, IncompleteLineWaits) {
  // Arrange
  const NewlineSplitter splitter;

  // Act —— 沒有換行結尾。
  const FrameView fv = splitter.next(R"({"a":1})");

  // Assert
  EXPECT_EQ(fv.status, FrameStatus::NeedMore);
  EXPECT_EQ(fv.consumed, 0U);
}

TEST(FrameSplitterNewline, UnterminatedLineIsCappedByMaxFrameSize) {
  // ★ NDJSON 沒有長度前綴，所以這個上限不是防禦性程式碼，是必要條件。
  //   一個永遠不送換行的 client 可以讓 Buffer 無限成長直到 OOM。
  // Arrange
  const NewlineSplitter splitter{64};
  const std::string wire(128, 'x');  // 沒有換行

  // Act
  const FrameView fv = splitter.next(wire);

  // Assert
  EXPECT_EQ(fv.status, FrameStatus::Error);
  EXPECT_EQ(fv.error, ErrorCode::FrameTooLarge);
}

// ===========================================================================
// 名字表 —— 雙向查找必須自洽
// ===========================================================================

TEST(ProtoNames, MsgTypeNamesRoundTrip) {
  // Act & Assert —— 走訪整張名字表，兩個方向都必須自洽。
  for (const auto& entry : kMsgTypeNames) {
    EXPECT_EQ(nameOfMsgType(entry.type), entry.name);
    const auto back = msgTypeFromName(entry.name);
    ASSERT_TRUE(back.ok()) << entry.name;
    EXPECT_EQ(back.value(), entry.type);
  }
  EXPECT_FALSE(msgTypeFromName("no_such_type").ok());
}

TEST(ProtoNames, ErrorCodeNamesRoundTrip) {
  // errorCodeFromString 走訪全部值比對 toString()，所以名字表只有一份。
  // 這條測試確保沒有兩個錯誤碼共用同一個名字（那會讓反查靜默回錯的那個）。
  // Act & Assert —— 走訪每一個錯誤碼。
  const auto count = static_cast<std::uint16_t>(ErrorCode::Count);
  for (std::uint16_t i = 0; i < count; ++i) {
    const auto code = static_cast<ErrorCode>(i);
    EXPECT_EQ(errorCodeFromString(toString(code)), code)
        << "ErrorCode " << i << " 的名字 " << toString(code) << " 不是唯一的";
  }
  EXPECT_EQ(errorCodeFromString("NO_SUCH_ERROR"), ErrorCode::Count);
}

TEST(ProtoNames, CodecTagNames) {
  // Assert only —— 一張兩列的事實表。
  EXPECT_EQ(nameOf(CodecTag::Binary), "binary");
  EXPECT_EQ(nameOf(CodecTag::Json), "json");
}

// ===========================================================================
// 兩種編碼必須表達同一件事
// ===========================================================================

TEST(CodecEquivalence, BothCodecsDecodeToIdenticalValues) {
  // ★ 這條測試是欄位描述表存在的理由本身。
  //
  //   如果有人加了欄位卻只在一種編碼裡處理，這裡就會紅。
  //   在手寫 28 個編解碼函式的版本裡，這種漏失是靜默的。

  // Arrange
  const RequestEnvelope env{42, sampleTransfer()};

  // Act —— 同一個值分別走兩種編碼再解回來。
  const auto viaBin = kBin.decodeRequest(stripPrefix(kBin.encodeRequest(env)));
  const auto viaJson = kJson.decodeRequest(stripNewline(kJson.encodeRequest(env)));

  // Assert —— 兩邊必須逐欄位相等。
  ASSERT_TRUE(viaBin.ok());
  ASSERT_TRUE(viaJson.ok());

  EXPECT_EQ(viaBin.value().reqId, viaJson.value().reqId);

  const auto& b = std::get<TransferReq>(viaBin.value().body);
  const auto& j = std::get<TransferReq>(viaJson.value().body);
  EXPECT_EQ(b.idemKey, j.idemKey);
  EXPECT_EQ(b.from, j.from);
  EXPECT_EQ(b.to, j.to);
  EXPECT_EQ(b.amount, j.amount);
  EXPECT_EQ(b.ccy, j.ccy);
}

TEST(CodecEquivalence, FieldCountMatchesDeclaration) {
  // 一個很便宜的哨兵：有人加了欄位卻沒更新 golden 表格時，
  // 這裡會先紅，錯誤訊息比「hex 字串對不上」好懂得多。

  // Assert only —— 一張欄位數量的事實表。
  EXPECT_EQ(fieldCount<PingReq>(), 0U);
  EXPECT_EQ(fieldCount<TransferReq>(), 5U);
  EXPECT_EQ(fieldCount<GetAccountReq>(), 1U);
  EXPECT_EQ(fieldCount<GetStatsReq>(), 0U);
  EXPECT_EQ(fieldCount<PongResp>(), 0U);
  EXPECT_EQ(fieldCount<TransferOkResp>(), 3U);
  EXPECT_EQ(fieldCount<AccountResp>(), 4U);
  EXPECT_EQ(fieldCount<StatsResp>(), 20U);
  EXPECT_EQ(fieldCount<ErrorResp>(), 2U);
}
