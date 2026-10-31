// What a 1.x install left behind, for the check's "1.x leftovers" report and, later, the 1.x
// clean-up. Detection only: nothing here deletes or changes anything.
//
// The per-user folder %LOCALAPPDATA%\Programs\<1.x name> is also where a per-user 2.x install
// (and its installer) puts its own files, so files count as 1.x leftovers only by exact name,
// and names a 2.x install might also use only with evidence that 1.x put them there:
//   always 1.x     <1.x name>.ps1, <1.x name>.cmd, settings.ini, repair.log, repair.log.old,
//                  elevated.log, bin\, and app\1.* build folders
//   with a 1.x     README.md and LICENSE, while <1.x name>.ps1 or .cmd is still there (2.x
//   manager        ships neither name)
//   with nothing   licenses\, while <1.x name>.ps1 or .cmd is still there, nothing of 2.x is
//   of 2.x         (its executables, LICENSE.txt, THIRD-PARTY-NOTICES.md, an uninstaller), and
//                  the folder holds only files from 1.x's license set, no subfolders
// Links and junctions (reparse points) are never counted and never looked into.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "registration.h"
#include "registry_roots.h"

namespace pdft {

// HKCU's RunOnce key, where 1.x's uninstall left a command that deletes the per-user folder at
// the next sign-in (value kLegacyRunOnce).
inline constexpr wchar_t kRunOnceKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";

struct LegacyState {
  bool runOnce = false;        // HKCU RunOnce holds the 1.x clean-up value
  bool legacyTask = false;     // the 1.x scheduled task exists; the task reader fills this in
  bool ini = false;            // the 1.x settings.ini exists in the 1.x per-user folder
  std::optional<int> iniZoom;  // its zoom (clamped like the handler), when it has one
  std::vector<std::filesystem::path> files;  // 1.x files and folders, full paths
  bool userClassInV1 = false;     // the per-user class's DLL is in an app\1.* build folder
  bool machineClassInV1 = false;  // the machine-wide class's DLL is
};

// Anything that 1.x left and its clean-up removes (pure). A machine-wide class in a 1.x build is
// not one: it is the install itself until 2.0 is installed for all users.
bool HasV1Leftovers(const LegacyState& state) noexcept;

// One entry of a folder listing.
struct FolderEntry {
  std::wstring name;
  bool directory = false;
  bool reparsePoint = false;
};

// The parts of the 1.x per-user folder the classifier looks at: its own entries, those of its
// app\ folder and those of its licenses\ folder (both listed only when they are real folders).
struct UserFolderListing {
  std::vector<FolderEntry> root;
  std::vector<FolderEntry> app;
  std::vector<FolderEntry> licenses;
};

// The 1.x leftovers in |listing| by the rules at the top, as paths relative to the folder
// ("bin", "app\1.1.0.0-67a1323e"), sorted ignoring case (pure). May throw std::bad_alloc.
std::vector<std::wstring> ClassifyV1Files(const UserFolderListing& listing);

// |dll| lies in a 1.x build folder of the install root |root|: <root>\app\1.<anything>\<file>,
// compared as IsUnderLexically does (pure).
bool IsInV1BuildFolder(const std::filesystem::path& dll,
                       const std::filesystem::path& root) noexcept;

// The 1.x install roots: FOLDERID_UserProgramFiles\<1.x name> (the per-user folder) and
// FOLDERID_ProgramFiles\<1.x name>. They stay where 1.x put them even if this product is renamed.
[[nodiscard]] HRESULT GetLegacyUserRoot(std::filesystem::path* root) noexcept;
[[nodiscard]] HRESULT GetLegacyMachineRoot(std::filesystem::path* root) noexcept;

// Lists |root| for ClassifyV1Files, reading only. A missing folder gives an empty listing.
[[nodiscard]] HRESULT ListUserFolder(const std::filesystem::path& root,
                                     UserFolderListing* listing) noexcept;

// Sets userClassInV1 and machineClassInV1 from classes already read (pure): a class counts when
// its DLL is in an app\1.* folder of either 1.x root.
void MarkV1Classes(const std::optional<ClassReg>& userClass,
                   const std::optional<ClassReg>& machineClass,
                   const std::filesystem::path& userRoot, const std::filesystem::path& machineRoot,
                   LegacyState* state) noexcept;

// Everything in LegacyState but legacyTask (left false): the RunOnce value under roots.user,
// settings.ini and the files in |userRoot|, and MarkV1Classes for both classes as read through
// |roots|.
[[nodiscard]] HRESULT ReadLegacyState(const RegistryRoots& roots,
                                      const std::filesystem::path& userRoot,
                                      const std::filesystem::path& machineRoot,
                                      LegacyState* state) noexcept;

}  // namespace pdft
