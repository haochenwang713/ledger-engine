# ---------------------------------------------------------------------------
# ledger_core —— 引擎的靜態函式庫。
#
# 為什麼要拆成 library 而不是全部塞進執行檔：
#   測試需要連結同一份程式碼。拆成 library 之後，ledger_engine（執行檔）與
#   ledger_tests（測試）都連結它，保證「測到的」跟「跑到的」是同一份編譯結果。
#
# Stage 1 只放一個 Version 模組，用途是證明「編譯 → 連結 → 執行 → 測試」
# 這條路已經打通。之後每個 Stage 往這裡加原始檔即可。
# ---------------------------------------------------------------------------

add_library(ledger_core STATIC
  common/Version.cpp
  # Stage 2  money/Currency.cpp  money/Money.cpp
  # Stage 3  core/Account.cpp  core/AccountRegistry.cpp  core/LedgerCore.cpp
  # Stage 4  net/EventLoop.cpp  net/Acceptor.cpp  net/Connection.cpp  net/Buffer.cpp
  # Stage 5  proto/Codec.cpp  concurrent/ThreadPool.cpp
  # Stage 6  db/PgPool.cpp  db/LedgerRepository.cpp
)

# PUBLIC：連結 ledger_core 的 target 也會拿到這個 include 路徑
target_include_directories(ledger_core PUBLIC
  ${PROJECT_SOURCE_DIR}/include
)

# 版本號的單一真實來源是根目錄 project(VERSION ...)，這裡把它注入編譯期。
target_compile_definitions(ledger_core PRIVATE
  LEDGER_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  LEDGER_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  LEDGER_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  LEDGER_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
)

# 執行緒支援。POSIX 平台上會帶進 -pthread。
find_package(Threads REQUIRED)

target_link_libraries(ledger_core
  PRIVATE ledger_warnings      # 警告只套用在編譯 ledger_core 自己的原始檔
  PUBLIC  ledger_sanitizers    # sanitizer 必須全程式一致，所以是 PUBLIC
  PUBLIC  Threads::Threads
)

# ---------------------------------------------------------------------------
# ledger_engine —— 伺服器執行檔。
# Stage 1 階段它只印出組建資訊就結束（空殼），Stage 4 之後才真的開 socket。
# ---------------------------------------------------------------------------
add_executable(ledger_engine main.cpp)

target_link_libraries(ledger_engine
  PRIVATE ledger_core
  PRIVATE ledger_warnings
)
