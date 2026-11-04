// The user's settings and the record of the last check, under HKCU\<product key> (through
// RegistryRoots, so tests use a private key):
//
//   Zoom                 REG_DWORD  thumbnail zoom in percent; the handler reads it too
//   ConfirmFullRefresh   REG_DWORD  0 = the settings window rebuilds the thumbnail cache without
//                                   asking (its "Don't ask again"; the command line uses --yes)
//                                   (default 1)
//   State\LastCheckTime        REG_QWORD  FILETIME (UTC) of the last recorded check
//        \LastCheckResult      REG_DWORD  its exit code
//        \LastCheckSummary     REG_SZ     one line saying what it found or did
//        \LastPdfCraftVersion  REG_SZ     the PdfCraft version it saw ("" when none)
//        \LastLiveTest         REG_DWORD  the Explorer test's HRESULT (absent when not run)
//        \MigratedFromV1       REG_DWORD  1 once the 1.x clean-up ran (written by it, read here)
//
// The zoom is decided exactly as the handler decides it, so the app never shows a zoom the
// thumbnails do not use: a REG_DWORD Zoom value (clamped), else the 1.x settings file, else 100.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

#include "exit_code.h"
#include "registry_roots.h"

namespace pdft {

inline constexpr wchar_t kConfirmFullRefreshValueName[] = L"ConfirmFullRefresh";

struct Settings {
  int zoom = 100;
  bool confirmFullRefresh = true;
};

// Where the zoom in effect comes from.
enum class ZoomOrigin {
  kRegistry,   // HKCU\<product key>\Zoom
  kLegacyIni,  // the 1.x settings.ini
  kDefault,    // neither: 100
};

// What the Zoom value in the registry holds.
enum class ZoomValueState {
  kMissing,     // no value
  kValid,       // a REG_DWORD from 100 to 200
  kOutOfRange,  // a REG_DWORD outside 100..200: the handler clamps it
  kWrongType,   // not a 4-byte REG_DWORD: the handler ignores it and falls back to the INI
};

// The value needs rewriting (the check's "zoom invalid" finding).
constexpr bool IsZoomValueInvalid(ZoomValueState state) noexcept {
  return state == ZoomValueState::kOutOfRange || state == ZoomValueState::kWrongType;
}

// The zoom in effect and the evidence for it (pure): |registry| is the Zoom value as stored
// (std::nullopt: missing), |legacyIniZoom| what the 1.x settings file says (std::nullopt: no file
// or no Zoom key in it; otherwise already read and clamped like the handler does).
struct ZoomDecision {
  int zoom = 100;
  ZoomOrigin origin = ZoomOrigin::kDefault;
  ZoomValueState registry = ZoomValueState::kMissing;
};
ZoomDecision DecideZoom(const std::optional<RegistryValue>& registry,
                        std::optional<int> legacyIniZoom) noexcept;

// The 1.x settings file the handler reads: the file named by the environment variable
// kLegacySettingsEnv when it is set and shorter than MAX_PATH (the handler's own limit), else
// %LOCALAPPDATA%<kLegacySettingsIni>. The variable exists for tests; honouring it here keeps the
// app and the handler in agreement wherever both see the same environment.
[[nodiscard]] HRESULT LegacyIniPath(std::filesystem::path* ini) noexcept;

// The Zoom in |ini|'s [Thumbnails] section, read like the handler reads it (GetPrivateProfileIntW,
// then clamped). std::nullopt when the file or the key does not exist.
[[nodiscard]] HRESULT ReadLegacyIniZoom(const std::filesystem::path& ini,
                                        std::optional<int>* zoom) noexcept;

// Everything LoadSettings found out about the zoom.
struct ZoomDetails {
  ZoomDecision decision;
  DWORD registryType = REG_NONE;     // the Zoom value's type, when it exists
  std::optional<DWORD> registryRaw;  // its number, when it is a 4-byte REG_DWORD
  std::filesystem::path legacyIni;   // the settings file looked at
  std::optional<int> legacyIniZoom;  // what it says (clamped)
};

// Reads the settings from roots.user and the 1.x settings file (LegacyIniPath).
[[nodiscard]] HRESULT LoadSettings(const RegistryRoots& roots, Settings* settings,
                                   ZoomDetails* details = nullptr) noexcept;

// The same with the 1.x settings file |legacyIni|.
[[nodiscard]] HRESULT LoadSettingsWithIni(const RegistryRoots& roots,
                                          const std::filesystem::path& legacyIni,
                                          Settings* settings,
                                          ZoomDetails* details = nullptr) noexcept;

// Writes the zoom as a REG_DWORD, clamped to 100..200.
[[nodiscard]] HRESULT SaveZoom(const RegistryRoots& roots, int zoom) noexcept;

[[nodiscard]] HRESULT SaveConfirmFullRefresh(const RegistryRoots& roots, bool confirm) noexcept;

// HKCU\<product key>\State, relative to roots.user. May throw std::bad_alloc.
std::wstring StateKeyPath();

struct CheckRecord {
  FILETIME time = {};  // UTC
  ExitCode result = ExitCode::kOk;
  std::wstring summary;
  std::wstring pdfcraftVersion;
  std::optional<HRESULT> liveTest;  // the Explorer test's result, when it ran
};

// Records a check. The time is written last, so a reader that sees the new time sees the rest of
// the new record too; a live test that did not run removes the old LastLiveTest.
[[nodiscard]] HRESULT RecordCheck(const RegistryRoots& roots, const CheckRecord& record) noexcept;

// The last recorded check; std::nullopt when there is none, or when LastCheckTime or
// LastCheckResult is missing or not a value RecordCheck writes. Other values that are missing or
// of the wrong type read as empty.
[[nodiscard]] HRESULT ReadCheck(const RegistryRoots& roots,
                                std::optional<CheckRecord>* record) noexcept;

// Whether the 1.x clean-up has run for this user (State\MigratedFromV1 is a non-zero DWORD).
[[nodiscard]] HRESULT ReadMigratedFromV1(const RegistryRoots& roots, bool* migrated) noexcept;

// Records that the 1.x clean-up has run for this user (State\MigratedFromV1 = 1).
[[nodiscard]] HRESULT WriteMigratedFromV1(const RegistryRoots& roots) noexcept;

}  // namespace pdft
