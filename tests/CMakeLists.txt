# ---------------------------------------------------------------------------
# GoogleTest 取得策略：先找系統套件，找不到才從網路抓。
#
# 為什麼是這個順序：
#   - Docker 映像已經 apt install libgtest-dev，走 find_package 是零網路、秒配置。
#   - 在沒有系統套件的環境（例如同學的機器）自動 fallback 到 FetchContent，
#     不用改任何設定就能建置。
# ---------------------------------------------------------------------------

find_package(GTest QUIET)

if(GTest_FOUND)
  message(STATUS "GoogleTest: 使用系統安裝版本")
else()
  message(STATUS "GoogleTest: 系統找不到，改用 FetchContent 下載")
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
  )
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

add_executable(ledger_tests
  test_version.cpp
  test_money.cpp
  test_ledger_core.cpp
  test_concurrency.cpp
  test_buffer.cpp        # 平台無關
  test_codec.cpp         # Stage 5a：協定層，平台無關
  test_thread_pool.cpp   # Stage 5b：佇列與執行緒池，平台無關
)

# 網路整合測試只在 Linux 上編譯 —— 它們需要真的 epoll。
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(ledger_tests PRIVATE test_echo_server.cpp)
endif()

target_link_libraries(ledger_tests
  PRIVATE ledger_core
  PRIVATE GTest::gtest_main
)

# 注意：這裡刻意不連結 ledger_warnings。
# GoogleTest 的巨集（EXPECT_EQ 等）會展開出一堆觸發 -Wconversion 的程式碼，
# 那不是我們的錯，不該讓它汙染測試建置的輸出。

# 把每個 TEST() 註冊成獨立的 CTest 測項，這樣 ctest 的輸出能指出是哪一個失敗。
#
# TIMEOUT 是併發測試的裁判，不是保險絲。
#
#   死鎖與「漏了 notify_all」這兩類 bug 的表現形式是「永遠不返回」，
#   而不是「斷言失敗」—— 程式不會報錯，它只是安靜地卡住。斷言永遠
#   等不到執行的機會，所以唯一能判定失敗的方式就是逾時。
#
#   Stage 3 的反向死鎖測試也是靠這個機制判定的。60 秒對正常執行來說
#   極度寬鬆（全部測項約 10 秒），但對卡住的情況是決定性的。
gtest_discover_tests(ledger_tests PROPERTIES TIMEOUT 60)
