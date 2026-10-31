#include <windows.h>

#include "temp_dir.h"

#include <winioctl.h>

#include <atomic>
#include <filesystem>
#include <system_error>
#include <vector>

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

bool MakeJunction(const std::wstring& link, const std::wstring& target) {
  // REPARSE_DATA_BUFFER's mount-point form (the structure is in the driver kit's headers only).
  struct MountPoint {
    DWORD tag;
    WORD dataLength;
    WORD reserved;
    WORD substituteOffset;
    WORD substituteLength;
    WORD printOffset;
    WORD printLength;
    wchar_t paths[1];
  };
  const std::wstring substitute = L"\\??\\" + target;
  std::vector<BYTE> buffer(sizeof(MountPoint) + (substitute.size() + target.size() + 2) * 2);
  auto* data = reinterpret_cast<MountPoint*>(buffer.data());
  data->tag = IO_REPARSE_TAG_MOUNT_POINT;
  data->substituteOffset = 0;
  data->substituteLength = static_cast<WORD>(substitute.size() * 2);
  data->printOffset = static_cast<WORD>(data->substituteLength + 2);
  data->printLength = static_cast<WORD>(target.size() * 2);
  wchar_t* paths = data->paths;
  substitute.copy(paths, substitute.size());
  target.copy(paths + substitute.size() + 1, target.size());
  const DWORD pathBytes = data->printOffset + data->printLength + 2u;
  data->dataLength = static_cast<WORD>(4 * sizeof(WORD) + pathBytes);
  HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD returned = 0;
  const BOOL ok =
      DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, data, 2 * sizeof(DWORD) + data->dataLength,
                      nullptr, 0, &returned, nullptr);
  CloseHandle(handle);
  return ok != FALSE;
}

}  // namespace pdft::test
