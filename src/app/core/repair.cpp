#include "repair.h"

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <string>
#include <utility>

#include "file_ops.h"
#include "log.h"
#include "migrate_v1.h"
#include "project_config.h"
#include "registration.h"
#include "settings.h"
#include "strings.h"

namespace pdft {
namespace {

constexpr wchar_t kLogComponent[] = L"check";

std::filesystem::path NormalFolder(const std::filesystem::path& path) {
  std::filesystem::path normal = path.lexically_normal();
  if (normal.has_relative_path() && normal.filename().empty()) normal = normal.parent_path();
  return normal;
}

bool IsV1Version(std::wstring_view version) noexcept {
  return version == L"1" || version.starts_with(L"1.");
}

HRESULT LastErrorOr(HRESULT fallback) noexcept {
  const DWORD error = GetLastError();
  return error == ERROR_SUCCESS ? fallback : HRESULT_FROM_WIN32(error);
}

// The folder |path| exists as a real folder: not a file, not a link or junction.
HRESULT CheckRealFolder(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return LastErrorOr(E_FAIL);
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
  return S_OK;
}

// Deletes a file, read-only or not.
HRESULT DeletePlainFile(const std::filesystem::path& file, DWORD attributes) noexcept {
  if (attributes & FILE_ATTRIBUTE_READONLY) {
    SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
  }
  if (DeleteFileW(file.c_str())) return S_OK;
  return LastErrorOr(E_FAIL);
}

// The DLL may become the per-user class: <userRoot>\app\<2.x build>\<handler DLL>, every folder
// on the way a real folder, the DLL and PDFium present.
HRESULT CheckUserBuildDll(const std::filesystem::path& userRoot,
                          const std::filesystem::path& dll) noexcept try {
  RETURN_HR_IF(E_INVALIDARG, userRoot.empty() || !userRoot.is_absolute() || !dll.is_absolute());
  const std::filesystem::path normal = dll.lexically_normal();
  RETURN_HR_IF(E_INVALIDARG, !IEquals(normal.filename().native(), kHandlerDll));
  const std::filesystem::path folder = normal.parent_path();
  const std::filesystem::path app = NormalFolder(userRoot / kBuildsFolderName);
  RETURN_HR_IF(E_INVALIDARG, !IEquals(folder.parent_path().native(), app.native()));
  const std::optional<BuildFolderNameParts> parts =
      ParseBuildFolderName(folder.filename().native());
  RETURN_HR_IF(E_INVALIDARG, !parts || IsV1Version(parts->version));
  RETURN_IF_FAILED(CheckRealFolder(NormalFolder(userRoot)));
  RETURN_IF_FAILED(CheckRealFolder(app));
  RETURN_IF_FAILED(CheckRealFolder(folder));
  for (const std::filesystem::path& file : {normal, folder / kPdfiumDll}) {
    const DWORD attributes = GetFileAttributesW(file.c_str());
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
                 attributes == INVALID_FILE_ATTRIBUTES ||
                     (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)));
  }
  return S_OK;
}
CATCH_RETURN();

const wchar_t* NotAppliedName(NotApplied reason) noexcept {
  switch (reason) {
    case NotApplied::kNone:
      return L"";
    case NotApplied::kElevated:
      return L"elevated";
    case NotApplied::kNoProgram:
      return L"no-program";
    case NotApplied::kNoTrigger:
      return L"no-trigger";
  }
  return L"";
}

const wchar_t* ResultName(RepairResult result) noexcept {
  switch (result) {
    case RepairResult::kDone:
      return L"done";
    case RepairResult::kUnchanged:
      return L"unchanged";
    case RepairResult::kKept:
      return L"kept-in-use";
    case RepairResult::kNotApplied:
      return L"not-applied";
    case RepairResult::kFailed:
      return L"failed";
  }
  return L"failed";
}

