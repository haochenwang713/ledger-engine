#pragma once

#include <cstdint>
#include <string>

namespace ledger {

/// 語意化版本號。專案版本的單一來源是 CMakeLists.txt 的 project(VERSION ...)，
/// 由 CMake 透過 compile definition 傳進來（見 Version.cpp）。
struct Version {
  std::uint32_t major = 0;
  std::uint32_t minor = 0;
  std::uint32_t patch = 0;
};

/// 回傳目前的組建版本。
[[nodiscard]] Version version() noexcept;

/// 回傳 "0.1.0" 形式的字串。
[[nodiscard]] std::string versionString();

/// 回傳一行組建資訊：版本、編譯器、建置型別、sanitizer 狀態。
/// 用途：啟動時印出來，避免「以為跑的是 TSan 版本其實不是」這種浪費半天的誤會。
[[nodiscard]] std::string buildInfo();

}  // namespace ledger
