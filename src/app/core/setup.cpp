#include "setup.h"

#include <wil/result_macros.h>

#include <utility>

#include "file_ops.h"
#include "instance.h"
#include "log.h"
#include "project_config.h"
#include "settings.h"
#include "status_format.h"
#include "strings.h"
#include "zoom.h"

namespace pdft {
namespace {

void Failed(std::vector<std::wstring>* failures, const wchar_t* step, HRESULT hr) {
  failures->push_back(std::wstring(step) + L": " + HResultText(hr));
}

bool FileExists(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

// The context for a per-user step: the caller's, else the canary's. *refusal is set when the
// step may not run in it.
HRESULT UserContext(const std::optional<ProcessContext>& given, const SystemProbes& probes,
                    ProcessContext* context, SetupRefusal* refusal) noexcept try {
  *refusal = SetupRefusal::kNone;
  if (given) {
    *context = *given;
  } else {
    RETURN_IF_FAILED(probes.GetProcessContext(ContextProbe::kWrite, context));
  }
  // Only the canary says whether writes reach the real hive.
  if (context->probe != ContextProbe::kWrite || RefusesWrites(context->virtualization)) {
    *refusal = SetupRefusal::kWrongContext;
  } else if (context->elevated) {
    // Everything here is the signed-in user's own, in their profile: an administrator process
    // may not even be that user.
    *refusal = SetupRefusal::kElevated;
  }
  return S_OK;
}
CATCH_RETURN();

ExitCode RefusalCode(SetupRefusal refusal) noexcept {
  switch (refusal) {
    case SetupRefusal::kNotElevated:
      return ExitCode::kNeedsAdmin;
    case SetupRefusal::kNotInstalled:
      return ExitCode::kNotInstalled;
    case SetupRefusal::kNone:
      return ExitCode::kOk;
    default:
      return ExitCode::kWrongContext;
  }
}

// Takes the operation lock for |command| ("setup-user", the log's word). S_OK also when it did
// not: *busy when another process keeps it, *refused when Windows does not let this process make
// it (logged). May throw std::bad_alloc.
HRESULT TakeLock(const wchar_t* command, DWORD waitMs, std::optional<OperationLock>* lock,
                 bool* busy, bool* refused) {
  *busy = false;
  *refused = false;
  const HRESULT hr = OperationLock::Acquire(EffectiveLockWaitMs(waitMs), lock);
  if (hr == OperationLock::kBusy || hr == OperationLock::kTakenByLowerIntegrity) {
    *busy = true;
    return S_OK;
  }
  if (hr == kNamedObjectsRefusedHr) {
    *refused = true;
    Log(LogLevel::kWarning, command,
        std::wstring(command) + L" result=failed named-objects-refused hr=" + HResultText(hr));
    return S_OK;
  }
  if (SUCCEEDED(hr) && lock->has_value() && (*lock)->sessionOnly()) {
    Log(LogLevel::kWarning, command, std::wstring(command) + L" lock=session-only");
  }
  return hr;
}

void LogInfo(const wchar_t* component, const std::wstring& message) noexcept {
  Log(LogLevel::kInfo, component, message);
}

}  // namespace

// ---- --setup-user ---------------------------------------------------------------------------

TaskOptions SetupTaskOptions(const std::optional<ChecksSelection>& checks,
                             const TaskState& current) noexcept {
  if (checks) return OptionsFromCheckboxes(checks->afterPdfCraft, checks->atSignIn);
  if (current.exists && !HasNoTriggers(current.options)) return current.options;
  return TaskOptions{};
}

std::vector<std::filesystem::path> OldBuildDlls(const Snapshot& snapshot,
                                                const std::wstring& activeDll) {
  std::vector<std::filesystem::path> dlls;
  for (const std::vector<BuildInfo>* builds : {&snapshot.userBuilds, &snapshot.machineBuilds}) {
    for (const BuildInfo& build : *builds) {
      if (!activeDll.empty() && DllIsInFolder(activeDll, build.folder.dir)) continue;
      dlls.push_back(build.folder.dir / kHandlerDll);
      dlls.push_back(build.folder.dir / kPdfiumDll);
    }
  }
  return dlls;
}

HRESULT SetupUser(const SetupUserOptions& options, const RegistryRoots& roots,
                  const SystemProbes& probes, SetupUserResult* result) noexcept try {
  *result = SetupUserResult{};
  ProcessContext context;
  RETURN_IF_FAILED(UserContext(options.context, probes, &context, &result->refusal));
  if (result->refusal != SetupRefusal::kNone) {
    result->code = RefusalCode(result->refusal);
    return S_OK;
  }
  std::optional<OperationLock> lock;
  if (options.takeLock) {
    RETURN_IF_FAILED(TakeLock(L"setup-user", options.lockWaitMs, &lock, &result->busy,
                              &result->namedObjectsRefused));
    if (result->busy || result->namedObjectsRefused) {
      result->code = result->busy ? ExitCode::kBusy : ExitCode::kFailed;
      return S_OK;
    }
  }

  GatherOptions gather;
  gather.liveTest = false;  // quiet and fast: it runs at every sign-in after an update
  gather.context = context;
  gather.paths = options.paths;
  RETURN_IF_FAILED(GatherSnapshot(roots, gather, probes, &result->before));
  const Snapshot& before = result->before;
  const std::optional<std::filesystem::path> program = InstalledSettingsExe(before);
  if (!program) {
    result->refusal = SetupRefusal::kNotInstalled;
    result->code = ExitCode::kNotInstalled;
    return S_OK;
  }
  std::vector<std::wstring>& failures = result->failures;

  // 1. The sign-in clean-up first: 1.x's would delete the per-user folder.
  if (const HRESULT hr = DeleteRunOnceRemovals(roots, &result->runOnceDeleted); FAILED(hr)) {
    Failed(&failures, L"runonce", hr);
  }

  // 2. The zoom.
  {
    std::optional<int> zoom = options.zoom;
    HRESULT hr = S_OK;
    if (!zoom) {
      std::optional<RegistryValue> stored;
      hr = ReadRegistryValue(roots.user, kRegKey, kZoomValueName, &stored);
      if (SUCCEEDED(hr) && !stored) {
        std::optional<int> iniZoom;
        if (!before.paths.legacyIni.empty())
          hr = ReadLegacyIniZoom(before.paths.legacyIni, &iniZoom);
        zoom = iniZoom.value_or(kMinZoom);
      }
    }
    if (SUCCEEDED(hr) && zoom) {
      hr = SaveZoom(roots, *zoom);
      if (SUCCEEDED(hr)) result->zoomWritten = ClampZoom(*zoom);
    }
    if (FAILED(hr)) Failed(&failures, L"zoom", hr);
  }

  // 3. Both slots, taken back from another handler: installing is the user's choice.
  if (const HRESULT hr = WriteSlots(roots, SlotWrite::kTakeBack, &result->slotsWritten);
      FAILED(hr)) {
    Failed(&failures, L"slots", hr);
  }

  // 4. A per-user class hides the machine-wide one: it goes while the machine-wide one is there,
  // unless that is 1.x's and the per-user one is not (a per-user install forced over 1.x's install
  // for all users; a per-user 1.x class is the clean-up's below).
  {
    std::optional<ClassReg> machine;
    std::optional<ClassReg> user;
    HRESULT hr = ReadClass(roots, Scope::kMachine, &machine);
    if (SUCCEEDED(hr) && machine) hr = ReadClass(roots, Scope::kUser, &user);
    const bool forcedOverV1 = machine && user &&
                              IsInV1BuildFolder(machine->dll, before.paths.legacyMachineRoot) &&
                              !IsInV1BuildFolder(user->dll, before.paths.legacyUserRoot);
    if (SUCCEEDED(hr) && machine && user && !forcedOverV1) {
      hr = DeleteClass(roots, Scope::kUser);
      result->userClassDeleted = SUCCEEDED(hr);
    }
    if (FAILED(hr)) Failed(&failures, L"user-class", hr);
  }

  // 5. The check task.
  if (!before.taskLocation.name.empty()) {
    TaskState current;
    HRESULT hr = probes.ReadTask(before.taskLocation, &current);
    if (FAILED(hr)) current = TaskState{};  // registering replaces what could not be read
    result->task = SetupTaskOptions(options.checks, current);
    hr = probes.ApplyTaskOptions(before.taskLocation, result->task, *program, context);
    result->taskRegistered = SUCCEEDED(hr) && !HasNoTriggers(result->task);
    result->taskTaken = hr == kTaskTakenHr;
    if (result->taskTaken) {
      // Only an administrator can free the names: the line says so, and which tasks to remove.
      failures.push_back(L"task: " + TaskTakenText() + L" (" + HResultText(hr) + L"). " +
                         TaskTakenHint(before.taskLocation));
    } else if (FAILED(hr)) {
      Failed(&failures, L"task", hr);
    }
  }

  // 6. Thumbnail processes that still hold an old build, so the new one loads.
  {
    std::optional<ClassReg> machine;
    std::optional<ClassReg> user;
    HRESULT hr = ReadClass(roots, Scope::kMachine, &machine);
    if (SUCCEEDED(hr)) hr = ReadClass(roots, Scope::kUser, &user);
    if (SUCCEEDED(hr)) {
      const std::wstring active = machine ? machine->dll : user ? user->dll : std::wstring();
      std::vector<std::filesystem::path> files;
      for (std::filesystem::path& dll : OldBuildDlls(before, active)) {
        if (FileExists(dll)) files.push_back(std::move(dll));
      }
      std::vector<Holder> holders;
      if (!files.empty()) hr = probes.FindHolders(files, &holders);
      for (const Holder& holder : holders) {
        if (probes.EndHolder(holder) == S_OK) ++result->holdersEnded;
      }
    }
    if (FAILED(hr)) Failed(&failures, L"holders", hr);
  }

  // 7. The user part of the 1.x clean-up; the task and the zoom are settled above.
  V1UserOptions migrate;
  migrate.registerTask = false;
  migrate.keepZoom = true;
  if (const HRESULT hr = MigrateV1User(roots, probes, before, migrate, &result->migration);
      FAILED(hr)) {
    Failed(&failures, L"migrate-v1", hr);
  }
  failures.insert(failures.end(), result->migration.failures.begin(),
                  result->migration.failures.end());

  // 8. Explorer reads associations once and keeps them.
  result->notified = NotifyAssocChanged(roots);

  result->code = failures.empty() ? ExitCode::kOk : ExitCode::kFailed;
  LogInfo(L"setup-user", L"setup-user result=" + std::wstring(CheckResultName(result->code)) +
                             L" runonce=" + std::to_wstring(result->runOnceDeleted) + L" task=" +
                             (result->taskRegistered ? L"yes"
                              : result->taskTaken    ? L"taken"
                                                     : L"no") +
                             L" holders-ended=" + std::to_wstring(result->holdersEnded) +
                             L" failures=" + std::to_wstring(failures.size()));
  return S_OK;
}
CATCH_RETURN();

// ---- --setup-machine ------------------------------------------------------------------------

HRESULT SetupMachine(const SetupMachineOptions& options, const RegistryRoots& roots,
                     SetupMachineResult* result) noexcept try {
  *result = SetupMachineResult{};
  // The rights the elevated side requires of its own token, first: an elevated token alone does
  // not prove them.
  bool rights = false;
  RETURN_IF_FAILED(ReadAdministratorRights(&rights));
  if (!rights) {
    result->refusal = SetupRefusal::kNotElevated;
    result->code = ExitCode::kNeedsAdmin;
    return S_OK;
  }
  // Below an app package, or where the processes above it cannot be read, files and the registry
  // may be private copies: the step goes on only where its own test writes reach Windows.
  if (options.machineWrites) {
    result->machineWrites = *options.machineWrites;
  } else {
    CheckMachineWrites(&result->machineWrites);
  }
  if (RefusesMachineWrites(result->machineWrites)) {
    result->refusal = SetupRefusal::kPackaged;
    result->code = ExitCode::kWrongContext;
    return S_OK;
  }
  // No operation lock: it is named after one user, so it would not keep out another
  // administrator's steps anyway, and any account may make its name first. In session 0 (a
  // deployment tool running this as SYSTEM) nothing else is left to take, so that account could
  // stop the step. The installer runs one at a time by itself.
  EnginePaths paths;
  if (options.paths) {
    paths = *options.paths;
  } else {
    RETURN_IF_FAILED(GetEnginePaths(&paths));
  }
  std::vector<std::wstring>& failures = result->failures;

  result->taskFolder = EnsureTaskFolder(options.taskFolder);
  if (FAILED(result->taskFolder)) Failed(&failures, L"task-folder", result->taskFolder);

  const std::filesystem::path legacyTrash =
      options.trashParent.empty() ? paths.legacyMachineRoot.parent_path() : options.trashParent;
  if (const HRESULT hr = MigrateV1Machine(roots, paths.legacyMachineRoot, legacyTrash, &result->v1);
      FAILED(hr)) {
    Failed(&failures, L"migrate-v1", hr);
  }
  if (FAILED(result->v1.firstError)) Failed(&failures, L"migrate-v1", result->v1.firstError);

  const std::filesystem::path trash =
      options.trashParent.empty() ? paths.machineRoot.parent_path() : options.trashParent;
  if (const HRESULT hr = RemoveOldMachineBuilds(roots, paths.machineRoot, trash, &result->old);
      FAILED(hr)) {
    Failed(&failures, L"old-builds", hr);
  }
  if (FAILED(result->old.firstError)) Failed(&failures, L"old-builds", result->old.firstError);

  if (!failures.empty()) {
    result->code = ExitCode::kFailed;
  } else if (result->v1.restartPending || result->old.restartPending) {
    result->code = ExitCode::kProblems;
  } else {
    result->code = ExitCode::kOk;
  }
  return S_OK;
}
CATCH_RETURN();

// ---- --migrate-v1 ---------------------------------------------------------------------------

HRESULT MigrateV1(const MigrateOptions& options, const RegistryRoots& roots,
                  const SystemProbes& probes, MigrateResult* result) noexcept try {
  *result = MigrateResult{};
  ProcessContext context;
  RETURN_IF_FAILED(UserContext(options.context, probes, &context, &result->refusal));
  if (result->refusal != SetupRefusal::kNone) {
    result->code = RefusalCode(result->refusal);
    return S_OK;
  }
  std::optional<OperationLock> lock;
  RETURN_IF_FAILED(TakeLock(L"migrate-v1", options.lockWaitMs, &lock, &result->busy,
                            &result->namedObjectsRefused));
  if (result->busy || result->namedObjectsRefused) {
    result->code = result->busy ? ExitCode::kBusy : ExitCode::kFailed;
    return S_OK;
  }
  GatherOptions gather;
  gather.liveTest = false;
  gather.context = context;
  gather.paths = options.paths;
  RETURN_IF_FAILED(GatherSnapshot(roots, gather, probes, &result->before));
  const Snapshot& before = result->before;
  // Without this version installed, 1.x is what makes thumbnails work: it stays as it is.
  if (!InstalledSettingsExe(before)) {
    result->refusal = SetupRefusal::kNotInstalled;
    result->code = ExitCode::kNotInstalled;
    return S_OK;
  }
  std::vector<std::wstring>& failures = result->failures;

  if (options.scope != MigrateScope::kMachineOnly) {
    if (const HRESULT hr = MigrateV1User(roots, probes, before, V1UserOptions{}, &result->user);
        FAILED(hr)) {
      Failed(&failures, L"user", hr);
    }
    result->userPart = true;
    failures.insert(failures.end(), result->user.failures.begin(), result->user.failures.end());
  }

  if (options.scope != MigrateScope::kUserOnly) {
    if (const HRESULT hr =
            MachineV1RemovalNeeded(roots, before.paths.legacyMachineRoot, &result->machineNeeded);
        FAILED(hr)) {
      Failed(&failures, L"machine", hr);
    }
    if (result->machineNeeded) {
      // The administrator step runs only from the settings executable installed for all users.
      const std::filesystem::path exe = before.paths.machineRoot / kSettingsExe;
      if (!FileExists(exe)) {
        result->machineNoInstall = true;
      } else if (!options.launcher) {
        Failed(&failures, L"machine", E_INVALIDARG);
      } else {
        ElevatedRun run;
        if (const HRESULT hr = RunElevated(ElevatedVerb::kMachineMigrateV1, exe, options.owner,
                                           options.launcher, &run);
            FAILED(hr)) {
          Failed(&failures, L"machine", hr);
        } else {
          // A step that did not get administrator rights, or refused below an app package, has
          // an exit code of its own. One that names its parts lists them instead of the HRESULT
          // it ended with.
          if (NeedsStepFailure(run)) Failed(&failures, L"machine", run.failure);
          for (std::wstring& failure : NotDoneFailures(run, L"machine")) {
            failures.push_back(std::move(failure));
          }
          result->run = std::move(run);
        }
      }
    }
  }

  if (result->run && result->run->outcome == ElevatedOutcome::kCancelled) {
    result->code = ExitCode::kCancelled;
  } else if (!failures.empty()) {
    result->code = ExitCode::kFailed;
  } else if (result->run && LackedAdministratorRights(*result->run)) {
    result->code = ExitCode::kNeedsAdmin;
  } else if (result->run && RefusedBelowPackage(*result->run)) {
    result->code = ExitCode::kWrongContext;
  } else if ((result->userPart && result->user.Incomplete()) || result->machineNoInstall ||
             (result->run && result->run->outcome == ElevatedOutcome::kRestartPending)) {
    result->code = ExitCode::kProblems;
  } else {
    result->code = ExitCode::kOk;
  }
  LogInfo(L"migrate-v1", L"migrate-v1 result=" + std::wstring(CheckResultName(result->code)) +
                             L" user=" + (result->userPart ? L"yes" : L"no") + L" machine=" +
                             (result->run             ? L"ran"
                              : result->machineNeeded ? L"needed"
                                                      : L"none") +
                             L" failures=" + std::to_wstring(failures.size()));
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