// One log line per step: no paths (they name the user), only the build folder's name.
void LogOutcome(const RepairOutcome& outcome) noexcept try {
  std::wstring message = L"repair ";
  message += RepairStepName(outcome.step.kind);
  if (outcome.step.slot >= 0) message += L" slot=" + std::to_wstring(outcome.step.slot);
  if (outcome.step.kind == RepairStep::Kind::kRemoveUserBuild ||
      outcome.step.kind == RepairStep::Kind::kRegisterUserClass) {
    const std::filesystem::path folder = outcome.step.kind == RepairStep::Kind::kRemoveUserBuild
                                             ? outcome.step.path
                                             : outcome.step.path.parent_path();
    message += L" build=" + NormalFolder(folder).filename().native();
  }
  if (outcome.step.kind == RepairStep::Kind::kRewriteZoom) {
    message += L" zoom=" + std::to_wstring(outcome.step.zoom);
  }
  message += L" result=";
  message += ResultName(outcome.result);
  if (outcome.notApplied != NotApplied::kNone) {
    message += L" why=";
    message += NotAppliedName(outcome.notApplied);
  }
  if (FAILED(outcome.hr)) message += L" hr=" + HResultText(outcome.hr);
  Log(outcome.result == RepairResult::kFailed ? LogLevel::kWarning : LogLevel::kInfo, kLogComponent,
      message);
} catch (...) {
  // Logging is best effort.
}

// Points one slot at the handler if |mode| still allows it for what the slot holds now.
HRESULT WriteSlot(const RegistryRoots& roots, int slot, SlotWrite mode, bool* written) noexcept
    try {
  *written = false;
  RETURN_HR_IF(E_INVALIDARG, slot < 0 || static_cast<size_t>(slot) >= kSlotCount);
  SlotValues now;
  RETURN_IF_FAILED(ReadSlots(roots, &now));
  if (!SlotNeedsWrite(now[static_cast<size_t>(slot)], mode)) return S_OK;
  RETURN_IF_FAILED(WriteRegistryString(roots.user, SlotKeyPath(static_cast<size_t>(slot)), L"",
                                       kHandlerClsidString));
  *written = true;
  return S_OK;
}
CATCH_RETURN();

