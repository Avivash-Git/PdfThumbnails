#include "explorer_settings.h"

#include <wil/result_macros.h>

namespace pdft {
namespace {

constexpr size_t kMaxBuildDigits = 10;

bool IsBuildNumber(const std::optional<std::wstring>& text) noexcept {
  if (!text || text->empty() || text->size() > kMaxBuildDigits) return false;
  for (const wchar_t c : *text) {
    if (c < L'0' || c > L'9') return false;
  }
  return true;
}

// A value that should be text: its text, or std::nullopt when missing or of another type.
HRESULT ReadText(HKEY root, const wchar_t* key, const wchar_t* name,
                 std::optional<std::wstring>* text) {
  text->reset();
  std::optional<RegistryValue> raw;
  RETURN_IF_FAILED(ReadRegistryValue(root, key, name, &raw));
  if (raw) *text = RegistryText(*raw);
  return S_OK;
}

}  // namespace

bool IsIconsOnlyValue(const std::optional<RegistryValue>& value) noexcept {
  if (!value) return false;
  const std::optional<DWORD> number = RegistryDword(*value);
  return number && *number == 1;
}

bool IsPolicyOnValue(const std::optional<RegistryValue>& value) noexcept {
  if (!value) return false;
  const std::optional<DWORD> number = RegistryDword(*value);
  return number && *number != 0;
}

HRESULT ReadExplorerSettings(const RegistryRoots& roots, ExplorerSettings* settings) noexcept {
  *settings = {};
  std::optional<RegistryValue> value;
  RETURN_IF_FAILED(ReadRegistryValue(roots.user, kExplorerAdvancedKey, kIconsOnlyValue, &value));
  settings->iconsOnly = IsIconsOnlyValue(value);
  RETURN_IF_FAILED(
      ReadRegistryValue(roots.user, kExplorerPoliciesKey, kDisableThumbnailsValue, &value));
  settings->thumbnailsOffByUserPolicy = IsPolicyOnValue(value);
  RETURN_IF_FAILED(
      ReadRegistryValue(roots.machine, kExplorerPoliciesKey, kDisableThumbnailsValue, &value));
  settings->thumbnailsOffByMachinePolicy = IsPolicyOnValue(value);
  return S_OK;
}

std::wstring FormatOsBuild(const std::optional<std::wstring>& currentBuild,
                           const std::optional<std::wstring>& currentBuildNumber,
                           std::optional<DWORD> ubr) {
  std::wstring build;
  if (IsBuildNumber(currentBuild)) {
    build = *currentBuild;
  } else if (IsBuildNumber(currentBuildNumber)) {
    build = *currentBuildNumber;
  } else {
    return {};
  }
  if (ubr) build += L"." + std::to_wstring(*ubr);
  return build;
}

HRESULT ReadOsBuild(const RegistryRoots& roots, std::wstring* build) noexcept try {
  build->clear();
  std::optional<std::wstring> currentBuild;
  std::optional<std::wstring> currentBuildNumber;
  RETURN_IF_FAILED(ReadText(roots.machine, kWindowsVersionKey, L"CurrentBuild", &currentBuild));
  RETURN_IF_FAILED(
      ReadText(roots.machine, kWindowsVersionKey, L"CurrentBuildNumber", &currentBuildNumber));
  std::optional<RegistryValue> raw;
  RETURN_IF_FAILED(ReadRegistryValue(roots.machine, kWindowsVersionKey, L"UBR", &raw));
  const std::optional<DWORD> ubr = raw ? RegistryDword(*raw) : std::nullopt;
  *build = FormatOsBuild(currentBuild, currentBuildNumber, ubr);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
