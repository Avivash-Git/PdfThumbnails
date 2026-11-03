// File operations for installing and removing the product: copying a build into place, removing
// folders that may still be in use, the per-user clean-up that runs at the next sign-in, and the
// checks that come first (no links on the way to a target, nobody else able to change a build
// folder that elevated code is about to copy from, which then stays open until it is copied).
//
// Links and junctions (reparse points) are never followed by anything here: a target path that
// leads through one is refused, and one found inside a folder being removed is removed itself,
// never entered.

#pragma once

#include <windows.h>

#include <wil/resource.h>

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
//   a folder, a pipe or a device fails with HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE); a pipe's
//   server gets an identification token at most, never one it could act with). Nobody may
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
// user and administrators may be able to change that folder, or anything copied from it, before
// or while it does. The check looks at the owner and the permissions of the folder, of every
// file and folder that is copied from it and of every folder above it up to the volume root.
// Only these are trusted:
//   the user the check is made for (the one who asked for the install)
//   Administrators, SYSTEM, TrustedInstaller
//   the built-in Administrator account (RID 500) and Domain Admins (512) of this PC's own
//   accounts, of the domain it is joined to and of the user's own domain: they administer this
//   PC or the user's account anyway. The same RIDs of any other domain (one in another forest
//   that this PC only trusts, say) are not administrators here, and Enterprise Admins are
//   administrators of a member PC only through Administrators.
// An owner that is not trusted makes the object unsafe by itself: an owner may always change the
// permissions. Every other account or group that an entry names is judged with Everyone: each
// of the broad groups (Everyone, Authenticated Users, Users, INTERACTIVE) on its own and all four
// together, and every other SID on its own, so a deny entry for one cannot hide what another may
// do, and a deny that comes first (for that SID or for Everyone) still counts. The rights that
// count:
//   the folder and what is copied  writing or appending (adding files or folders to a folder),
//                                  deleting, changing permissions or the owner, deleting children
//   the folders above              deleting, changing permissions or the owner, deleting children
//                                  (adding new entries to a folder above changes nothing below it:
//                                  C:\ itself lets Authenticated Users add folders)
// A missing (null) DACL grants everything. Entries that apply only to children (inherit-only) do
// not count for the object itself. An allow entry with a condition or an object type counts as
// an allow for its SID (the condition may hold); a deny entry with one as if it did not apply
// (the condition may not hold). An entry of any other kind counts as allowing its rights to
// everyone. CREATOR OWNER and CREATOR GROUP in an entry that applies to the object match nobody;
// OWNER RIGHTS stands for the owner. Entries for app containers and their capabilities
// (S-1-15-...) are left out: Windows grants an app container only what its user and groups get
// as well, so they can never add a right.

// The rights that count on the folder and what is copied from it, and on the folders above it.
// Files and folders share one generic mapping, the one Windows uses for every file object:
// GENERIC_ALL gives a folder FILE_DELETE_CHILD, GENERIC_WRITE lets it take new files and folders.
inline constexpr ACCESS_MASK kBuildObjectChangeRights =
    FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD;
inline constexpr ACCESS_MASK kBuildParentChangeRights =
    DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD;

// An account or group that is not trusted, and what it may do.
struct BroadAccess {
  std::wstring principal;  // "Authenticated Users", "PC\name", or the SID when it has no name
  ACCESS_MASK rights = 0;  // the rights of the mask it has
  std::wstring sid;        // "S-1-5-11"; empty when there is none to name
  bool owner = false;      // it owns the object (|rights|: what owning gives)
  bool lookedUp = false;   // |principal| is the name Windows gave |sid| (an account's, maybe)
};

// Which of the rights in |mask| an account or group other than the trusted ones gets from
// |descriptor| (an in-memory self-relative or absolute security descriptor with its owner and
// DACL), with file rights (generic rights are mapped first), when |user| (may be null: then only
// the administrators are trusted) asks. |domains| are this PC's own accounts' domain and the one
// it is joined to ("S-1-5-21-a-b-c"), whose RIDs 500 and 512 are trusted like those of |user|'s
// own domain. The owner comes first; std::nullopt when nobody else gets any. Pure apart from
// reading |descriptor| (unknown SIDs are named by their text). May throw std::bad_alloc.
std::optional<BroadAccess> FindBroadAccess(PSECURITY_DESCRIPTOR descriptor, ACCESS_MASK mask,
                                           PSID user, std::span<const PSID> domains);

// Why an object makes a build's folder unsafe to install from.
enum class UnsafeKind {
  kAccess,   // an account or group that is not trusted may change it, or owns it (|access|)
  kLink,     // it is a link, a junction or a mount point (|reparseTag|), or lies behind one
  kNetwork,  // it is on a network drive: another computer serves what is read
};

// An object of a build's folder that others could change.
struct UnsafeAccess {
  std::filesystem::path object;  // the folder, a file or folder in it, or a folder above
  BroadAccess access;            // with kLink and kNetwork, words that fit "can be changed by"
  UnsafeKind kind = UnsafeKind::kAccess;
  DWORD reparseTag = 0;  // kLink: the reparse tag; 0 when a folder on the way led elsewhere
};

// What was found, without the object's path: "can be changed by Authenticated Users
// (S-1-5-11)", "is owned by PC\name (S-1-5-21-...)", "is a junction or mount point", "is on a
// network drive" (pure). Without |accountNames|, a name Windows looked up for a SID is left out
// and the SID stands alone ("is owned by S-1-5-21-..."): such a name may be a person's, and
// where the text is logged, diagnostics masks SIDs but not other accounts' names. May throw
// std::bad_alloc.
std::wstring UnsafeAccessReason(const UnsafeAccess& unsafe, bool accountNames = true);

