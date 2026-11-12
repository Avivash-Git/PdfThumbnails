// Removing the product: --uninstall, the uninstaller's clean-up step (--uninstall-cleanup) and
// the administrator step machine-uninstall.
//
// --uninstall [--yes] [--rebuild-thumbnails], unelevated:
//   refused where the registry may be virtualized (8) and in an elevated process (8: the
//   per-user part must run as the signed-in user); while the installer (Inno Setup) has an
//   install here, its own uninstaller runs instead, so its records stay right (UninstallByInstaller
//   in uninstall.cpp): silently with --yes, from the system folder, waited for at most 30 minutes
//   (10 with --yes; Ctrl+C stops waiting, and it goes on by itself); then its record and the
//   handler's registration are looked at again, and with exit code 0 and both gone this user's
//   own part is removed too. With --rebuild-thumbnails and a check that allows the rebuild, the
//   operation lock is taken before the uninstaller starts (4 when busy), and the thumbnail cache
//   is rebuilt once nothing of the handler is left. Exit codes: InstallerUninstallExitCode.
//   Otherwise: the operation lock (4; 5 when Windows does not
//   let this process make it); nothing of the product here: 6; stopped (Ctrl+C) before anything
//   is removed: 3, nothing changed. Then
//   1. the per-user part: the .pdf slots that name the handler (and the keys above them left
//      empty), the per-user class, the check task and 1.x's, HKCU\<product key>; Explorer is told;
//   2. with --rebuild-thumbnails, the thumbnail cache, now that File Explorer no longer finds the
//      handler for this user: rebuilt as --refresh --full rebuilds it (rebuild.h), which restarts
//      File Explorer. This process holds the lock and never ran the Explorer test, so it does not
//      hold the cache itself. A rebuild that is refused (also by the check the command made
//      before its question), stopped before its point of no return or cannot start changed
//      nothing: the report says why and the uninstall goes on. One after which File Explorer did
//      not come back, or a cache file could not be deleted, is a step that failed ("rebuild:
//      <HRESULT>");
//   3. the part for all users, when the class for all users or the install folder for all users
//      is there: the administrator step machine-uninstall of the settings executable installed
//      there (a prompt; declined: the part stays, exit 1);
//   4. the user's files: this version's files in the per-user folder, its app folder, and the
//      app's data folder (the log). What is in use stays and is removed at the next sign-in
//      (a RunOnce value per folder, file_ops.h).
//   Exit 0; 1 when the part for all users is left; 5 when a step failed; 7 when nothing else
//   failed but the administrator step did not get administrator rights (the part stays); 8 when
//   nothing else failed but the administrator step refused to run below an app package (the part
//   stays).
//
// machine-uninstall (elevated): the class for all users; every user's check task (the shared task
// folder) and 1.x's task; the install folder for all users, files in use (its own executable
// among them) moved into a trash folder and deleted at the next restart (while the installer has
// an install for all users, its uninstaller's own files and the folder are left to it); then, in
// every user hive that is loaded (HKEY_USERS\S-1-5-21-..., and its _Classes hive), the slots that
// name the handler, the handler's class key and the product key. Those keys are deleted without
// following any link and within DeleteRegistryTree's bounds (registry_roots.h): a link or a tree
// past them is left as it is and reported ("left"). Each part that fails is reported as not done
// and the step goes on; it exits with the first one's HRESULT. Users who are not signed in keep
// their slots and settings; a slot naming a class that is gone shows no thumbnail and hides only
// other handlers registered for .pdf for all users.
//
// --uninstall-cleanup (the installer's uninstall step): the machine-uninstall steps when it runs
// elevated with the administrator rights the elevated side requires of its own token
// (ReadAdministratorRights; below an app package, or where the processes above it cannot be read,
// only when test values it writes under HKEY_LOCAL_MACHINE\Software\Classes and in its own user's
// registry come back through WMI, context.h), the per-user registry part when the registry check
// shows that writes reach the real hive, and a RunOnce value for the app's data folder (an
// elevated process never deletes in the user's profile). Never asks anything, and always exits 0;
// what it left out or could not do goes to its error output (UninstallCleanupErrors). Elevated,
// the per-user part gathers nothing first: it deletes fixed key names only, and never reads a path
// from the user's registry to open it.

