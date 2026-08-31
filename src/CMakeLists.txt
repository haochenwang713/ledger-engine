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
  money/Currency.cpp
  money/Money.cpp
  core/AccountRegistry.cpp
  core/Journal.cpp
  core/LedgerCore.cpp
  net/Buffer.cpp          # 平台無關，macOS 上也能編譯與測試
  proto/Messages.cpp      # Stage 5a：協定層完全不碰 socket，macOS 也能測
  proto/FrameSplitter.cpp
  proto/BinaryCodec.cpp
  proto/JsonCodec.cpp
  proto/Session.cpp
  core/LedgerRequestHandler.cpp   # Stage 5c：協定 ↔ 帳本的翻譯層
  concurrent/ThreadPool.cpp   # Stage 5b：執行緒池，平台無關
  # Stage 6   db/PgPool.cpp  db/LedgerRepository.cpp
)

# PUBLIC：連結 ledger_core 的 target 也會拿到這個 include 路徑
target_include_directories(ledger_core PUBLIC
  ${PROJECT_SOURCE_DIR}/include
)

# ---------------------------------------------------------------------------
# 網路層只在 Linux 上編譯。
#
# epoll / eventfd / accept4 都是 Linux 專屬。macOS 用的是 kqueue，
# 沒有對應的標頭檔。與其寫一層抽象去同時支援兩種（那是另一個專案的規模），
# 不如把網路層限定在 Linux，其他層維持跨平台。
#
# 效果：在 Mac 上 make test 照樣跑得動，只是少了網路相關的測試。
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(ledger_core PRIVATE
    net/Socket.cpp
    net/EventLoop.cpp
    net/Acceptor.cpp
    net/Connection.cpp
    net/EchoServer.cpp      # Stage 4 的驗收目標；保留為網路層的回歸測試
    net/LedgerServer.cpp    # Stage 5c：協定 + 執行緒池 + 帳本的接線
  )
  target_compile_definitions(ledger_core PUBLIC LEDGER_HAS_EPOLL=1)
  message(STATUS "網路層（epoll）：啟用")
else()
  message(STATUS "網路層（epoll）：停用（非 Linux 平台）")
endif()

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
  PUBLIC  nlohmann_json::nlohmann_json   # header-only，PUBLIC 讓測試也拿得到
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
