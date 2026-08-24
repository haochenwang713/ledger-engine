// ---------------------------------------------------------------------------
// Stage 1 的測試只有一個任務：證明測試框架本身跑得起來。
//
// 它驗證的是「工具鏈」而不是「業務邏輯」——
//   ledger_core 連結得起來、標頭找得到、GoogleTest 執行得了、ctest 收得到結果。
// 真正的業務不變式測試（I1–I5）從 Stage 3 開始加進來。
// ---------------------------------------------------------------------------

#include <ledger/common/Version.h>

#include <gtest/gtest.h>

#include <string>

namespace {

TEST(Version, ReturnsConfiguredVersion) {
  const ledger::Version v = ledger::version();

  // 版本號由 CMake 從 project(VERSION 0.1.0) 注入。
  // 如果這條掛了，代表 target_compile_definitions 沒生效。
  EXPECT_EQ(v.major, 0u);
  EXPECT_EQ(v.minor, 1u);
  EXPECT_EQ(v.patch, 0u);
}

TEST(Version, StringMatchesComponents) {
  EXPECT_EQ(ledger::versionString(), "0.1.0");
}

TEST(Version, BuildInfoIsNotEmpty) {
  const std::string info = ledger::buildInfo();

  EXPECT_FALSE(info.empty());
  EXPECT_NE(info.find("ledger_engine"), std::string::npos);
  EXPECT_NE(info.find("sanitizer:"), std::string::npos);
}

}  // namespace
