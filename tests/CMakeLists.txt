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
  # Stage 2  test_money.cpp
  # Stage 3  test_ledger_core.cpp
  # Stage 5  test_codec.cpp
  # Stage 7  test_concurrency.cpp   <- TSan 主戰場
)

target_link_libraries(ledger_tests
  PRIVATE ledger_core
  PRIVATE GTest::gtest_main
)

# 注意：這裡刻意不連結 ledger_warnings。
# GoogleTest 的巨集（EXPECT_EQ 等）會展開出一堆觸發 -Wconversion 的程式碼，
# 那不是我們的錯，不該讓它汙染測試建置的輸出。

# 把每個 TEST() 註冊成獨立的 CTest 測項，這樣 ctest 的輸出能指出是哪一個失敗。
gtest_discover_tests(ledger_tests)
