// File operations for installing and removing the product: copying a build into place, removing
// folders that may still be in use, the per-user clean-up that runs at the next sign-in, and the
// checks that come first (no links on the way to a target, nobody else able to change a build
// folder that elevated code is about to copy from).
//
// Links and junctions (reparse points) are never followed by anything here: a target path that
// leads through one is refused, and one found inside a folder being removed is removed itself,
// never entered.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "build_folder.h"
#include "registry_roots.h"

namespace pdft {

// Fails with HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK) when |path| or any folder above it up
// to and including |stopAt| exists and is a reparse point (a link, a junction, a mount point or
// anything else with a reparse tag). Parts that do not exist yet pass: they cannot redirect
// anything, and a caller that creates them checks again afterwards. |stopAt| must be |path| or a
// folder above it; empty means the volume root. Both must be absolute (E_INVALIDARG otherwise).
[[nodiscard]] HRESULT CheckNoReparsePoints(const std::filesystem::path& path,
                                           const std::filesystem::path& stopAt) noexcept;

// Copies the file |from| to |to| and returns the SHA-256 of the bytes copied in |sha| (optional).
// - The source is opened without following a link, and must be a regular file on disk (a link,
//   a folder, a pipe or a device fails with HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE)). Nobody may
//   write to or delete it while it is open: a file someone else has open for writing fails with
//   HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION).
// - The bytes go to a new temporary file next to |to|, are read back and hashed again, and only
//   then replace |to| by a rename, so |to| is never half written. Any failure deletes the
//   temporary file. The folder of |to| must exist and must not be a reparse point.
// - An existing |to| is replaced; one that is in use (a running program, a loaded DLL) fails
//   with HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) and stays as it was.
// The copy gets the permissions its new folder passes on, not the source's.
[[nodiscard]] HRESULT CopyFileVerified(const std::filesystem::path& from,
                                       const std::filesystem::path& to,
                                       Sha256Digest* sha = nullptr) noexcept;

// Deletes |dir| and everything in it. Links and junctions inside are removed themselves; their
// targets are never entered. Read-only files are deleted too. A missing |dir| is not an error.
// Fails with the first error; everything that could be deleted is gone by then.
[[nodiscard]] HRESULT RemoveTreeNoFollow(const std::filesystem::path& dir) noexcept;

// Makes a new empty folder <trashParent>\<Product>.trash-<32 hex digits> for files that are in
// use and cannot be deleted yet. |trashParent| must be on the same volume as the files that go
// there (a move never copies).
[[nodiscard]] HRESULT MakeTrashFolder(const std::filesystem::path& trashParent,
                                      std::filesystem::path* trash) noexcept;

// A name MakeTrashFolder gives (pure): <Product>.trash- and 32 hexadecimal digits, ignoring case.
bool IsTrashFolderName(std::wstring_view name) noexcept;

// Moves the regular file |file| into the folder |trash| under a new name (<32 hex digits> plus
// its extension), without copying. Works for a loaded DLL or a running program, which can be
// renamed but not deleted. |moved| (optional) receives the new path.
[[nodiscard]] HRESULT MoveIntoTrash(const std::filesystem::path& file,
                                    const std::filesystem::path& trash,
                                    std::filesystem::path* moved = nullptr) noexcept;

// Asks Windows to delete |path| (a file, or an empty folder) at the next restart
// (MOVEFILE_DELAY_UNTIL_REBOOT). Needs administrator rights: otherwise it fails with
// HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED). Never call it for a path a later install may reuse:
// the deletion would hit the new file. Only for files already moved into a trash folder.
[[nodiscard]] HRESULT ScheduleDeleteAtRestart(const std::filesystem::path& path) noexcept;

// What RemoveTreeOrTrash did.
struct RemoveOutcome {
  bool removed = false;  // |dir| is gone (some of its files possibly into a trash folder)
  // Something stays until Windows restarts or the user signs in again: files moved into a trash
  // folder and queued for deletion at the restart, or files left in place because they are in
  // use (|left|; the caller schedules their clean-up, ScheduleRunOnceRemoval).
  bool restartPending = false;
  std::filesystem::path trash;              // the trash folder, when one was made
  std::vector<std::filesystem::path> left;  // files left in place, in use
};

// Removes the real folder |dir| (never a link or junction itself: that fails with
// HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK)) with everything in it, without following links
// inside. Files that are in use are retried for up to |retryMs| milliseconds. What is still in
// use then:
// - elevated (TokenElevation), with a |trashParent|: moved into a new trash folder there
//   (MakeTrashFolder), each file and then the trash folder queued for deletion at the next
//   restart, and |dir| removed;
// - otherwise left in place (outcome->left), with |dir| and the folders that hold them.
// A missing |dir| counts as removed. Fails for other errors (access denied to a file that is not
// in use, for example), after removing what it could.
[[nodiscard]] HRESULT RemoveTreeOrTrash(const std::filesystem::path& dir,
                                        const std::filesystem::path& trashParent, DWORD retryMs,
                                        RemoveOutcome* outcome) noexcept;

// ---- The clean-up at the next sign-in (HKCU RunOnce) ----------------------------------------
// A folder that cannot be removed now (a program in it is running, a DLL in it is loaded) is
// removed when the user next signs in, by a RunOnce value per folder:
//   <Product>Cleanup, <Product>Cleanup2, <Product>Cleanup3, ...
// each holding
//   "<System32>\cmd.exe" /d /c rd /s /q "<folder>"
// with the System32 path written out (never a bare cmd.exe, which the sign-in would look up on
// the search path). Windows runs a RunOnce command line only up to 260 characters, so a longer
// one is not written at all: the next check retries the removal itself. Everything that installs
// or checks deletes the whole family again (DeleteRunOnceRemovals), because 1.x's value of the
// same name would delete the per-user folder that 2.x shares with it.

// The RunOnce value name for the |index|th folder (pure): 0 gives <Product>Cleanup, 1 gives
// <Product>Cleanup2, and so on.
std::wstring RunOnceValueName(size_t index);

// A value name of the family (pure): <Product>Cleanup followed by nothing or by digits, ignoring
// case; also 1.x's own value name.
bool IsRunOnceRemovalName(std::wstring_view name) noexcept;

// Windows' limit for a RunOnce command line, in characters.
inline constexpr size_t kRunOnceMaxChars = 260;

// The command line that removes |dir| (pure), with |cmdExe| as cmd.exe's full path. Fails with
// E_INVALIDARG unless |dir| is an absolute drive path ("X:\...") below the drive's root, and
// with HRESULT_FROM_WIN32(ERROR_INVALID_NAME) when |dir| holds a character the command line
// cannot carry safely (a quote, %, !, or a control character); with
// HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE) when the line would not be shorter than
// kRunOnceMaxChars. A trailing backslash is dropped. May throw std::bad_alloc.
[[nodiscard]] HRESULT RunOnceRemovalCommand(const std::filesystem::path& cmdExe,
                                            const std::filesystem::path& dir,
                                            std::wstring* command);

// Writes one RunOnce value under roots.user per folder in |dirs|, at the first free name of the
// family; a folder whose command is already there is not written twice. Folders whose command
// cannot be written (RunOnceRemovalCommand fails) are listed in |notScheduled| (optional), and
// the result is then S_FALSE.
[[nodiscard]] HRESULT ScheduleRunOnceRemoval(
    const RegistryRoots& roots, std::span<const std::filesystem::path> dirs,
    std::vector<std::filesystem::path>* notScheduled = nullptr) noexcept;

// Deletes every value of the family under roots.user's RunOnce key, and nothing else there.
// |deleted| (optional) receives how many went.
[[nodiscard]] HRESULT DeleteRunOnceRemovals(const RegistryRoots& roots,
                                            size_t* deleted = nullptr) noexcept;

// The names of the family's values under roots.user's RunOnce key, as stored.
[[nodiscard]] HRESULT FindRunOnceRemovals(const RegistryRoots& roots,
                                          std::vector<std::wstring>* names) noexcept;

// ---- Deleting a file by its contents --------------------------------------------------------

// What DeleteFileIfContent found.
enum class ContentDelete {
  kDeleted,
  kMissing,   // nothing of that name
  kMismatch,  // a regular file, but its contents are none of the expected ones: kept
  kNotAFile,  // a folder, a link or junction, a device: kept, never followed
  kInUse,     // another process has it open for writing or deleting, or runs or maps it: kept
};

// Deletes |file| if it is a regular file (never a link: the name itself is opened) whose SHA-256
// is one of |allowed|; an empty |allowed| accepts any contents. The file is opened so that nobody
// can write, rename or delete it until it is gone, hashed through that handle and deleted through
// it, so what was hashed is what goes; a read-only file goes too. S_OK with *result saying what
// happened; a failure only for other errors (the folder cannot be read, ...).
[[nodiscard]] HRESULT DeleteFileIfContent(const std::filesystem::path& file,
                                          std::span<const Sha256Digest> allowed,
                                          ContentDelete* result) noexcept;

// ---- Who else could change a build folder ---------------------------------------------------
// Elevated code copies a build from a folder and registers it for every user, so nobody but the
// user and administrators may be able to change that folder before or while it does. The check
// looks at the permissions of the folder, of each binary in it and of every folder above it up
// to the volume root, as they apply to the broad groups any other account belongs to: Everyone,
// Authenticated Users, Users and INTERACTIVE.
//   the folder and its binaries  writing or appending (adding files or folders to the folder),
//                                deleting, changing permissions or the owner, deleting children
//   the folders above            deleting, changing permissions or the owner, deleting children
//                                (adding new entries to a folder above changes nothing below it:
//                                C:\ itself lets Authenticated Users add folders)
// An owner among the groups counts as able to change permissions. A missing (null) DACL grants
// everything. Entries that apply only to children (inherit-only) do not count for the object
// itself; an allow entry with a condition counts as if the condition held, a deny entry with
// one as if it did not.

// The rights that count on the folder and its binaries, and on the folders above it.
inline constexpr ACCESS_MASK kBuildObjectChangeRights =
    FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD;
inline constexpr ACCESS_MASK kBuildParentChangeRights =
    DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD;

// A broad group and what it may do.
struct BroadAccess {
  std::wstring principal;  // "Authenticated Users", ...
  ACCESS_MASK rights = 0;  // the rights of the mask it has
};

// Which of the rights in |mask| one of the broad groups gets from |descriptor| (an in-memory
// self-relative or absolute security descriptor with its owner and DACL), with file rights
// (generic rights are mapped first). std::nullopt when none. Each group is judged on its own,
// together with Everyone, and all four together, so a deny entry for one group cannot hide what
// another group may do. Pure apart from reading |descriptor|. May throw std::bad_alloc.
std::optional<BroadAccess> FindBroadAccess(PSECURITY_DESCRIPTOR descriptor, ACCESS_MASK mask);

// An object one of the broad groups could change.
struct UnsafeAccess {
  std::filesystem::path object;  // the folder, a binary or a folder above
  BroadAccess access;
};

// Checks |folder| (canonicalized first, so links on the way are resolved and the real folders
// are judged), the files |binaries| in it (names relative to it) and every folder above it.
// *unsafe is std::nullopt when nobody else can change any of them, else the first object that
// can be changed. Fails when a descriptor cannot be read or a binary is missing.
[[nodiscard]] HRESULT FindUnsafeBuildFolderAccess(const std::filesystem::path& folder,
                                                  std::span<const std::wstring> binaries,
                                                  std::optional<UnsafeAccess>* unsafe) noexcept;

}  // namespace pdft