// The file |path| is there (not a folder).
bool FileExists(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Registers the check task again for the installed settings executable |step.path|, with the
// triggers and the enabled state the task has now.
void ReRegisterTask(const SystemProbes& probes, const Snapshot& snapshot, const RepairStep& step,
                    RepairOutcome* outcome) {
  const auto notApplied = [&](NotApplied reason) {
    outcome->result = RepairResult::kNotApplied;
    outcome->hr = S_OK;
    outcome->notApplied = reason;
  };
  // The task is the signed-in user's and runs with their rights; an elevated process may not
  // even be that user.
  if (snapshot.context.elevated) return notApplied(NotApplied::kElevated);
  const std::optional<std::filesystem::path> program = InstalledSettingsExe(snapshot);
  if (!program || !IEquals(program->native(), step.path.native()) || !FileExists(*program)) {
    return notApplied(NotApplied::kNoProgram);
  }
  TaskState now;
  HRESULT hr = probes.ReadTask(snapshot.taskLocation, &now);
  if (SUCCEEDED(hr) && (!now.exists || TaskRunsProgram(now, *program))) {
    // Deleted or registered again meanwhile: nothing left to do.
    outcome->result = RepairResult::kUnchanged;
    outcome->hr = S_OK;
    return;
  }
  if (SUCCEEDED(hr) && HasNoTriggers(now.options)) return notApplied(NotApplied::kNoTrigger);
  if (SUCCEEDED(hr)) {
    hr = probes.ApplyTaskOptions(snapshot.taskLocation, now.options, *program, snapshot.context);
  }
  outcome->hr = hr;
  outcome->result = FAILED(hr) ? RepairResult::kFailed : RepairResult::kDone;
}

RepairOutcome Apply(const RegistryRoots& roots, const SystemProbes& probes,
                    const Snapshot& snapshot, const RepairStep& step) {
  RepairOutcome outcome;
  outcome.step = step;
  const auto finish = [&](HRESULT hr, bool changed) {
    outcome.hr = hr;
    outcome.result = FAILED(hr) ? RepairResult::kFailed
                     : changed  ? RepairResult::kDone
                                : RepairResult::kUnchanged;
  };
  switch (step.kind) {
    case RepairStep::Kind::kDeleteRunOnce: {
      // The whole family: 1.x's value and any an uninstall left (file_ops.h).
      size_t deleted = 0;
      const HRESULT hr = DeleteRunOnceRemovals(roots, &deleted);
      finish(hr, deleted > 0);
      break;
    }
    case RepairStep::Kind::kRegisterUserClass: {
      HRESULT hr = CheckUserBuildDll(snapshot.paths.userRoot, step.path);
      if (SUCCEEDED(hr)) hr = WriteClass(roots, Scope::kUser, step.path);
      finish(hr, true);
      break;
    }
    case RepairStep::Kind::kDeleteUserClass: {
      // Only while the machine-wide class is still there: without it, the per-user class is the
      // install.
      std::optional<ClassReg> machine;
      HRESULT hr = ReadClass(roots, Scope::kMachine, &machine);
      bool deleted = false;
      if (SUCCEEDED(hr) && machine) {
        hr = DeleteClass(roots, Scope::kUser);
        deleted = SUCCEEDED(hr);
      }
      finish(hr, deleted);
      break;
    }
    case RepairStep::Kind::kFillSlot:
    case RepairStep::Kind::kTakeBackSlot: {
      bool written = false;
      const HRESULT hr = WriteSlot(
          roots, step.slot,
          step.kind == RepairStep::Kind::kFillSlot ? SlotWrite::kFillEmpty : SlotWrite::kTakeBack,
          &written);
      finish(hr, written);
      break;
    }
    case RepairStep::Kind::kRewriteZoom:
      finish(SaveZoom(roots, step.zoom), true);
      break;
    case RepairStep::Kind::kReRegisterTask:
      ReRegisterTask(probes, snapshot, step, &outcome);
      break;
    case RepairStep::Kind::kRemoveUserBuild: {
      if (snapshot.context.elevated) {
        // The folder is in the user's profile, where the user's own programs can swap a link in
        // between the checks and the deletion: an administrator process leaves it to the next
        // unelevated check (RunCheck refuses elevated processes anyway).
        outcome.result = RepairResult::kNotApplied;
        outcome.hr = S_OK;
        outcome.notApplied = NotApplied::kElevated;
        break;
      }
      bool kept = false;
      const HRESULT hr = RemoveUserBuildFolder(snapshot.paths.userRoot, step.path, &kept);
      finish(hr, !kept);
      if (SUCCEEDED(hr) && kept) outcome.result = RepairResult::kKept;
      break;
    }
    case RepairStep::Kind::kRemoveV1Files: {
      if (snapshot.context.elevated) {
        // The user's profile again: left to an unelevated check.
        outcome.result = RepairResult::kNotApplied;
        outcome.hr = S_OK;
        outcome.notApplied = NotApplied::kElevated;
        break;
      }
      V1FilePlan plan;
      std::vector<V1FileOutcome> files;
      const HRESULT hr = RemoveV1UserFiles(roots, probes, snapshot, &plan, &files);
      const auto any = [&](V1FileOutcome::Result result) {
        return std::any_of(files.begin(), files.end(),
                           [&](const V1FileOutcome& file) { return file.result == result; });
      };
      if (FAILED(hr) || any(V1FileOutcome::Result::kFailed)) {
        outcome.result = RepairResult::kFailed;
        outcome.hr = hr;
        for (const V1FileOutcome& file : files) {
          if (SUCCEEDED(outcome.hr) && FAILED(file.hr)) outcome.hr = file.hr;
        }
        if (SUCCEEDED(outcome.hr)) outcome.hr = E_FAIL;
      } else if (std::any_of(files.begin(), files.end(), [](const V1FileOutcome& file) {
                   // Only what waits for a process counts; contents that differ stay for good.
                   return file.result == V1FileOutcome::Result::kKept &&
                          (file.reason == V1KeepReason::kInUse ||
                           file.reason == V1KeepReason::kHeld ||
                           file.reason == V1KeepReason::kWaiting);
                 })) {
        outcome.result = RepairResult::kKept;
      } else {
        outcome.result =
            any(V1FileOutcome::Result::kDone) ? RepairResult::kDone : RepairResult::kUnchanged;
      }
      break;
    }
  }
  return outcome;
}

bool HasFix(std::span<const Finding> findings, FindingId id, FixKind fix) noexcept {
  return std::any_of(findings.begin(), findings.end(), [&](const Finding& finding) {
    return finding.id == id && finding.fix == fix;
  });
}

}  // namespace

