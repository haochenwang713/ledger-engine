#include <ledger/core/LedgerRequestHandler.h>
#include <ledger/net/LedgerServer.h>

#include <utility>

namespace ledger::net {

// ===========================================================================
// ConnectionContext
// ===========================================================================

void ConnectionContext::sendResponse(const proto::ResponseEnvelope& resp) {
  const ConnectionPtr conn = conn_.lock();
  if (!conn) {
    return;  // client 已經斷線，沒有人在等這個回應了
  }
  // encodeResponse() 的輸出含框架開銷（長度前綴或結尾換行），可以直接送。
  //
  // send() 可以從任何執行緒呼叫：它只把位元組放進 outputBuffer_，
  // 真正的 write() 由 IO 執行緒透過 runInLoop() 執行。
  // ★ worker 絕不能自己 write(fd) —— 兩個 worker 同時寫同一個 socket，
  //   位元組會交織、長度前綴對不上、協定崩潰。而那不是 data race，
  //   TSan 永遠抓不到，只能靠這條架構規則排除。
  conn->send(codec_.encodeResponse(resp));
}

void ConnectionContext::deliver(const proto::ResponseEnvelope& resp, proto::CodecTag /*codec*/) {
  // 這裡在 worker 執行緒上。codec 參數目前用不到 —— 每條連線的編碼在
  // 它連上哪個 port 時就固定了，所以直接用 codec_。
  // 保留這個參數是為了 Stage 6 之後可能出現的「同一個 pool 服務多種編碼」。
  sendResponse(resp);
}

void ConnectionContext::onMessage(Buffer& buffer) {
  // 這裡在 IO 執行緒上。整個函式必須是非阻塞的 ——
  // 它一旦卡住，所有連線一起停擺。
  const proto::Session::Outcome outcome = session_.drain(buffer.view());

  // ⚠ 順序：先消費緩衝，再處理結果。
  //
  //   outcome 裡的 RequestEnvelope 是值型別（解碼時已經複製過），
  //   所以它們不依賴 buffer 的內容。先 retrieve 可以確保即使下面
  //   任何一步提早返回，緩衝也不會殘留已經處理過的位元組 ——
  //   殘留的話下一次讀取事件會把同一則訊息再處理一次。
  buffer.retrieve(outcome.consumed);

  // 解碼失敗的錯誤直接回，不經過 worker。這類請求根本沒有進到帳本，
  // 讓它們去排佇列只是浪費一個名額。
  for (const proto::ResponseEnvelope& err : outcome.errors) {
    sendResponse(err);
  }

  for (const proto::RequestEnvelope& req : outcome.requests) {
    const std::uint32_t reqId = req.reqId;

    concurrent::Task task{weak_from_this(), req, codec_.tag()};

    // ★ 背壓：佇列滿了就當場拒絕，絕不等待。
    //
    //   這是整個伺服器最重要的一條紀律。IO 執行緒在這裡阻塞的話，
    //   event loop 停擺，所有連線一起死 —— 不只是送出這筆請求的那一條。
    //   ThreadPool::submit() 內部只用 tryPush，所以它保證不會阻塞。
    if (!pool_.submit(std::move(task))) {
      sendResponse(proto::makeError(reqId, ErrorCode::ServerBusy, "queue full, retry later"));
    }
  }

  if (outcome.fatal) {
    // 框架失敗：位元組流已經無法對齊，找不到下一則訊息的開頭。
    // 上面已經送出說明用的錯誤回應，現在關閉連線。
    if (const ConnectionPtr conn = conn_.lock()) {
      conn->close();
    }
  }
}

// ===========================================================================
// LedgerServer
// ===========================================================================

LedgerServer::LedgerServer(EventLoop& loop,
                           LedgerCore& core,
                           AccountRegistry& registry,
                           Options options)
    : loop_(loop),
      binaryAcceptor_(loop, options.binaryPort),
      jsonAcceptor_(loop, options.jsonPort),
      newlineSplitter_(proto::kMaxFrameSize),
      binaryPool_("binary",
                  options.binaryWorkers,
                  options.binaryQueueCapacity,
                  makeLedgerHandlerFactory(core, registry)),
      jsonPool_("json",
                options.jsonWorkers,
                options.jsonQueueCapacity,
                makeLedgerHandlerFactory(core, registry)) {
  binaryAcceptor_.setNewConnectionCallback(
      [this](int fd, std::string peer) { onNewConnection(fd, std::move(peer), Protocol::Binary); });
  jsonAcceptor_.setNewConnectionCallback(
      [this](int fd, std::string peer) { onNewConnection(fd, std::move(peer), Protocol::Json); });
}

LedgerServer::~LedgerServer() {
  shutdown();
}

Status LedgerServer::start() {
  if (const Status s = binaryAcceptor_.start(); !s) {
    return s;
  }
  return jsonAcceptor_.start();
}

void LedgerServer::shutdown() {
  // 優雅關機：拒收新工作，但把佇列裡已經收下的請求處理完 ——
  // 那些 client 還在等回應。
  //
  // ⚠ 必須在 EventLoop::run() 返回之後才呼叫。
  //   loop 還在跑的時候關掉 pool，新進來的請求會全部拿到 SERVER_BUSY，
  //   而 client 看到的是「伺服器還在，但什麼都不做」。
  binaryPool_.shutdown();
  jsonPool_.shutdown();
}

void LedgerServer::onNewConnection(int fd, std::string peer, Protocol protocol) {
  auto conn = std::make_shared<Connection>(loop_, fd, std::move(peer));

  // 連線用哪一組切包器、codec、執行緒池，由它連上哪個 port 決定，
  // 之後不再改變。這是兩個 port 能有完全不同行為卻共用同一套
  // 網路層與帳本核心的關鍵。
  const proto::FrameSplitter& splitter =
      protocol == Protocol::Binary ? static_cast<const proto::FrameSplitter&>(lengthSplitter_)
                                   : static_cast<const proto::FrameSplitter&>(newlineSplitter_);
  const proto::Codec& codec = protocol == Protocol::Binary
                                  ? static_cast<const proto::Codec&>(binaryCodec_)
                                  : static_cast<const proto::Codec&>(jsonCodec_);
  concurrent::ThreadPool& pool = protocol == Protocol::Binary ? binaryPool_ : jsonPool_;

  auto context = std::make_shared<ConnectionContext>(conn, splitter, codec, pool);

  // ★ 回呼捕捉 context 的 shared_ptr，這就是 context 活著的原因。
  //   Connection 被銷毀 → 回呼被銷毀 → context 被銷毀
  //   → worker 手上的 weak_ptr 失效 → 結果被丟棄（W2）。
  conn->setMessageCallback(
      [context](const ConnectionPtr& /*c*/, Buffer& buf) { context->onMessage(buf); });
  conn->setCloseCallback([this](const ConnectionPtr& c) { onClose(c); });

  // 先存進表格再 start()，理由同 Stage 4 的 EchoServer：
  // start() 之後到存進表格之前若立刻有事件進來，
  // 這個 shared_ptr 的唯一持有者還是區域變數，邏輯上很難推理。
  connections_[fd] = conn;
  activeCount_.store(connections_.size(), std::memory_order_relaxed);

  if (!conn->start()) {
    connections_.erase(fd);
    activeCount_.store(connections_.size(), std::memory_order_relaxed);
  }
}

void LedgerServer::onClose(const ConnectionPtr& conn) {
  connections_.erase(conn->fd());
  activeCount_.store(connections_.size(), std::memory_order_relaxed);
}

}  // namespace ledger::net
