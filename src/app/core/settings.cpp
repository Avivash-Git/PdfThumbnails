#include "settings.h"

#include <shlobj.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <iterator>
#include <string_view>
#include <vector>

#include "project_config.h"
#include "strings.h"
#include "zoom.h"

namespace pdft {
namespace {

// The 1.x settings file: [Thumbnails] Zoom=<percent>.
constexpr wchar_t kIniSection[] = L"Thumbnails";
constexpr wchar_t kIniZoomKey[] = L"Zoom";

// Values under the State key.
constexpr wchar_t kLastCheckTime[] = L"LastCheckTime";
constexpr wchar_t kLastCheckResult[] = L"LastCheckResult";
constexpr wchar_t kLastCheckSummary[] = L"LastCheckSummary";
constexpr wchar_t kLastPdfCraftVersion[] = L"LastPdfCraftVersion";
constexpr wchar_t kLastLiveTest[] = L"LastLiveTest";
constexpr wchar_t kMigratedFromV1[] = L"MigratedFromV1";

// Room for the key names of the INI's section; GetPrivateProfileStringW cuts the list off
// beyond it, which only matters for a file with thousands of keys.
constexpr DWORD kIniKeyListChars = 32 * 1024;

// A string value of the State key that may be missing or of the wrong type: both read as "".
HRESULT ReadStateString(HKEY root, const std::wstring& path, const wchar_t* name,
                        std::wstring* text) {
  text->clear();
  std::optional<RegistryValue> raw;
  RETURN_IF_FAILED(ReadRegistryValue(root, path, name, &raw));
  if (!raw) return S_OK;
  if (std::optional<std::wstring> value = RegistryText(*raw)) *text = std::move(*value);
  return S_OK;
}

HRESULT ReadStateDword(HKEY root, const std::wstring& path, const wchar_t* name,
                       std::optional<DWORD>* value) {
  value->reset();
  std::optional<RegistryValue> raw;
  RETURN_IF_FAILED(ReadRegistryValue(root, path, name, &raw));
  if (raw) *value = RegistryDword(*raw);
  return S_OK;
}

}  // namespace

ZoomDecision DecideZoom(const std::optional<RegistryValue>& registry,
                        std::optional<int> legacyIniZoom) noexcept {
  ZoomDecision decision;
  if (registry) {
    if (const std::optional<DWORD> value = RegistryDword(*registry)) {
      // The handler clamps whatever DWORD it finds, and uses it.
      decision.zoom = ClampZoom(*value);
      decision.origin = ZoomOrigin::kRegistry;
      decision.registry =
          *value >= static_cast<DWORD>(kMinZoom) && *value <= static_cast<DWORD>(kMaxZoom)
              ? ZoomValueState::kValid
              : ZoomValueState::kOutOfRange;
      return decision;
    }
    // Any other type: the handler skips the value as if it were missing.
    decision.registry = ZoomValueState::kWrongType;
  }
  if (legacyIniZoom) {
    decision.zoom = ClampZoom(*legacyIniZoom);
    decision.origin = ZoomOrigin::kLegacyIni;
  } else {
    decision.zoom = kMinZoom;
    decision.origin = ZoomOrigin::kDefault;
  }
  return decision;
}

HRESULT LegacyIniPath(std::filesystem::path* ini) noexcept try {
  ini->clear();
  // The handler's own steps, limits included (ReadLegacyZoomPercent in the handler).
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(kLegacySettingsEnv, buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) {
    *ini = std::wstring(buffer, length);
    return S_OK;
  }
  wil::unique_cotaskmem_string folder;
  RETURN_IF_FAILED(SHGetKnownFolderPath(kFolderIdLocalAppData, 0, nullptr, &folder));
  std::wstring path = std::wstring(folder.get()) + kLegacySettingsIni;
  // The handler gives up on a path that does not fit MAX_PATH and uses 100: no file at all.
  if (path.size() >= MAX_PATH) return S_OK;
  *ini = std::move(path);
  return S_OK;
}
CATCH_RETURN();

HRESULT ReadLegacyIniZoom(const std::filesystem::path& ini, std::optional<int>* zoom) noexcept try {
  zoom->reset();
  if (ini.empty()) return S_OK;
  const DWORD attributes = GetFileAttributesW(ini.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return S_OK;
  }
  // Is there a Zoom key at all? The section's key names, each ending in a null character.
  std::vector<wchar_t> keys(kIniKeyListChars, L'\0');
  const DWORD length = GetPrivateProfileStringW(kIniSection, nullptr, L"", keys.data(),
                                                kIniKeyListChars, ini.c_str());
  bool found = false;
  for (size_t at = 0; at < length && !found;) {
    const std::wstring_view key(keys.data() + at);
    found = IEquals(key, kIniZoomKey);
    at += key.size() + 1;
  }
  if (!found) return S_OK;
  // Exactly the handler's read: GetPrivateProfileIntW with 100 as the default, then clamped as
  // an unsigned number (so "Zoom=-5" gives what the handler gives).
  const UINT value = GetPrivateProfileIntW(kIniSection, kIniZoomKey, kMinZoom, ini.c_str());
  *zoom = ClampZoom(value);
  return S_OK;
}
CATCH_RETURN();

HRESULT LoadSettingsWithIni(const RegistryRoots& roots, const std::filesystem::path& legacyIni,
                            Settings* settings, ZoomDetails* details) noexcept try {
  *settings = {};
  if (details) *details = {};
  RETURN_HR_IF(E_INVALIDARG, roots.user == nullptr);
  std::optional<RegistryValue> zoomValue;
  RETURN_IF_FAILED(ReadRegistryValue(roots.user, kRegKey, kZoomValueName, &zoomValue));
  std::optional<int> iniZoom;
  RETURN_IF_FAILED(ReadLegacyIniZoom(legacyIni, &iniZoom));
  const ZoomDecision decision = DecideZoom(zoomValue, iniZoom);
  settings->zoom = decision.zoom;

  std::optional<RegistryValue> confirm;
  RETURN_IF_FAILED(ReadRegistryValue(roots.user, kRegKey, kConfirmFullRefreshValueName, &confirm));
  if (confirm) {
    // Anything but a DWORD 0 keeps the safe default: ask first.
    const std::optional<DWORD> value = RegistryDword(*confirm);
    settings->confirmFullRefresh = !value || *value != 0;
  }

  if (details) {
    details->decision = decision;
    if (zoomValue) {
      details->registryType = zoomValue->type;
      details->registryRaw = RegistryDword(*zoomValue);
    }
    details->legacyIni = legacyIni;
    details->legacyIniZoom = iniZoom;
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT LoadSettings(const RegistryRoots& roots, Settings* settings, ZoomDetails* details) noexcept
    try {
  std::filesystem::path ini;
  RETURN_IF_FAILED(LegacyIniPath(&ini));
  return LoadSettingsWithIni(roots, ini, settings, details);
}
CATCH_RETURN();

HRESULT SaveZoom(const RegistryRoots& roots, int zoom) noexcept {
  RETURN_HR_IF(E_INVALIDARG, roots.user == nullptr);
  return WriteRegistryDword(roots.user, kRegKey, kZoomValueName,
                            static_cast<DWORD>(ClampZoom(zoom)));
}

HRESULT SaveConfirmFullRefresh(const RegistryRoots& roots, bool confirm) noexcept {
  RETURN_HR_IF(E_INVALIDARG, roots.user == nullptr);
  return WriteRegistryDword(roots.user, kRegKey, kConfirmFullRefreshValueName, confirm ? 1 : 0);
}

std::wstring StateKeyPath() { return std::wstring(kRegKey) + L"\\State"; }

HRESULT RecordCheck(const RegistryRoots& roots, const CheckRecord& record) noexcept try {
  RETURN_HR_IF(E_INVALIDARG, roots.user == nullptr);
  const std::wstring path = StateKeyPath();
  RETURN_IF_FAILED(
      WriteRegistryDword(roots.user, path, kLastCheckResult, static_cast<DWORD>(record.result)));
  RETURN_IF_FAILED(WriteRegistryString(roots.user, path, kLastCheckSummary, record.summary));
  RETURN_IF_FAILED(
      WriteRegistryString(roots.user, path, kLastPdfCraftVersion, record.pdfcraftVersion));
  if (record.liveTest) {
    RETURN_IF_FAILED(
        WriteRegistryDword(roots.user, path, kLastLiveTest, static_cast<DWORD>(*record.liveTest)));
  } else {
    RETURN_IF_FAILED(DeleteRegistryValue(roots.user, path, kLastLiveTest));
  }
  ULARGE_INTEGER time = {};
  time.LowPart = record.time.dwLowDateTime;
  time.HighPart = record.time.dwHighDateTime;
  RETURN_IF_FAILED(WriteRegistryQword(roots.user, path, kLastCheckTime, time.QuadPart));
  return S_OK;
}
CATCH_RETURN();

HRESULT ReadCheck(const RegistryRoots& roots, std::optional<CheckRecord>* record) noexcept try {
  record->reset();
  RETURN_HR_IF(E_INVALIDARG, roots.user == nullptr);
  const std::wstring path = StateKeyPath();
  std::optional<RegistryValue> raw;
  RETURN_IF_FAILED(ReadRegistryValue(roots.user, path, kLastCheckTime, &raw));
  const std::optional<ULONGLONG> time = raw ? RegistryQword(*raw) : std::nullopt;
  if (!time) return S_OK;
  std::optional<DWORD> result;
  RETURN_IF_FAILED(ReadStateDword(roots.user, path, kLastCheckResult, &result));
  if (!result || *result >= std::size(kExitCodes)) return S_OK;

  CheckRecord check;
  ULARGE_INTEGER split = {};
  split.QuadPart = *time;
  check.time.dwLowDateTime = split.LowPart;
  check.time.dwHighDateTime = split.HighPart;
  check.result = static_cast<ExitCode>(*result);
  RETURN_IF_FAILED(ReadStateString(roots.user, path, kLastCheckSummary, &check.summary));
  RETURN_IF_FAILED(ReadStateString(roots.user, path, kLastPdfCraftVersion, &check.pdfcraftVersion));
  std::optional<DWORD> liveTest;
  RETURN_IF_FAILED(ReadStateDword(roots.user, path, kLastLiveTest, &liveTest));
  if (liveTest) check.liveTest = static_cast<HRESULT>(*liveTest);
  *record = std::move(check);
  return S_OK;
}
CATCH_RETURN();

HRESULT ReadMigratedFromV1(const RegistryRoots& roots, bool* migrated) noexcept try {
  *migrated = false;
  RETURN_HR_IF(E_INVALIDARG, roots.user == nullptr);
  std::optional<DWORD> value;
  RETURN_IF_FAILED(ReadStateDword(roots.user, StateKeyPath(), kMigratedFromV1, &value));
  *migrated = value.value_or(0) != 0;
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