const wchar_t* RepairStepName(RepairStep::Kind kind) noexcept {
  switch (kind) {
    case RepairStep::Kind::kDeleteRunOnce:
      return L"delete-runonce";
    case RepairStep::Kind::kRegisterUserClass:
      return L"register-user-class";
    case RepairStep::Kind::kDeleteUserClass:
      return L"delete-user-class";
    case RepairStep::Kind::kFillSlot:
      return L"fill-slot";
    case RepairStep::Kind::kTakeBackSlot:
      return L"take-back-slot";
    case RepairStep::Kind::kRewriteZoom:
      return L"rewrite-zoom";
    case RepairStep::Kind::kReRegisterTask:
      return L"reregister-task";
    case RepairStep::Kind::kRemoveUserBuild:
      return L"remove-user-build";
    case RepairStep::Kind::kRemoveV1Files:
      return L"remove-v1-files";
  }
  return L"unknown";
}

RepairPlan PlanRepairs(const Snapshot& snapshot, std::span<const Finding> findings,
                       RepairMode mode) {
  (void)snapshot;  // everything needed is in the findings; kept for later steps
  RepairPlan plan;
  if (mode == RepairMode::kNone || HasFinding(findings, FindingId::kNotInstalled)) return plan;
  const auto step = [&](RepairStep::Kind kind) -> RepairStep& {
    plan.user.push_back(RepairStep{kind});
    return plan.user.back();
  };

  if (HasFix(findings, FindingId::kCleanupPending, FixKind::kAuto)) {
    step(RepairStep::Kind::kDeleteRunOnce);
  }

  // One class registration covers a missing class, a damaged one and its threading model.
  for (const Finding& finding : findings) {
    const bool userClassFix =
        finding.id == FindingId::kClassMissingUser || finding.id == FindingId::kClassDamagedUser ||
        (finding.id == FindingId::kThreadingModel && finding.scope == Scope::kUser);
    if (userClassFix && finding.fix == FixKind::kAuto && !finding.path.empty()) {
      step(RepairStep::Kind::kRegisterUserClass).path = finding.path;
      break;
    }
  }
  if (HasFix(findings, FindingId::kUserClassShadowsMachine, FixKind::kAuto)) {
    step(RepairStep::Kind::kDeleteUserClass);
  }

  for (const Finding& finding : findings) {
    if (finding.id == FindingId::kSlotEmpty) {
      step(RepairStep::Kind::kFillSlot).slot = finding.slot;
    } else if (finding.id == FindingId::kSlotOtherHandler && mode == RepairMode::kSafeAndTakeBack) {
      step(RepairStep::Kind::kTakeBackSlot).slot = finding.slot;
    }
  }

  for (const Finding& finding : findings) {
    if (finding.id == FindingId::kZoomInvalid && finding.fix == FixKind::kAuto) {
      step(RepairStep::Kind::kRewriteZoom).zoom = finding.zoom;
    }
  }
  for (const Finding& finding : findings) {
    if (finding.id == FindingId::kTaskStaleAction && finding.fix == FixKind::kAuto) {
      step(RepairStep::Kind::kReRegisterTask).path = finding.path;
    }
  }
  for (const Finding& finding : findings) {
    if (finding.id != FindingId::kOldBuildFolders || finding.fix != FixKind::kAuto) continue;
    for (const std::filesystem::path& folder : finding.paths) {
      step(RepairStep::Kind::kRemoveUserBuild).path = folder;
    }
  }
  if (HasFix(findings, FindingId::kV1Leftovers, FixKind::kAuto)) {
    step(RepairStep::Kind::kRemoveV1Files);
  }

  const bool machinePart =
      HasFinding(findings, FindingId::kClassDamagedMachine) ||
      std::any_of(findings.begin(), findings.end(), [](const Finding& finding) {
        return finding.id == FindingId::kThreadingModel && finding.scope == Scope::kMachine;
      });
  if (machinePart) plan.machine = ElevatedVerb::kMachineRepairClass;
  return plan;
}

