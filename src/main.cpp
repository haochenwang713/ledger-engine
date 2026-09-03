// ---------------------------------------------------------------------------
// ledger_engine —— 進入點。
//
// Stage 5：       跑起完整的 LedgerServer —— 兩個 port、兩組執行緒池、
//                  記憶體帳本。帳戶資料由 seedDemoAccounts() 載入。
// Step 10（目前）：get_stats —— 引擎會報告自己的即時狀態，
//                  Step 12 的網頁儀表板就是靠這個畫出來的。
// Stage 6：       seedDemoAccounts() 會被 "SELECT id, balance FROM accounts"
//                 取代，handler 工廠會為每個 worker 開一條 pqxx::connection。
// ---------------------------------------------------------------------------

#include <ledger/common/Version.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#if defined(LEDGER_HAS_EPOLL)
#include <ledger/core/AccountRegistry.h>
#include <ledger/core/Journal.h>
#include <ledger/core/LedgerCore.h>
#include <ledger/core/LedgerRequestHandler.h>
#include <ledger/net/EventLoop.h>
#include <ledger/net/LedgerServer.h>

#include <csignal>
#endif

namespace {

// epoll 是 Linux 專屬 API（macOS 用 kqueue，Windows 用 IOCP）。
//
// 但「需要 Linux」只適用於網路層。核心帳本邏輯 —— Account、
// AccountRegistry、shared_mutex 排序取鎖 —— 是純標準 C++20，
// 在 macOS 上編譯、執行、跑 sanitizer 都沒問題。
//
// 所以這裡用編譯期常數而不是 #error 把整個建置擋掉：
// 在 Mac 上可以原生開發 Stage 1–3，只有要開 socket 時才需要進 Linux。
#if defined(LEDGER_HAS_EPOLL)
inline constexpr bool kHasEpoll = true;
#else
inline constexpr bool kHasEpoll = false;
#endif

constexpr std::uint16_t kDefaultPort = 9000;
constexpr std::uint16_t kDefaultJsonPort = 9001;

void printUsage() {
  std::cout << "usage: ledger_engine [--port N] [--json-port N] [--version] [--help]\n"
            << "\n"
            << "  --port N       binary 協定的 TCP port（預設 " << kDefaultPort << "）\n"
            << "  --json-port N  NDJSON 協定的 TCP port（預設 " << kDefaultJsonPort << "）\n"
            << "  --version      印出版本與組建資訊後結束\n"
            << "  --help         印出這段說明\n"
            << "\n"
            << "手動測試（JSON port 就是為此存在）：\n"
            << "  nc localhost " << kDefaultJsonPort << "\n"
            << R"(  {"id":"1","type":"ping"})"
            << "\n"
            << R"(  {"id":"2","type":"get_account","account_id":"1001"})"
            << "\n"
            << R"(  {"id":"3","type":"get_stats"})"
            << "\n";
}

#if defined(LEDGER_HAS_EPOLL)

ledger::net::EventLoop* g_loop = nullptr;

void handleSignal(int) {
  // ⚠ 訊號處理常式裡能做的事極少 —— 只能呼叫 async-signal-safe 的函式。
  //   printf、malloc、lock 都不行（可能正好中斷在它們自己的臨界區裡）。
  //
  //   EventLoop::stop() 只做兩件事：一個 atomic store 和一次 write()
  //   到 eventfd。兩者都是 async-signal-safe 的，所以這裡呼叫它是安全的。
  //   這也正是 eventfd 喚醒機制的另一個好處。
  if (g_loop != nullptr) {
    g_loop->stop();
  }
}

int runServer(std::uint16_t binaryPort, std::uint16_t jsonPort) {
  ledger::net::EventLoop loop;
  if (!loop.valid()) {
    std::cerr << "建立 event loop 失敗（epoll_create1 或 eventfd 出錯）\n";
    return EXIT_FAILURE;
  }

  // 帳本的三個部件。它們的生命週期必須比 LedgerServer 長 ——
  // worker 執行緒會一直持有它們的參照，直到 pool 關閉為止。
  // 宣告在這裡（LedgerServer 之前）就保證了正確的銷毀順序。
  ledger::AccountRegistry registry;
  ledger::Journal journal;
  ledger::LedgerCore core(registry, journal);

  if (const ledger::Status seeded = ledger::seedDemoAccounts(core, registry); !seeded) {
    std::cerr << "載入示範帳戶失敗: " << ledger::toString(seeded.error()) << '\n';
    return EXIT_FAILURE;
  }

  ledger::net::LedgerServer::Options options;
  options.binaryPort = binaryPort;
  options.jsonPort = jsonPort;

  ledger::net::LedgerServer server(loop, core, registry, options);
  if (!server.valid()) {
    std::cerr << "監聽失敗: " << ledger::toString(server.error()) << '\n';
    return EXIT_FAILURE;
  }
  if (!server.start()) {
    std::cerr << "啟動伺服器失敗\n";
    return EXIT_FAILURE;
  }

  g_loop = &loop;
  std::signal(SIGINT, handleSignal);
  std::signal(SIGTERM, handleSignal);

  // SIGPIPE：對已被對端關閉的 socket 寫入時，預設行為是直接殺掉行程。
  // 對伺服器來說完全不能接受 —— 一個 client 斷線就會整台掛掉。
  // 忽略它之後，write() 會改成回 -1 並把 errno 設成 EPIPE，
  // 由我們的錯誤處理路徑正常關閉那條連線。
  std::signal(SIGPIPE, SIG_IGN);

  std::cout << "ledger 引擎啟動\n"
            << "  binary  port " << server.binaryPort() << "（長度前綴）\n"
            << "  json    port " << server.jsonPort() << "（NDJSON，一行一個物件）\n"
            << "  workers " << options.binaryWorkers << " binary / " << options.jsonWorkers
            << " json（各自獨立的佇列）\n"
            << "  帳戶    " << registry.size() << " 個已載入\n"
            << "\n"
            << "試試看：\n"
            << "  nc localhost " << server.jsonPort() << "\n"
            << R"(  {"id":"1","type":"get_account","account_id":"1001"})"
            << "\n"
            << R"(  {"id":"2","type":"transfer","idem_key":"k1","from":"1001","to":"2002",)"
            << R"("amount":"2500","ccy":"USD"})"
            << "\n"
            << R"(  {"id":"3","type":"get_stats"})"
            << "\n"
            << "\n"
            << "停止：Ctrl-C\n";

  loop.run();

  // ⚠ 順序很重要：先讓 event loop 停下來，再關執行緒池。
  //
  //   反過來的話，pool 已經關閉但 loop 還在收請求，
  //   每一筆新進來的都會拿到 SERVER_BUSY —— client 看到的是
  //   「伺服器還在，但什麼都不做」，比乾脆拒絕連線更難診斷。
  server.shutdown();

  const auto& binary = server.binaryPool();
  const auto& json = server.jsonPool();
  std::cout << "\n已停止。\n"
            << "  連線     " << server.totalConnections() << " 條（累計）\n"
            << "  binary   " << binary.completed() << " 筆完成，" << binary.rejected()
            << " 筆因佇列滿被拒，" << binary.droppedNoSink() << " 筆因斷線丟棄\n"
            << "  json     " << json.completed() << " 筆完成，" << json.rejected()
            << " 筆因佇列滿被拒，" << json.droppedNoSink() << " 筆因斷線丟棄\n"
            << "  轉帳     " << core.transferCount() << " 成功 / " << core.rejectedCount()
            << " 拒絕\n"
            << "  不變式   " << (core.verifyInvariants() ? "通過 —— 帳本是平的" : "★ 失敗 ★")
            << '\n';

  g_loop = nullptr;
  return EXIT_SUCCESS;
}

#endif  // LEDGER_HAS_EPOLL

}  // namespace