#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
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
#include "rebuild.h"
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
  // The installer has an install for all users: its own files in the install folder were left
  // to its uninstaller (IsInnoUninstallerFileName), and so was the folder.
  bool installerFiles = false;
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
// |notDone| receives MachineUninstallResult::failures.
[[nodiscard]] HRESULT RunMachineUninstall(ElevatedReporter& report, bool* restartPending,
                                          std::vector<std::wstring>* notDone) noexcept;

// ---- --uninstall ----------------------------------------------------------------------------

// How long --uninstall waits for the installer's uninstaller (its administrator prompt and its
// work); then it stops waiting, and the uninstaller goes on by itself.
inline constexpr DWORD kInstallerWaitMs = 30 * 60 * 1000;     // when it may ask
inline constexpr DWORD kInstallerWaitYesMs = 10 * 60 * 1000;  // --yes (silent)
DWORD InstallerWaitMs(bool yes) noexcept;                     // pure

// What a ProgramRunner gives when it stopped waiting; the program goes on either way.
inline constexpr HRESULT kStoppedWaitingHr = HRESULT_FROM_WIN32(ERROR_OPERATION_ABORTED);
inline constexpr HRESULT kWaitLimitHr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
struct ProgramWait {
  std::stop_token stop;      // a stop request ends the wait: kStoppedWaitingHr
  DWORD limitMs = INFINITE;  // then the wait ends: kWaitLimitHr
};

// Starts |exe| with |arguments| (the installer's uninstaller, which asks for administrator
// rights itself), waits as |wait| says and returns its exit code; the program is never ended.
// Injectable: tests pass a fake.
using ProgramRunner =
    std::function<HRESULT(const std::filesystem::path& exe, const std::wstring& arguments,
                          const ProgramWait& wait, DWORD* exitCode)>;

// The real runner: ShellExecuteExW "open" (so the program's own manifest decides about
// elevation), from the system folder, no error UI, a console program sharing this console.
[[nodiscard]] HRESULT RunProgramAndWait(const std::filesystem::path& exe,
                                        const std::wstring& arguments, const ProgramWait& wait,
                                        DWORD* exitCode) noexcept;

// What of an install can be seen without changing anything: read before and after the
// installer's uninstaller.
struct InstallTraces {
  bool read = false;          // all of it could be read
  bool machineClass = false;  // the class for all users
  bool userClass = false;     // this user's class
  bool slotOurs = false;      // one of this user's .pdf slots names the handler
  bool folder = false;        // the install folder of the installer's scope
};
[[nodiscard]] HRESULT ReadInstallTraces(const RegistryRoots& roots,
                                        const std::filesystem::path& folder,
                                        InstallTraces* traces) noexcept;
// Whether anything |before| had is gone in |after| (pure); true when either was not read.
bool TracesChanged(const InstallTraces& before, const InstallTraces& after) noexcept;
// What is still registered (pure): kMachine (the class for all users), kUser (this user's class
// or a slot naming the handler), or kNone. The folder does not count.
Scope HandlerLeft(const InstallTraces& traces) noexcept;

// How the installer's uninstaller ended, as --uninstall judges it.
enum class InstallerOutcome {
  kNotRun,          // there was no installer's install
  kCancelled,       // it did not start: cancelled (exit 3)
  kNotStarted,      // it could not be started (5)
  kStoppedWaiting,  // Ctrl+C while waiting; it goes on by itself (1)
  kTimedOut,        // still running at the limit; it goes on by itself (1)
  kNothingRemoved,  // exit code not 0, its record there and nothing of it changed (3)
  kStoppedPartWay,  // exit code not 0, and its record gone or something changed (5)
  kRecordLeft,      // exit code 0, but its record is still there (1)
  kHandlerLeft,     // exit code 0, but a class or slot still names the handler (1)
  kRemoved,         // exit code 0, its record gone, nothing of the handler registered
};
// The outcome (pure) from the runner's HRESULT, the exit code, whether the record is still there,
// whether anything changed, and what is still registered.
InstallerOutcome JudgeInstallerRun(HRESULT run, DWORD exitCode, bool recordLeft, bool changed,
                                   Scope handlerLeft) noexcept;
