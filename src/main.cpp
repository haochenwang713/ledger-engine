// ---------------------------------------------------------------------------
// ledger_engine —— 進入點。
//
// Stage 4（目前）：跑起 epoll echo server。收到什麼就原樣送回去。
//                  用途是先把網路層本身跑通、測乾淨，再談協定。
// Stage 5：       這裡會啟動 ThreadPool（20 個 worker），
//                 並把 echo 的回呼換成「切 frame → 丟進佇列」。
// Stage 6：       這裡會初始化 PgPool 並從 DB 載入帳戶狀態。
// ---------------------------------------------------------------------------

#include <ledger/common/Version.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#if defined(LEDGER_HAS_EPOLL)
#include <ledger/net/EchoServer.h>
#include <ledger/net/EventLoop.h>

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

void printUsage() {
  std::cout << "usage: ledger_engine [--port N] [--version] [--help]\n"
            << "\n"
            << "  --port N    監聽的 TCP port（預設 " << kDefaultPort << "）\n"
            << "  --version   印出版本與組建資訊後結束\n"
            << "  --help      印出這段說明\n"
            << "\n"
            << "Stage 4 是 echo server：收到什麼就原樣送回去。\n"
            << "測試方式：nc localhost " << kDefaultPort << "\n";
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

int runServer(std::uint16_t port) {
  ledger::net::EventLoop loop;
  if (!loop.valid()) {
    std::cerr << "建立 event loop 失敗（epoll_create1 或 eventfd 出錯）\n";
    return EXIT_FAILURE;
  }

  ledger::net::EchoServer server(loop, port);
  if (!server.valid()) {
    std::cerr << "監聽 port " << port << " 失敗: " << ledger::toString(server.error()) << '\n';
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

  std::cout << "echo server 監聽中：port " << server.port() << '\n'
            << "試試看：  nc localhost " << server.port() << '\n'
            << "停止：    Ctrl-C\n";

  loop.run();

  std::cout << "\n已停止。累計接受 " << server.totalConnections() << " 條連線。\n";
  g_loop = nullptr;
  return EXIT_SUCCESS;
}

#endif  // LEDGER_HAS_EPOLL

}  // namespace

int main(int argc, char* argv[]) {
  std::uint16_t port = kDefaultPort;

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

    std::cerr << "未知的參數: " << arg << "\n\n";
    printUsage();
    return EXIT_FAILURE;
  }

  std::cout << ledger::buildInfo() << '\n';

  if constexpr (!kHasEpoll) {
    std::cout << "平台：非 Linux，沒有 epoll —— 網路層未建置。\n"
              << "      核心帳本邏輯與其測試在本平台可以原生執行（make test）。\n"
              << "      要跑伺服器請進 Linux 容器：make up && make shell\n";
    return EXIT_SUCCESS;
  }

#if defined(LEDGER_HAS_EPOLL)
  return runServer(port);
#else
  return EXIT_SUCCESS;
#endif
}
