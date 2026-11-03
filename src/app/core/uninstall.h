// Removing the product: --uninstall, the uninstaller's clean-up step (--uninstall-cleanup) and
// the administrator step machine-uninstall.
//
// --uninstall [--yes] [--rebuild-thumbnails], unelevated:
//   refused where the registry may be virtualized (8) and in an elevated process (8: the
//   per-user part must run as the signed-in user); while the installer (Inno Setup) has an
//   install here, its own uninstaller runs instead (silently with --yes), so its records stay
//   right; the operation lock (4; 5 when Windows does not let this process make it); nothing of
//   the product here: 6. Then
//   1. the per-user part: the .pdf slots that name the handler (and the keys above them left
//      empty), the per-user class, the check task and 1.x's, HKCU\<product key>; Explorer is told;
//   2. the part for all users, when the class for all users or the install folder for all users
//      is there: the administrator step machine-uninstall of the settings executable installed
//      there (a prompt; declined: the part stays, exit 1);
//   3. the user's files: this version's files in the per-user folder, its app folder, and the
//      app's data folder (the log). What is in use stays and is removed at the next sign-in
//      (a RunOnce value per folder, file_ops.h).
//   Exit 0; 1 when the part for all users is left; 5 when a step failed; 7 when nothing else
//   failed but the administrator step did not get administrator rights (the part stays).
//   --rebuild-thumbnails belongs to the thumbnail refresh, which this build does not have: it is
//   accepted and said so, and the uninstall goes on.
//
// machine-uninstall (elevated): the class for all users; every user's check task (the shared task
// folder) and 1.x's task; the install folder for all users, files in use (its own executable
// among them) moved into a trash folder and deleted at the next restart; then, in every user hive
// that is loaded (HKEY_USERS\S-1-5-21-..., and its _Classes hive), the slots that name the
// handler, the handler's class key and the product key. Those keys are deleted without following
// any link and within DeleteRegistryTree's bounds (registry_roots.h): a link or a tree past them
// is left as it is and reported ("left"). Users who are not signed in keep their slots and
// settings; a slot naming a class that is gone shows no thumbnail and hides only other handlers
// registered for .pdf for all users.
//
// --uninstall-cleanup (the installer's uninstall step): the machine-uninstall steps when it runs
// elevated, the per-user registry part when the registry check shows that writes reach the real
// hive, and a RunOnce value for the app's data folder (an elevated process never deletes in the
// user's profile). Never asks anything, and always exits 0. Elevated, the per-user part gathers
// nothing first: it deletes fixed key names only, and never reads a path from the user's registry
// to open it.

#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "context.h"
#include "elevation.h"
#include "exit_code.h"
#include "file_ops.h"
#include "inno.h"
#include "migrate_v1.h"
#include "registration.h"
#include "registry_roots.h"
#include "snapshot.h"
#include "system_probes.h"