// The exit code of an uninstall by the installer's uninstaller (pure); |failures|: a step after
// it failed (this user's part, a check, the rebuild).
ExitCode InstallerUninstallExitCode(InstallerOutcome outcome, bool failures) noexcept;

enum class UninstallRefusal {
  kNone,
  kWrongContext,  // 8
  kElevated,      // 8
  kNotInstalled,  // 6
  kCancelled,     // 3: stopped before anything was removed
};

struct UninstallOptions {
  bool yes = false;  // --yes: the installer's uninstaller runs silently
  bool rebuildThumbnails = false;
  // The check the command made before its question (PrecheckRebuild). When it refused or failed,
  // no rebuild is tried and that is the reason the report gives; a later check may only refuse a
  // rebuild it allowed. Not given: Uninstall leaves the decision to the rebuild.
  std::optional<RebuildPrecheck> rebuildPrecheck;
  // What the rebuild asks Windows; null: RealRebuildSystem. Tests pass a fake.
  const RebuildSystem* rebuildSystem = nullptr;
  // The rebuild's progress (optional; RebuildThumbnailCache).
  RebuildProgressFn rebuildProgress;
  // Ctrl+C: before step 1 the uninstall stops (kCancelled); later it only keeps the rebuild from
  // starting, until the rebuild's point of no return, and the uninstall goes on.
  std::stop_token stop;
  std::optional<ProcessContext> context;  // the canary's; when absent, probes' kWrite context
  std::optional<EnginePaths> paths;
  std::filesystem::path dataDir;  // empty: GetDataDir
  DWORD lockWaitMs = kCheckLockWaitMs;
  HWND owner = nullptr;
  ElevationLauncher launcher;  // the administrator step; tests pass a fake
  ProgramRunner runner;        // the installer's uninstaller; tests pass a fake
  // Called once just before the installer's uninstaller starts; |rebuildAfter| when the cache is
  // rebuilt once it has finished. The command leaves the install folder as its current folder
  // and says that it waits there.
  std::function<void(bool rebuildAfter)> beforeUninstaller;
  DWORD installerWaitMs = 0;  // 0: InstallerWaitMs(yes)
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
  InstallerOutcome innoOutcome = InstallerOutcome::kNotRun;
  DWORD innoLimitMs = 0;
  ULONGLONG innoWaitMs = 0;
  bool innoRecordLeft = false;
  Scope handlerLeft = Scope::kNone;  // after an exit code of 0
  // --rebuild-thumbnails with the installer's install: the rebuild was to follow the uninstaller
  // (its check allowed it); when it did not, innoOutcome says why.
  bool rebuildPlanned = false;
  UserRemoval user;
  bool machinePart = false;       // the part for all users was there
  bool machineNoProgram = false;  // ... but no settings executable for all users to remove it
  std::optional<ElevatedRun> run;
  bool machineLeft = false;
  UserFilesRemoval files;
  // --rebuild-thumbnails: what the rebuild did, a refusal or a failure before it started included
  std::optional<RebuildResult> rebuild;
  // "<step>: <HRESULT>", and "machine <part>: <HRESULT>" for each part of the step for all users
  // that failed
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
  // Whether a package is above it and what its test writes showed; when absent,
  // CheckMachineWrites. Tests pass one.
  std::optional<MachineWriteCheck> machineWrites;
};

struct UninstallCleanupResult {
  bool userPart = false;     // the registry check allowed it
  bool machinePart = false;  // elevated: it ran
  // Elevated, but its token lacks the administrator rights the elevated side requires
  // (ReadAdministratorRights said no): the part for all users was left out ("machine:
  // <kNoAdministratorRightsHr>").
  bool machineNoRights = false;
  // Elevated, but below an app package (or with the processes above it unreadable) where its
  // test writes did not come back: the part for all users was left out ("machine: <HRESULT>").
  bool machineRefused = false;
  MachineWriteCheck machineWrites;  // what it judged that by (elevated only)
  // The per-user part's registry check (GetProcessContext's canary), which decides userPart: its
  // verdict and the failure behind an "unknown".
  Virtualization userWrites = Virtualization::kUnknown;
  HRESULT userProbeError = S_OK;
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