HRESULT ApplyUserRepairs(const RegistryRoots& roots, const SystemProbes& probes,
                         const Snapshot& snapshot, const RepairPlan& plan,
                         std::vector<RepairOutcome>* outcomes) noexcept try {
  outcomes->clear();
  for (const RepairStep& step : plan.user) {
    RepairOutcome outcome = Apply(roots, probes, snapshot, step);
    LogOutcome(outcome);
    outcomes->push_back(std::move(outcome));
  }
  return S_OK;
}
CATCH_RETURN();

bool ChangedSlotOrClass(std::span<const RepairOutcome> outcomes) noexcept {
  return std::any_of(outcomes.begin(), outcomes.end(), [](const RepairOutcome& outcome) {
    if (outcome.result != RepairResult::kDone) return false;
    switch (outcome.step.kind) {
      case RepairStep::Kind::kRegisterUserClass:
      case RepairStep::Kind::kDeleteUserClass:
      case RepairStep::Kind::kFillSlot:
      case RepairStep::Kind::kTakeBackSlot:
        return true;
      default:
        return false;
    }
  });
}

int CountDone(std::span<const RepairOutcome> outcomes) noexcept {
  return static_cast<int>(std::count_if(
      outcomes.begin(), outcomes.end(),
      [](const RepairOutcome& outcome) { return outcome.result == RepairResult::kDone; }));
}

bool AnyFailed(std::span<const RepairOutcome> outcomes) noexcept {
  return std::any_of(outcomes.begin(), outcomes.end(), [](const RepairOutcome& outcome) {
    return outcome.result == RepairResult::kFailed;
  });
}

bool IsInUseError(HRESULT hr) noexcept {
  return hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) ||
         hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
         hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) ||
         hr == HRESULT_FROM_WIN32(ERROR_USER_MAPPED_FILE);
}

HRESULT RemoveUserBuildFolder(const std::filesystem::path& userRoot,
                              const std::filesystem::path& dir, bool* kept) noexcept try {
  *kept = false;
  RETURN_HR_IF(E_INVALIDARG, userRoot.empty() || !userRoot.is_absolute() || !dir.is_absolute());
  const std::filesystem::path root = NormalFolder(userRoot);
  const std::filesystem::path app = NormalFolder(root / kBuildsFolderName);
  const std::filesystem::path folder = NormalFolder(dir);
  RETURN_HR_IF(E_INVALIDARG, !IEquals(folder.parent_path().native(), app.native()));
  const std::optional<BuildFolderNameParts> parts =
      ParseBuildFolderName(folder.filename().native());
  RETURN_HR_IF(E_INVALIDARG, !parts || IsV1Version(parts->version));

  RETURN_IF_FAILED(CheckRealFolder(root));
  RETURN_IF_FAILED(CheckRealFolder(app));
  if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES &&
      (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
    return S_OK;  // gone already
  }
  RETURN_IF_FAILED(CheckRealFolder(folder));

  // The handler DLL first: while a thumbnail process has it loaded it cannot be deleted, and the
  // folder then stays whole for the next check.
  const std::filesystem::path dll = folder / kHandlerDll;
  const DWORD dllAttributes = GetFileAttributesW(dll.c_str());
  if (dllAttributes != INVALID_FILE_ATTRIBUTES &&
      !(dllAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    const HRESULT hr = DeletePlainFile(dll, dllAttributes);
    if (IsInUseError(hr)) {
      *kept = true;
      return S_OK;
    }
    RETURN_IF_FAILED(hr);
  }
  const HRESULT hr = RemoveTreeNoFollow(folder);
  if (IsInUseError(hr) || hr == HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY)) {
    *kept = true;
    return S_OK;
  }
  return hr;
}
CATCH_RETURN();

}  // namespace pdft
