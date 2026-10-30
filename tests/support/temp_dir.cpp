#include <windows.h>

#include "temp_dir.h"

#include <atomic>
#include <filesystem>
#include <system_error>

namespace pdft::test {
namespace {

std::atomic<unsigned> g_counter{0};

bool KeepTemp() {
  wchar_t value[4] = {};
  return GetEnvironmentVariableW(L"PDFT_KEEP_TEMP", value, 4) == 1 && value[0] == L'1';
}

}  // namespace

TempDir::TempDir(std::wstring_view prefix) {
  wchar_t temp[MAX_PATH + 1];
  const DWORD length = GetTempPathW(MAX_PATH + 1, temp);
  if (length == 0 || length > MAX_PATH) return;
  std::wstring root = temp;  // ends with a backslash
  root += L"pdft-tests";
  if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return;
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::wstring candidate = root + L"\\" + std::wstring(prefix) + L"-" +
                             std::to_wstring(GetCurrentProcessId()) + L"-" +
                             std::to_wstring(g_counter++);
    if (CreateDirectoryW(candidate.c_str(), nullptr)) {
      path_ = std::move(candidate);
      return;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) return;
  }
}

TempDir::~TempDir() {
  if (!path_.empty() && !KeepTemp()) RemoveTree(path_);
}

std::wstring TempDir::File(std::wstring_view name) const {
  return path_ + L"\\" + std::wstring(name);
}

bool RemoveTree(const std::wstring& path) {
  std::error_code error;
  std::filesystem::remove_all(std::filesystem::path(path), error);
  return !std::filesystem::exists(std::filesystem::path(path), error) && !error;
}

}  // namespace pdft::test
