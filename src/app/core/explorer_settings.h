// Explorer settings that hide every thumbnail, whatever the handler does, and the Windows build
// the status and the diagnostics name.
//
//   IconsOnly          HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced,
//                      REG_DWORD 1: Folder Options > View > "Always show icons, never
//                      thumbnails" (1.x reported it the same way).
//   DisableThumbnails  Software\Microsoft\Windows\CurrentVersion\Policies\Explorer, under HKCU
//                      and HKLM: the policy "Turn off the display of thumbnails and only display
//                      icons" (Thumbnails.admx, a user policy). A non-zero REG_DWORD turns it on.
// Windows' own policy definitions name no other value that turns thumbnails off everywhere:
// DisableThumbnailsOnNetworkFolders covers network folders only, NoThumbnailCache only stops
// caching them, and there is no "NoThumbnails" policy.
//
// Also whether Windows restarts File Explorer by itself after it ends (AutoRestartShell under
// HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon), which rebuilding the thumbnail
// cache relies on.
//
// Everything is read through RegistryRoots, so tests plant their own values.

#pragma once

#include <windows.h>

#include <optional>
#include <string>

#include "registry_roots.h"

namespace pdft {

inline constexpr wchar_t kExplorerAdvancedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
inline constexpr wchar_t kIconsOnlyValue[] = L"IconsOnly";
inline constexpr wchar_t kExplorerPoliciesKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
inline constexpr wchar_t kDisableThumbnailsValue[] = L"DisableThumbnails";

struct ExplorerSettings {
  bool iconsOnly = false;                     // the user's Folder Options choice
  bool thumbnailsOffByUserPolicy = false;     // DisableThumbnails under roots.user
  bool thumbnailsOffByMachinePolicy = false;  // DisableThumbnails under roots.machine
};

// IconsOnly as 1.x read it: a REG_DWORD equal to 1 (pure).
bool IsIconsOnlyValue(const std::optional<RegistryValue>& value) noexcept;

// A policy switch: a REG_DWORD other than 0 (pure). Anything else leaves the policy off.
bool IsPolicyOnValue(const std::optional<RegistryValue>& value) noexcept;

// Reads the three values. Read-only.
[[nodiscard]] HRESULT ReadExplorerSettings(const RegistryRoots& roots,
                                           ExplorerSettings* settings) noexcept;

// ---- Restarting File Explorer -------------------------------------------------------------

inline constexpr wchar_t kWinlogonKey[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
inline constexpr wchar_t kAutoRestartShellValue[] = L"AutoRestartShell";

// Windows restarts File Explorer after it ends abnormally unless AutoRestartShell is a REG_DWORD
// 0 (pure): missing, or any other type or value, means on.
bool IsAutoRestartShellOn(const std::optional<RegistryValue>& value) noexcept;

// Reads AutoRestartShell under roots.machine (IsAutoRestartShellOn). Read-only.
[[nodiscard]] HRESULT ReadAutoRestartShell(const RegistryRoots& roots, bool* on) noexcept;

// ---- Windows build ------------------------------------------------------------------------

inline constexpr wchar_t kWindowsVersionKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

// "<build>.<update revision>" ("26300.9818") from CurrentBuild (or, when it is missing or not a
// number, CurrentBuildNumber) and UBR (pure). "<build>" without a usable UBR, "" without a
// build. Builds are 1 to 10 digits; anything else is not shown. May throw std::bad_alloc.
std::wstring FormatOsBuild(const std::optional<std::wstring>& currentBuild,
                           const std::optional<std::wstring>& currentBuildNumber,
                           std::optional<DWORD> ubr);

// The Windows build from roots.machine (FormatOsBuild). Not ProductName, which says "Windows 10"
// on Windows 11. Read-only; "" when nothing usable is there.
[[nodiscard]] HRESULT ReadOsBuild(const RegistryRoots& roots, std::wstring* build) noexcept;

}  // namespace pdft