// "<object> <UnsafeAccessReason>" (pure). May throw std::bad_alloc.
std::wstring UnsafeAccessText(const UnsafeAccess& unsafe);

// |path| is on another computer: a UNC path (\\server\share, \\?\UNC\...), or a drive letter
// that Windows maps to a network share. A \\?\GLOBALROOT\ or \\.\GLOBALROOT\ path counts too: it
// names an object of the object manager (\Device\Mup, a share, among them) that cannot be judged
// without opening it. Reads nothing from |path| itself.
bool IsOnNetworkDrive(const std::filesystem::path& path) noexcept;

// What is reported for |folder| when it is on a network drive (IsOnNetworkDrive), else
// std::nullopt.
// May throw std::bad_alloc.
std::optional<UnsafeAccess> FindNetworkFolder(const std::filesystem::path& folder);

// What HoldBuildFolder holds and judges besides the folder and the folders above it. Names are
// relative to the folder, without "." or "..".
struct BuildFolderHoldOptions {
  std::span<const std::wstring_view> files;     // must be there as regular files (the binaries)
  std::span<const std::wstring_view> optional;  // held when there as regular files (notices)
  // A folder held with every folder and regular file in it when it is there (licenses), at most
  // |treeDepth| folders deep counting itself; deeper folders, other reparse points (not links)
  // and a file of that name are left out, as they are not copied.
  std::wstring_view tree;
  int treeDepth = 0;
  // The account the check is made for, as "S-1-5-21-..."; empty: this process's user.
  std::wstring_view user;
  // false: the owners and permissions are not judged (tests whose folders above %TEMP% are the
  // PC's). Network folders and links are refused, and everything is held, all the same.
  bool checkAccess = true;
};

class HeldBuildFolder;

// Opens |folder| (an absolute drive path, "X:\..."), every folder above it and everything
// |options| names, and judges them as the comment above this section says. Nothing is resolved:
// a link, junction or mount point anywhere on the path or among what is copied is itself
// refused (callers pass the folder's final path, Canonicalize, so a link the user's own path went
// through is already resolved). A network folder is refused before anything is opened.
// Everything stays open in |held| until it is released, so that nobody can rename or delete any
// of it, nor write to a file that is copied, meanwhile: copy from there (CopyOpenFileVerified).
// S_OK with *unsafe set and nothing held when something was found; a failure when something
// cannot be opened or read (a missing binary: HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), one
// that is a folder or another kind of reparse point: HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE);
// a file someone has open for writing: HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)).
[[nodiscard]] HRESULT HoldBuildFolder(const std::filesystem::path& folder,
                                      const BuildFolderHoldOptions& options, HeldBuildFolder* held,
                                      std::optional<UnsafeAccess>* unsafe) noexcept;

// What HoldBuildFolder holds: the folders open so that nobody can rename or delete them, the
// files so that nobody can also write to them.
class HeldBuildFolder {
 public:
  HeldBuildFolder() = default;
  HeldBuildFolder(HeldBuildFolder&&) = default;
  HeldBuildFolder& operator=(HeldBuildFolder&&) = default;
  HeldBuildFolder(const HeldBuildFolder&) = delete;
  HeldBuildFolder& operator=(const HeldBuildFolder&) = delete;
  ~HeldBuildFolder() = default;

  // The folder as HoldBuildFolder got it; empty when nothing is held.
  const std::filesystem::path& dir() const noexcept { return dir_; }
  // The files held, relative to dir(): the required ones, the optional ones that were there,
  // then the tree's, folder by folder in name order (ignoring case).
  const std::vector<std::filesystem::path>& files() const noexcept { return files_; }
  // The open handle of the held file |relative| (as files() names it, ignoring case), readable
  // from its start; nullptr when it is not held.
  HANDLE File(const std::filesystem::path& relative) const noexcept;
  // Closes everything.
  void Release() noexcept;

 private:
  friend HRESULT HoldBuildFolder(const std::filesystem::path& folder,
                                 const BuildFolderHoldOptions& options, HeldBuildFolder* held,
                                 std::optional<UnsafeAccess>* unsafe) noexcept;

  std::filesystem::path dir_;
  std::vector<std::filesystem::path> files_;
  std::vector<wil::unique_hfile> fileHandles_;  // one per entry of files_
  std::vector<wil::unique_hfile> folders_;      // the folders above, the folder, the tree's
};

// Copies the open file |from| (from its start; a held file, HeldBuildFolder::File) to |to| the
// way CopyFileVerified copies a path, and returns the SHA-256 of the bytes copied in |sha|
// (optional).
[[nodiscard]] HRESULT CopyOpenFileVerified(HANDLE from, const std::filesystem::path& to,
                                           Sha256Digest* sha = nullptr) noexcept;

// The check alone, nothing kept open: |folder| (canonicalized first, so links on the way are
// resolved and the real folders are judged; only a mount point or a link made meanwhile is
// refused then), the files |binaries| in it (names relative to it) and every folder above it,
// for this process's user or for the account |userSid| ("S-1-5-21-..."). A network folder is
// refused before it is opened. *unsafe is std::nullopt when nobody else can change any of them,
// else the first thing found. Fails when a descriptor cannot be read or a binary is missing.
[[nodiscard]] HRESULT FindUnsafeBuildFolderAccess(const std::filesystem::path& folder,
                                                  std::span<const std::wstring> binaries,
                                                  std::optional<UnsafeAccess>* unsafe) noexcept;
[[nodiscard]] HRESULT FindUnsafeBuildFolderAccess(const std::filesystem::path& folder,
                                                  std::span<const std::wstring> binaries,
                                                  std::wstring_view userSid,
                                                  std::optional<UnsafeAccess>* unsafe) noexcept;

}  // namespace pdft
