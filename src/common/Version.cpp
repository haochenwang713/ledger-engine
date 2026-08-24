#include <ledger/common/Version.h>

#include <string>

// 版本號由 CMake 在編譯期注入（見 src/CMakeLists.txt 的 target_compile_definitions）。
// 這樣專案版本只有一個真實來源：根目錄 CMakeLists.txt 的 project(VERSION ...)。
#ifndef LEDGER_VERSION_MAJOR
#define LEDGER_VERSION_MAJOR 0
#endif
#ifndef LEDGER_VERSION_MINOR
#define LEDGER_VERSION_MINOR 0
#endif
#ifndef LEDGER_VERSION_PATCH
#define LEDGER_VERSION_PATCH 0
#endif
#ifndef LEDGER_BUILD_TYPE
#define LEDGER_BUILD_TYPE "unknown"
#endif

namespace ledger {
namespace {

// Sanitizer 偵測。
// GCC 定義 __SANITIZE_THREAD__ / __SANITIZE_ADDRESS__；
// Clang 走 __has_feature。兩邊都要處理，否則在其中一個編譯器上會誤報「沒開」。
constexpr const char* sanitizerName() noexcept {
#if defined(__SANITIZE_THREAD__)
  return "TSan";
#elif defined(__SANITIZE_ADDRESS__)
  return "ASan";
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
  return "TSan";
#elif __has_feature(address_sanitizer)
  return "ASan";
#else
  return "none";
#endif
#else
  return "none";
#endif
}

constexpr const char* compilerName() noexcept {
#if defined(__clang__)
  return "clang " __clang_version__;
#elif defined(__GNUC__)
  return "gcc " __VERSION__;
#else
  return "unknown compiler";
#endif
}

}  // namespace

Version version() noexcept {
  return Version{LEDGER_VERSION_MAJOR, LEDGER_VERSION_MINOR, LEDGER_VERSION_PATCH};
}

std::string versionString() {
  const Version v = version();
  return std::to_string(v.major) + '.' + std::to_string(v.minor) + '.' + std::to_string(v.patch);
}

std::string buildInfo() {
  return "ledger_engine " + versionString() + " | " + LEDGER_BUILD_TYPE + " | " + compilerName() +
         " | sanitizer: " + sanitizerName();
}

}  // namespace ledger
