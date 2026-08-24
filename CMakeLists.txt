cmake_minimum_required(VERSION 3.22)

project(ledger_engine
        VERSION 0.1.0
        DESCRIPTION "Transactional Ledger & Settlement Engine"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Language / global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 產生 compile_commands.json 給 clangd / VSCode 用
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(LEDGER_BUILD_TESTS  "Build unit tests (GoogleTest)"          ON)
option(LEDGER_ENABLE_TSAN  "Build with ThreadSanitizer"             OFF)
option(LEDGER_ENABLE_ASAN  "Build with AddressSanitizer + UBSan"    OFF)
option(LEDGER_WERROR       "Treat warnings as errors"               OFF)

if(LEDGER_ENABLE_TSAN AND LEDGER_ENABLE_ASAN)
  message(FATAL_ERROR
          "TSan 與 ASan 不能同時開啟 —— 它們的 runtime 會互相衝突。"
          "請分兩次 build：-DLEDGER_ENABLE_TSAN=ON 或 -DLEDGER_ENABLE_ASAN=ON。")
endif()

# ---------------------------------------------------------------------------
# Shared INTERFACE targets
#   ledger_warnings   —— 編譯警告設定（只給我們自己的程式碼，不汙染第三方）
#   ledger_sanitizers —— sanitizer 旗標（編譯與連結都要，所以拆成 INTERFACE target）
# ---------------------------------------------------------------------------
add_library(ledger_warnings INTERFACE)
target_compile_options(ledger_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow            # 區域變數遮蔽外層變數 —— 併發程式碼最容易出錯的地方之一
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Wcast-align
  -Wunused
  -Wconversion        # 金額都是 int64，型別窄化必須被抓出來
  -Wsign-conversion
  -Wdouble-promotion  # 記帳系統不該出現浮點數，這條當守門員
)
if(LEDGER_WERROR)
  target_compile_options(ledger_warnings INTERFACE -Werror)
endif()

add_library(ledger_sanitizers INTERFACE)
if(LEDGER_ENABLE_TSAN)
  message(STATUS "ThreadSanitizer: ENABLED")
  target_compile_options(ledger_sanitizers INTERFACE
                         -fsanitize=thread -g -O1 -fno-omit-frame-pointer)
  target_link_options(ledger_sanitizers INTERFACE -fsanitize=thread)
endif()
if(LEDGER_ENABLE_ASAN)
  message(STATUS "AddressSanitizer + UBSan: ENABLED")
  target_compile_options(ledger_sanitizers INTERFACE
                         -fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer)
  target_link_options(ledger_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(LEDGER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Build summary —— 每次 configure 都印出來，避免「以為開了 sanitizer 其實沒開」
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "==== ledger_engine ${PROJECT_VERSION} ====")
message(STATUS "  Build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler     : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests        : ${LEDGER_BUILD_TESTS}")
message(STATUS "  TSan / ASan  : ${LEDGER_ENABLE_TSAN} / ${LEDGER_ENABLE_ASAN}")
message(STATUS "=====================================")
message(STATUS "")
