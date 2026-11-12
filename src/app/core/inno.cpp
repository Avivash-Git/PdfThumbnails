#include "inno.h"

#include <wil/result_macros.h>

#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

bool IsSpace(wchar_t c) noexcept { return c == L' ' || c == L'\t'; }

}  // namespace

std::wstring InnoUninstallKey() {
  return std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + kInnoAppId +
         L"_is1";
}

HRESULT FindInnoInstall(const RegistryRoots& roots, std::optional<InnoInstall>* install) noexcept
    try {
  install->reset();
  const std::wstring key = InnoUninstallKey();
  for (const Scope scope : {Scope::kMachine, Scope::kUser}) {
    const HKEY root = RootOf(roots, scope);
    bool exists = false;
    RETURN_IF_FAILED(RegistryKeyExists(root, key, &exists));
    if (!exists) continue;
    InnoInstall found;
    found.scope = scope;
    std::optional<std::wstring> text;
    // A value of the wrong type reads as missing: the key is what counts.
    if (SUCCEEDED(ReadRegistryString(root, key, L"UninstallString", &text)) && text) {
      found.uninstallString = std::move(*text);
    }
    *install = std::move(found);
    return S_OK;
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT ParseInnoUninstallString(std::wstring_view text, std::filesystem::path* exe) {
  exe->clear();
  while (!text.empty() && IsSpace(text.front())) text.remove_prefix(1);
  while (!text.empty() && IsSpace(text.back())) text.remove_suffix(1);
  std::wstring_view path;
  if (!text.empty() && text.front() == L'"') {
    const size_t close = text.find(L'"', 1);
    if (close == std::wstring_view::npos || close + 1 != text.size()) return E_INVALIDARG;
    path = text.substr(1, close - 1);
  } else {
    if (text.find_first_of(L" \t\"") != std::wstring_view::npos) return E_INVALIDARG;
    path = text;
  }
  const bool drive = path.size() > 3 &&
                     ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'));
  // A colon after the drive's would name a stream of a file.
  if (!drive || path[1] != L':' || path[2] != L'\\' || path.find(L':', 2) != path.npos) {
    return E_INVALIDARG;
  }
  for (const wchar_t c : path) {
    if (c < L' ' || c == L'"' || c == L'/' || c == L'*' || c == L'?' || c == L'<' || c == L'>' ||
        c == L'|') {
      return E_INVALIDARG;
    }
  }
  const std::filesystem::path candidate(path);
  for (const std::filesystem::path& part : candidate.relative_path()) {
    if (part == L"." || part == L"..") return E_INVALIDARG;
  }
  if (!IEquals(candidate.extension().native(), L".exe")) return E_INVALIDARG;
  *exe = candidate;
  return S_OK;
}

bool IsInnoUninstallerFileName(std::wstring_view name) noexcept {
  constexpr std::wstring_view kStart = L"unins";
  if (name.size() != kStart.size() + 3 + 4 || !IStartsWith(name, kStart)) return false;
  for (size_t i = kStart.size(); i < kStart.size() + 3; ++i) {
    if (name[i] < L'0' || name[i] > L'9') return false;
  }
  const std::wstring_view extension = name.substr(kStart.size() + 3);
  return IEquals(extension, L".exe") || IEquals(extension, L".dat") || IEquals(extension, L".msg");
}

std::wstring InnoUninstallArguments(bool silent) { return silent ? L"/SILENT" : L""; }

}  // namespace pdft
