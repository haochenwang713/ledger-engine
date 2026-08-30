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
# nlohmann/json —— JSON port 的解析器（Stage 5）
#
# 取得策略跟 GoogleTest 一致：先找系統套件，找不到才從網路抓。
# Dockerfile.dev 已經 apt install nlohmann-json3-dev，所以容器內建置
# 是零網路、秒配置。
#
# 為什麼引入第三方庫：手刻 binary 序列化是這個專案的展示點，值得自己寫；
# 手刻 JSON parser 不是 —— 它只會拖慢進度並製造 bug，而且沒有人會因為
# 「他自己寫了 JSON parser」而加分。分界線是「這段程式碼有沒有在
# 展示這個專案想展示的能力」。
# ---------------------------------------------------------------------------
find_package(nlohmann_json 3.10 QUIET)

if(nlohmann_json_FOUND)
  message(STATUS "nlohmann/json: 使用系統安裝版本")
else()
  message(STATUS "nlohmann/json: 系統找不到，改用 FetchContent 下載")
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
  )
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(nlohmann_json)
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
