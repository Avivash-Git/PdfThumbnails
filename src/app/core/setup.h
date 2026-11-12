// The installer's steps and the 1.x clean-up as commands: --setup-user, --setup-machine and
// --migrate-v1.
//
// --setup-user (the installer's per-user step, also the end of a developer install), quiet, fast
// (no Explorer test) and safe to repeat:
//   refused where the registry may be virtualized (8) and in an elevated process (8: everything
//   here is the signed-in user's own); the operation lock (4; 5 when Windows does not let this
//   process make it); an installed program to set up for (6); then
//   1. the sign-in clean-up values (RunOnce) go: 1.x's would delete the per-user folder;
//   2. the zoom: --zoom if given; else the value there is kept; else 1.x's settings.ini value;
//      else 100;
//   3. both .pdf slots point at the handler, also when another handler had them;
//   4. a per-user class goes while the install is machine-wide (it would hide the machine-wide
//      one), but not a 2.x one over a machine-wide 1.x class (a per-user install forced over it);
//   5. the check task: the triggers --checks asks for; else the ones the user's task has; else
//      both (1.x's task is replaced the same way); never over another account's task, and when
//      such tasks hold every name the task may have, the failure says so and that an
//      administrator must remove them;
//   6. this session's thumbnail processes that hold an old build's DLLs are ended (only Windows'
//      own dllhost.exe of this user), so the new build loads at the next thumbnail;
//   7. the user part of the 1.x clean-up (migrate_v1.h), with the task already registered;
//   8. Explorer is told that associations changed.
//   Exit 0, or 5 when a step failed (the others still ran).
//
// --setup-machine [--build <folder>] (the installer's step for all users; run elevated): refused
// without the administrator rights the elevated side requires of its own token (7: elevated, the
// Administrators group enabled, an integrity level of High; ReadAdministratorRights), and below
// an app package (or where the processes above it cannot be read) unless test values it writes
// under HKEY_LOCAL_MACHINE\Software\Classes and in its own user's registry come back through WMI
// (8, context.h); no operation lock (see SetupMachine); then
//   1. with --build, the class for all users, pointed at <install folder>\app\<folder>: the
//      build the installer just put there (FindBuildToRegister checks it). The installer names
//      it because two builds of one version cannot be told apart by their names. When this
//      fails, the old builds stay (the class may still need one of them);
//   2. the shared task folder (EnsureTaskFolder), the machine part of the 1.x clean-up and the
//      old 2.x builds (every build folder but the one the class names).
// Exit 0, 1 when files wait for a restart, 5 when a step failed.
//
// --migrate-v1 [--user-only | --machine-only]: refused like --setup-user; needs an installed
// program (6), and leaves 1.x as it is without one. The user part runs here; the machine part, when
// 1.x's build folders for all users are left, through the administrator step machine-migrate-v1
// of the settings executable installed for all users (a prompt; declined: 3); a part of the step
// for all users that failed is listed as "machine <part>: <HRESULT>". Exit 0, 1 when something
// waits (for a restart, for a process to let go, or for an install for all users), 5 when a step
// failed, 7 when nothing else failed but the administrator step did not get administrator
// rights, 8 when nothing else failed but it refused to run below an app package.

#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check.h"
#include "cli_args.h"
#include "context.h"
#include "elevation.h"
#include "exit_code.h"
#include "migrate_v1.h"
#include "registration.h"
#include "registry_roots.h"
#include "snapshot.h"
#include "system_probes.h"
#include "tasks.h"

