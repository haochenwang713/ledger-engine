// ---------------------------------------------------------------------------
// ledger_engine —— 進入點。
//
// Stage 1（目前）：只印出組建資訊就結束。這是刻意的「空殼」，目的是先證明
//                  編譯 → 連結 → 執行 → 測試 這條路已經打通。
// Stage 4：       這裡會建立 EventLoop、Acceptor，開始 epoll 主迴圈。
// Stage 5：       這裡會啟動 ThreadPool（20 個 worker）。
// Stage 6：       這裡會初始化 PgPool 並從 DB 載入帳戶狀態。
// ---------------------------------------------------------------------------

#include <ledger/common/Version.h>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

// epoll 是 Linux 專屬 API（macOS 用 kqueue，Windows 用 IOCP）。
//
// 但「需要 Linux」只適用於網路層（Stage 4 之後）。核心帳本邏輯
// —— Account、AccountRegistry、shared_mutex 排序取鎖 —— 是純標準 C++20，
// 在 macOS 上編譯與執行都沒問題，TSan/ASan 也能跑。
//
// 所以這裡用「編譯期常數 + 執行期檢查」，而不是 #error 把整個建置擋掉。
// 這樣在 Mac 上可以原生開發 Stage 3，只有真的要開 socket 時才需要進容器。
#if defined(__linux__)
inline constexpr bool kHasEpoll = true;
#else
inline constexpr bool kHasEpoll = false;
#endif

void printUsage() {
  std::cout << "usage: ledger_engine [--version] [--help]\n"
            << "\n"
            << "  --version   印出版本與組建資訊後結束\n"
            << "  --help      印出這段說明\n"
            << "\n"
            << "Stage 1 的執行檔是空殼，尚未開啟 socket。\n"
            << "TCP server 會在 Stage 4 實作。\n";
}

}  // namespace

int main(int argc, char* argv[]) {
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

    std::cerr << "未知的參數: " << arg << "\n\n";
    printUsage();
    return EXIT_FAILURE;
  }

  std::cout << ledger::buildInfo() << '\n';

  if constexpr (kHasEpoll) {
    std::cout << "平台：Linux，epoll 可用。\n";
  } else {
    std::cout << "平台：非 Linux，沒有 epoll —— 網路層（Stage 4 之後）需要在\n"
              << "      Linux 容器內執行：make up && make shell\n"
              << "      核心帳本邏輯與測試在本平台可以原生建置與執行。\n";
  }

  std::cout << "Stage 1 空殼啟動成功。TCP server 尚未實作（Stage 4）。\n";
  return EXIT_SUCCESS;
}