int main(int argc, char* argv[]) {
  std::uint16_t port = kDefaultPort;
  std::uint16_t jsonPort = kDefaultJsonPort;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};

    if (arg == "--version") {
      std::cout << ledger::buildInfo() << '\n';
      return EXIT_SUCCESS;
    }
    if (arg == "--help" || arg == "-h") {
      printUsage();
      return EXIT_SUCCESS;
    }
    if (arg == "--port" && i + 1 < argc) {
      port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
      continue;
    }
    if (arg == "--json-port" && i + 1 < argc) {
      jsonPort = static_cast<std::uint16_t>(std::atoi(argv[++i]));
      continue;
    }

    std::cerr << "未知的參數: " << arg << "\n\n";
    printUsage();
    return EXIT_FAILURE;
  }

  std::cout << ledger::buildInfo() << '\n';

  if constexpr (!kHasEpoll) {
    // ⚠ 這裡印出 port 不只是為了訊息友善，也讓這個變數在非 Linux 平台
    //   上真的「被使用」。
    //
    //   在 macOS 上 LEDGER_HAS_EPOLL 是關的，runServer(port) 不會被編譯，
    //   於是 port 變成「有寫入、沒讀取」—— AppleClang 的
    //   -Wunused-but-set-variable 會擋下整個建置（配上 -Werror）。
    //
    //   Linux 的 CI 永遠碰不到這個路徑，所以這類問題只有在真的
    //   跨平台建置時才會冒出來。用 [[maybe_unused]] 也能消警告，
    //   但那是把症狀藏起來；把它印出來則是讓使用者知道
    //   「你給的參數我收到了，只是這個平台用不到」。
    std::cout << "平台：非 Linux，沒有 epoll —— 網路層未建置。\n"
              << "      （--port " << port << " / --json-port " << jsonPort
              << " 已解析，但這個平台不會啟動伺服器）\n"
              << "      核心帳本邏輯與其測試在本平台可以原生執行（make test）。\n"
              << "      要跑伺服器請進 Linux 容器：make up && make shell\n";
    return EXIT_SUCCESS;
  }

#if defined(LEDGER_HAS_EPOLL)
  return runServer(port, jsonPort);
#else
  return EXIT_SUCCESS;
#endif
}
