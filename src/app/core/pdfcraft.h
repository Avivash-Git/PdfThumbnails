// Whether PdfCraft is installed, and which version: the check records it, and the status shows
// it next to the default app ("PdfCraft 0.14.0"). PdfCraft is the PDF app whose installs and
// updates the check task watches; it is only detected, never touched.
//
// Three ways, each used only when the one before finds nothing:
//   1. Windows Installer, by PdfCraft's UpgradeCode (MsiEnumRelatedProductsW): the stable
//      identity of an MSI product line across versions;
//   2. Windows Installer, every product of this user and the machine (MsiEnumProductsExW) whose
//      ProductName is "PdfCraft", in case a future release changes the UpgradeCode;
//   3. the Uninstall keys (HKLM in the 64-bit and the 32-bit view, then HKCU) whose DisplayName is
//      "PdfCraft", in case it ever ships as something other than an MSI.
// msi.dll is delay-loaded; it is loaded only when PdfCraft is looked for.

#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

#include "registry_roots.h"

namespace pdft {

enum class PdfCraftSource { kNone, kUpgradeCode, kProductName, kUninstallKey };

struct PdfCraftInfo {
  bool installed = false;
  std::wstring version;      // "0.14.0"; may be empty when only the Uninstall key knew it
  std::wstring productCode;  // "{...}" (the MSI product code); empty when not known
  PdfCraftSource source = PdfCraftSource::kNone;
};

// Uninstall keys, relative to a root.
inline constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

// Versions longer than this are cut: the version comes from data any installer can write, and
// only a short number is useful in a status line.
inline constexpr size_t kMaxPdfCraftVersionChars = 64;

// |name| is PdfCraft's product name, ignoring case (pure).
bool IsPdfCraftName(std::wstring_view name) noexcept;

// A version as shown (pure): cut at kMaxPdfCraftVersionChars, control characters dropped. May
// throw std::bad_alloc.
std::wstring CleanVersionText(std::wstring_view version);

// Of two finds, the one to report (pure): the one with the higher version (CompareFileVersions),
// the first when they are equal.
const PdfCraftInfo& NewerPdfCraft(const PdfCraftInfo& a, const PdfCraftInfo& b) noexcept;

// Ways 1 and 2. Products that are only advertised, not installed, do not count. An unusable
// Windows Installer (msi.dll not loadable, the service failing) fails; finding nothing is
// installed = false.
[[nodiscard]] HRESULT DetectPdfCraftWithInstaller(PdfCraftInfo* info) noexcept;

// Way 3, through |roots|: roots.machine in the 64-bit view, then the 32-bit view (the same key
// under a test's private root), then roots.user. The newest match wins (NewerPdfCraft). Read-only.
[[nodiscard]] HRESULT DetectPdfCraftInUninstallKeys(const RegistryRoots& roots,
                                                    PdfCraftInfo* info) noexcept;

// All three ways, on the real system. A way that fails is skipped (the next one may still
// answer); only running out of memory fails. Read-only.
[[nodiscard]] HRESULT DetectPdfCraft(PdfCraftInfo* info) noexcept;

}  // namespace pdft