namespace pdft {

// Why a step was refused before anything was read.
enum class SetupRefusal {
  kNone,
  kWrongContext,  // 8: the registry may be virtualized, or the context is not the canary's
  kElevated,      // 8: the per-user steps refuse an elevated process
  kNotElevated,   // 7: --setup-machine needs administrator rights (ReadAdministratorRights)
  // 8: --setup-machine below an app package (or with the processes above it unreadable) where
  // its test writes did not come back
  kPackaged,
  kNotInstalled,  // 6
};

// ---- --setup-user ---------------------------------------------------------------------------

struct SetupUserOptions {
  std::optional<ChecksSelection> checks;  // --checks
  std::optional<int> zoom;                // --zoom
  // The context the command read with the canary; when absent, probes.GetProcessContext with
  // ContextProbe::kWrite.
  std::optional<ProcessContext> context;
  std::optional<EnginePaths> paths;  // when absent, GetEnginePaths
  DWORD lockWaitMs = kCheckLockWaitMs;
  bool takeLock = true;  // false when the caller holds the operation lock already
};

struct SetupUserResult {
  ExitCode code = ExitCode::kFailed;
  SetupRefusal refusal = SetupRefusal::kNone;
  bool busy = false;
  // exit 5: Windows does not let this process make the lock (kNamedObjectsRefusedHr)
  bool namedObjectsRefused = false;
  Snapshot before;
  size_t runOnceDeleted = 0;
  std::optional<int> zoomWritten;
  std::array<bool, kSlotCount> slotsWritten = {};
  bool userClassDeleted = false;
  bool taskRegistered = false;
  // ... could not be: another account's tasks hold every name it may have (kTaskTakenHr)
  bool taskTaken = false;
  TaskOptions task;  // what it was registered with
  size_t holdersEnded = 0;
  V1UserResult migration;
  bool notified = false;
  // "<step>: <HRESULT>", no paths; for names another account's tasks hold, why in words, the
  // HRESULT and the way out, which names those tasks
  std::vector<std::wstring> failures;
};

// The task options --setup-user registers (pure): the --checks selection when given; else the
// triggers and state of the user's |current| task when it has a trigger; else both triggers.
TaskOptions SetupTaskOptions(const std::optional<ChecksSelection>& checks,
                             const TaskState& current) noexcept;

// The old build DLLs whose holders --setup-user ends (pure): both DLLs of every build folder of
// |snapshot| (per user, for all users and 1.x's) except the one the class in effect now
// (|activeDll|, possibly empty) lies in. May throw std::bad_alloc.
std::vector<std::filesystem::path> OldBuildDlls(const Snapshot& snapshot,
                                                const std::wstring& activeDll);

// Runs --setup-user. S_OK when it came to a verdict (*result's code); a failure when the state
// could not be read. Never throws.
[[nodiscard]] HRESULT SetupUser(const SetupUserOptions& options, const RegistryRoots& roots,
                                const SystemProbes& probes, SetupUserResult* result) noexcept;

// ---- --setup-machine ------------------------------------------------------------------------

struct SetupMachineOptions {
  std::optional<EnginePaths> paths;  // when absent, GetEnginePaths
  // --build: the build folder's name; empty leaves the class for all users as it is.
  std::wstring build;
  // What the build's DLL must be made for; Arch::kOther: this Windows' own architecture.
  Arch osArch = Arch::kOther;
  std::wstring taskFolder = kTaskFolder;
  // Where in-use files go; when empty, the folder that holds the install folder.
  std::filesystem::path trashParent;
  // Whether a package is above it and what its test writes showed; when absent,
  // CheckMachineWrites. Tests pass one.
  std::optional<MachineWriteCheck> machineWrites;
};

struct SetupMachineResult {
  ExitCode code = ExitCode::kFailed;
  SetupRefusal refusal = SetupRefusal::kNone;
  MachineWriteCheck machineWrites;  // what it judged its context by (with kPackaged, why)
  // The class for all users: S_FALSE without --build, S_OK once written, else why not (then the
  // old builds were left as they were).
  HRESULT buildClass = S_FALSE;
  HRESULT taskFolder = S_OK;  // EnsureTaskFolder's answer
  MachineRemoval v1;          // 1.x's build folders
  MachineRemoval old;         // old 2.x builds
  std::vector<std::wstring> failures;
};

// The handler DLL of the build folder <machineRoot>\app\<name> that --setup-machine --build
// registers: |name| must be a build folder name of version 2 or later
// (E_INVALIDARG otherwise); the folder must be a real folder, not a link
// (HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) when it is missing); both DLLs must be files in it
// (HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)); the handler DLL must be made for |osArch|
// (HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT)); and the name computed from the two DLLs' bytes
// (ComputeBuildId) must be |name| (HRESULT_FROM_WIN32(ERROR_INVALID_DATA)), so the folder holds
// the build it is named after. Never throws.
[[nodiscard]] HRESULT FindBuildToRegister(const std::filesystem::path& machineRoot,
                                          std::wstring_view name, Arch osArch,
                                          std::filesystem::path* dll) noexcept;

// Runs --setup-machine. Never throws.
[[nodiscard]] HRESULT SetupMachine(const SetupMachineOptions& options, const RegistryRoots& roots,
                                   SetupMachineResult* result) noexcept;

// ---- --migrate-v1 ---------------------------------------------------------------------------

struct MigrateOptions {
  MigrateScope scope = MigrateScope::kBoth;
  std::optional<ProcessContext> context;
  std::optional<EnginePaths> paths;
  DWORD lockWaitMs = kCheckLockWaitMs;
  HWND owner = nullptr;
  // Starts the administrator step; tests pass a fake, so no test ever starts a real elevation.
  ElevationLauncher launcher;
};

struct MigrateResult {
  ExitCode code = ExitCode::kFailed;
  SetupRefusal refusal = SetupRefusal::kNone;
  bool busy = false;
  // exit 5: Windows does not let this process make the lock (kNamedObjectsRefusedHr)
  bool namedObjectsRefused = false;
  Snapshot before;
  bool userPart = false;  // ran
  V1UserResult user;
  bool machineNeeded = false;      // 1.x's build folders for all users are left
  bool machineNoInstall = false;   // ... but nothing is installed for all users to remove them
  std::optional<ElevatedRun> run;  // the administrator step, when it was started
  std::vector<std::wstring> failures;
};

// Runs --migrate-v1. Never throws.
[[nodiscard]] HRESULT MigrateV1(const MigrateOptions& options, const RegistryRoots& roots,
                                const SystemProbes& probes, MigrateResult* result) noexcept;

}  // namespace pdft