namespace pdft {

// ---- The per-user part ------------------------------------------------------------------------

struct UserRemoval {
  std::array<bool, kSlotCount> slots = {};
  bool classDeleted = false;
  bool taskDeleted = false;
  bool legacyTaskDeleted = false;
  bool settingsDeleted = false;  // HKCU\<product key>
  bool notified = false;
  std::vector<std::wstring> failures;
};

// The per-user registry part for the user of |snapshot|: slots if ours, the per-user class, the
// check task (not from an elevated process, where Task Scheduler would not let it act for the
// user; the step for all users deletes every user's task) and 1.x's, the product key; then
// Explorer is told. Of |snapshot| it reads only the context and the task (taskLocation, task),
// so an elevated caller passes one with only the context. Keys left as they were (a link, or a
// tree past DeleteRegistryTree's bounds) are listed in |removal->failures| as "<part> left".
// Fails only when out of memory.
[[nodiscard]] HRESULT RemoveUserRegistration(const RegistryRoots& roots, const SystemProbes& probes,
                                             const Snapshot& snapshot,
                                             UserRemoval* removal) noexcept;

// This version's files in a per-user install root, relative to it (pure): the executables, the
// notices and the licenses this version ships, in the order they are deleted. Folders are listed
// after what is in them and are removed only when empty; app\ goes as a whole.
std::span<const std::wstring_view> UserRootFiles() noexcept;

struct UserFilesRemoval {
  std::vector<std::filesystem::path> left;          // in use, with the folder that holds them
  std::vector<std::filesystem::path> scheduled;     // folders the next sign-in removes (RunOnce)
  std::vector<std::filesystem::path> notScheduled;  // ... whose command could not be written
  std::vector<std::wstring> failures;
};

// Removes this version's files from |userRoot| (UserRootFiles, never a link and never what 1.x
// or anyone else put there), its app folder, the root itself when it is empty then, and the app's
// data folder |dataDir|. A folder that keeps a file in use is removed at the next sign-in
// instead (ScheduleRunOnceRemoval); the per-user root only when 1.x's manager is not in it,
// otherwise only its app folder. For an unelevated caller only: nothing in the user's profile is
// deleted by an administrator process (--uninstall refuses one first). Fails only when out of
// memory.
[[nodiscard]] HRESULT RemoveUserFiles(const RegistryRoots& roots,
                                      const std::filesystem::path& userRoot,
                                      const std::filesystem::path& dataDir,
                                      UserFilesRemoval* removal) noexcept;

// ---- The part for all users (elevated) ------------------------------------------------------

// A user hive's name under HKEY_USERS (pure): S-1-5-21- followed by digits and dashes (a local or
// domain account), optionally with "_Classes" (that user's classes hive, loaded on its own).
// Service accounts and .DEFAULT are not users. |classes| (optional) says which kind it is.
bool IsUserHiveName(std::wstring_view name, bool* classes = nullptr) noexcept;

struct MachineUninstallOptions {
  std::filesystem::path machineRoot;
  std::filesystem::path legacyMachineRoot;  // removed too when it is another folder
  std::filesystem::path trashParent;        // empty: the folder that holds |machineRoot|
  std::wstring taskFolder = kTaskFolder;    // empty: no task is deleted (tests)
  std::wstring taskRootPrefix = kTaskRootPrefix;
  bool legacyTask = true;  // also 1.x's task
  // Where the user hives are loaded: HKEY_USERS; tests pass a private key with hives of their
  // own. nullptr: no hive is touched.
  HKEY users = HKEY_USERS;
  DWORD retryMs = kMachineRemoveRetryMs;
};

struct MachineUninstallResult {
  bool classDeleted = false;
  HRESULT tasks = S_FALSE;
  HRESULT legacyTask = S_FALSE;
  size_t hivesCleaned = 0;  // hives in which something was removed
  // Hives in which a key was left as it was (kRegistryTreeLeftHr: a link, or a tree past the
  // bounds). Also listed in |failures| as "hive left".
  size_t hivesLeft = 0;
  RemoveOutcome root;
  RemoveOutcome legacyRoot;
  bool restartPending = false;
  std::vector<std::wstring> failures;
  HRESULT firstError = S_OK;
};

// The step for all users of --uninstall. Goes on after a failure; S_OK with the first one in
// |result->firstError|. Fails only for invalid options or when out of memory.
[[nodiscard]] HRESULT UninstallForAllUsers(const MachineUninstallOptions& options,
                                           const RegistryRoots& roots,
                                           MachineUninstallResult* result) noexcept;

// The elevated process's step machine-uninstall, against the real registry and folders.
[[nodiscard]] HRESULT RunMachineUninstall(ElevatedReporter& report, bool* restartPending) noexcept;

// ---- --uninstall ----------------------------------------------------------------------------

// Starts |exe| with |arguments| (the installer's uninstaller, which asks for administrator
// rights itself), waits for it and returns its exit code. Injectable: tests pass a fake.
using ProgramRunner = std::function<HRESULT(const std::filesystem::path& exe,
                                            const std::wstring& arguments, DWORD* exitCode)>;

// The real runner: ShellExecuteExW "open" (so the program's own manifest decides about
// elevation), no error UI, waits for it.
[[nodiscard]] HRESULT RunProgramAndWait(const std::filesystem::path& exe,
                                        const std::wstring& arguments, DWORD* exitCode) noexcept;

enum class UninstallRefusal {
  kNone,
  kWrongContext,  // 8
  kElevated,      // 8
  kNotInstalled,  // 6
};

struct UninstallOptions {
  bool yes = false;  // --yes: the installer's uninstaller runs silently
  bool rebuildThumbnails = false;
  std::optional<ProcessContext> context;  // the canary's; when absent, probes' kWrite context
  std::optional<EnginePaths> paths;
  std::filesystem::path dataDir;  // empty: GetDataDir
  DWORD lockWaitMs = kCheckLockWaitMs;
  HWND owner = nullptr;
  ElevationLauncher launcher;  // the administrator step; tests pass a fake
  ProgramRunner runner;        // the installer's uninstaller; tests pass a fake
};

struct UninstallResult {
  ExitCode code = ExitCode::kFailed;
  UninstallRefusal refusal = UninstallRefusal::kNone;
  bool busy = false;
  // exit 5: Windows does not let this process make the lock (kNamedObjectsRefusedHr)
  bool namedObjectsRefused = false;
  std::optional<InnoInstall> inno;  // the installer's install: its uninstaller ran instead
  HRESULT innoRun = S_OK;
  DWORD innoExit = 0;
  UserRemoval user;
  bool machinePart = false;       // the part for all users was there
  bool machineNoProgram = false;  // ... but no settings executable for all users to remove it
  std::optional<ElevatedRun> run;
  bool machineLeft = false;
  UserFilesRemoval files;
  bool rebuildNotAvailable = false;
  std::vector<std::wstring> failures;
};

// Whether anything of the product is there for the user of |snapshot| (pure): a class, an
// install folder for all users, this version's files or build folders in the per-user folder, a
// slot that names the handler, the check task, the product key (|settingsKey|) or the data
// folder (|dataDir|).
bool AnythingToUninstall(const Snapshot& snapshot, bool settingsKey, bool machineRootExists,
                         bool userFilesExist, bool dataDir) noexcept;

// Runs --uninstall. Never throws.
[[nodiscard]] HRESULT Uninstall(const UninstallOptions& options, const RegistryRoots& roots,
                                const SystemProbes& probes, UninstallResult* result) noexcept;

// ---- --uninstall-cleanup --------------------------------------------------------------------

struct UninstallCleanupOptions {
  std::optional<EnginePaths> paths;
  std::filesystem::path dataDir;    // empty: GetDataDir
  MachineUninstallOptions machine;  // its roots filled in from |paths| when empty
  bool runMachinePart = true;       // only ever when elevated
};

struct UninstallCleanupResult {
  bool userPart = false;     // the registry check allowed it
  bool machinePart = false;  // elevated: it ran
  UserRemoval user;
  MachineUninstallResult machine;
  std::vector<std::filesystem::path> scheduled;
  std::vector<std::wstring> failures;
};

// Runs --uninstall-cleanup: whatever it can, always to the end. Never throws; the command exits
// 0 whatever this returns.
[[nodiscard]] HRESULT UninstallCleanup(const UninstallCleanupOptions& options,
                                       const RegistryRoots& roots, const SystemProbes& probes,
                                       UninstallCleanupResult* result) noexcept;

}  // namespace pdft
