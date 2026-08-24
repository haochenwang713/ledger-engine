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

// epoll 是 Linux 專屬的 API。macOS 用的是 kqueue，Windows 用 IOCP。
// 這個專案規格指定 epoll，所以只能在 Linux 上建置與執行。
// 在編譯期就擋下來，比等到 Stage 4 才發現 <sys/epoll.h> 找不到要好得多。
#if !defined(__linux__)
#error "ledger_engine 需要 Linux（epoll 是 Linux 專屬 API）。請用 docker compose 在容器內建置：make dev"
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
  std::cout << "Stage 1 空殼啟動成功。TCP server 尚未實作（Stage 4）。\n";
  return EXIT_SUCCESS;
}
