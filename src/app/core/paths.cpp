#include "paths.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <string>
#include <string_view>
#include <vector>

#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

// Windows paths cannot be longer than this (UNICODE_STRING counts bytes in 16 bits).
constexpr size_t kMaxPathChars = 32768;

constexpr std::wstring_view kLongPrefix = L"\\\\?\\";
constexpr std::wstring_view kLongUncPrefix = L"\\\\?\\UNC\\";

// |path| without a \\?\ prefix: \\?\C:\x becomes C:\x and \\?\UNC\server\share becomes
// \\server\share. Other paths stay as they are.
std::wstring WithoutLongPrefix(std::wstring path) {
  if (IStartsWith(path, kLongUncPrefix)) return L"\\\\" + path.substr(kLongUncPrefix.size());
  if (path.starts_with(kLongPrefix)) return path.substr(kLongPrefix.size());
  return path;
}

// <known folder>\<Product>. The folder need not exist yet (a per-user install creates
// %LOCALAPPDATA%\Programs).
HRESULT KnownFolderChild(REFKNOWNFOLDERID folder, std::filesystem::path* out) noexcept try {
  out->clear();
  wil::unique_cotaskmem_string base;
  RETURN_IF_FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &base));
  *out = std::filesystem::path(base.get()) / kProductName;
  return S_OK;
}
CATCH_RETURN();

// The components of an absolute, normalized path: root name ("C:" or "\\server"), root
// directory, then each name; the empty name a trailing separator leaves is dropped.
std::vector<std::wstring> Components(const std::filesystem::path& path) {
  std::vector<std::wstring> parts;
  for (const std::filesystem::path& part : path) {
    if (!part.empty()) parts.push_back(part.native());
  }
  return parts;
}

}  // namespace

HRESULT GetMachineRoot(std::filesystem::path* out) noexcept {
  return KnownFolderChild(FOLDERID_ProgramFiles, out);
}

HRESULT GetUserRoot(std::filesystem::path* out) noexcept {
  return KnownFolderChild(FOLDERID_UserProgramFiles, out);
}

HRESULT GetDataDir(std::filesystem::path* out) noexcept {
  return KnownFolderChild(FOLDERID_LocalAppData, out);
}

HRESULT GetLogPath(std::filesystem::path* out) noexcept try {
  RETURN_IF_FAILED(GetDataDir(out));
  *out /= L"Logs";
  *out /= std::wstring(kProductName) + L".log";
  return S_OK;
}
CATCH_RETURN();

HRESULT GetOwnExe(std::filesystem::path* out) noexcept try {
  out->clear();
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    RETURN_LAST_ERROR_IF(length == 0);
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    // The name was cut off: try again with more room.
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), buffer.size() >= kMaxPathChars);
    buffer.resize(buffer.size() * 2);
  }
  *out = std::move(buffer);
  return S_OK;
}
CATCH_RETURN();

HRESULT Canonicalize(const std::filesystem::path& in, std::filesystem::path* out) noexcept try {
  out->clear();
  // Attributes only, sharing everything: works for folders (backup semantics) and for files
  // other programs have open. Without FILE_FLAG_OPEN_REPARSE_POINT, links are followed. A path
  // that names a pipe gives its server an identification token at most.
  HANDLE handle = CreateFileW(
      in.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    // A missing path is an expected answer here, not a failure worth logging.
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
  }
  wil::unique_hfile file(handle);
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetFinalPathNameByHandleW(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    RETURN_LAST_ERROR_IF(length == 0);
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    // Too small: |length| is the size needed, terminating null included.
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), length > kMaxPathChars);
    buffer.resize(length);
  }
  *out = WithoutLongPrefix(std::move(buffer));
  return S_OK;
}
CATCH_RETURN();

bool IsUnderLexically(const std::filesystem::path& child,
                      const std::filesystem::path& parent) noexcept {
  try {
    const std::filesystem::path c =
        std::filesystem::path(WithoutLongPrefix(child.native())).lexically_normal();
    const std::filesystem::path p =
        std::filesystem::path(WithoutLongPrefix(parent.native())).lexically_normal();
    if (!c.is_absolute() || !p.is_absolute()) return false;
    const std::vector<std::wstring> childParts = Components(c);
    const std::vector<std::wstring> parentParts = Components(p);
    if (childParts.size() <= parentParts.size()) return false;
    for (size_t i = 0; i < parentParts.size(); ++i) {
      if (!IEquals(childParts[i], parentParts[i])) return false;
    }
    return true;
  } catch (...) {
    return false;  // out of memory: "not under" is the safe answer
  }
}

bool IsUnder(const std::filesystem::path& child, const std::filesystem::path& parent) noexcept {
  std::filesystem::path canonicalChild;
  std::filesystem::path canonicalParent;
  if (FAILED(Canonicalize(child, &canonicalChild)) ||
      FAILED(Canonicalize(parent, &canonicalParent))) {
    return false;
  }
  return IsUnderLexically(canonicalChild, canonicalParent);
}

}  // namespace pdft
