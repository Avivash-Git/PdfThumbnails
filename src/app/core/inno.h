// An install made by the product's installer (Inno Setup), seen from the app.
//
// Inno Setup records each install under
//   Software\Microsoft\Windows\CurrentVersion\Uninstall\<AppId>_is1
// in HKLM (an install for all users, 64-bit view) or HKCU (an install for one user), with the
// command that uninstalls it. While such an install exists the app's own install and uninstall
// commands step aside: installing over it would leave the installer's records wrong, and
// deleting its folder would leave an entry in Installed apps that can no longer work, so the
// uninstall runs the installer's uninstaller instead.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "exit_code.h"
#include "registry_roots.h"

namespace pdft {

// "Software\Microsoft\Windows\CurrentVersion\Uninstall\<AppId>_is1" (pure). May throw
// std::bad_alloc.
std::wstring InnoUninstallKey();

struct InnoInstall {
  Scope scope = Scope::kNone;    // kMachine (HKLM) or kUser (HKCU)
  std::wstring uninstallString;  // as stored; empty when the value is missing or not a string
};

// The installer's record under roots.machine first, then roots.user; std::nullopt when neither
// has the key. The key alone counts, with or without its UninstallString.
[[nodiscard]] HRESULT FindInnoInstall(const RegistryRoots& roots,
                                      std::optional<InnoInstall>* install) noexcept;

// The uninstaller in an UninstallString (pure): Inno Setup writes the program's full path in
// quotes ("C:\...\unins000.exe"); an unquoted path is accepted only without spaces. The path must
// be an absolute drive path to an .exe, with nothing after it. E_INVALIDARG otherwise. May throw
// std::bad_alloc.
[[nodiscard]] HRESULT ParseInnoUninstallString(std::wstring_view text, std::filesystem::path* exe);

// The uninstaller's arguments (pure): "/SILENT" when the user already said yes (no questions,
// only a progress window), else none.
std::wstring InnoUninstallArguments(bool silent);

}  // namespace pdft
