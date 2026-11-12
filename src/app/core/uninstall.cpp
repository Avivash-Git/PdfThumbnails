#include "uninstall.h"

#include <shellapi.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <utility>

#include "failure_trace.h"
#include "instance.h"
#include "legacy.h"
#include "log.h"
#include "paths.h"
#include "project_config.h"
#include "strings.h"
#include "tasks.h"

namespace pdft {
namespace {

constexpr wchar_t kLogComponent[] = L"uninstall";

constexpr std::wstring_view kLicensesPdfium = L"licenses\\pdfium";
constexpr std::wstring_view kLicenses = L"licenses";

// The per-user root's files of this version, in the order they go (folders after their files).
constexpr std::wstring_view kUserRootFiles[] = {
    kSettingsExe,
    kCliExe,
    L"LICENSE.txt",
    L"THIRD-PARTY-NOTICES.md",
    kLicensesPdfium,
    L"licenses\\pdfium-binaries-LICENSE.txt",
    L"licenses\\wil-LICENSE.txt",
    kLicenses,
};

// How long the per-user app folder is retried while Explorer lets go of its DLLs.
constexpr DWORD kUserRemoveRetryMs = 1000;

// One listed failure: "<step>: <hr>", or "<step> left: <hr>" for keys DeleteRegistryTree left as
// they were (kRegistryTreeLeftHr: a link it never follows, or a tree past its bounds).
void Failed(std::vector<std::wstring>* failures, const wchar_t* step, HRESULT hr) {
  failures->push_back(std::wstring(step) + (hr == kRegistryTreeLeftHr ? L" left: " : L": ") +
                      HResultText(hr));
}

// What makes the rebuild of --uninstall --rebuild-thumbnails a step that failed: File Explorer did
// not come back (why the start failed, or could not be made ready, where it came to a start;
// ERROR_TIMEOUT where none was tried, or one was and File Explorer still did not come back), or a
// cache file could not be deleted. A rebuild that was refused or could not start changed nothing;
// programs left alone that keep cache files are only reported.
std::optional<HRESULT> RebuildStepFailure(const RebuildResult& rebuild) noexcept {
  if (rebuild.shell == ShellOutcome::kNotBack) {
    // Where it never came to a start, startError may hold why the fallback could not be made
    // ready before anything changed, which is not why File Explorer did not come back.
    const bool cameToStart = rebuild.start == ShellStart::kStarted ||
                             rebuild.start == ShellStart::kJobForbids ||
                             rebuild.start == ShellStart::kFailed;
    return cameToStart && FAILED(rebuild.startError) ? rebuild.startError
                                                     : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
  if (rebuild.failed > 0) return rebuild.firstFileError;
  return std::nullopt;
}

DWORD Attributes(const std::filesystem::path& path) noexcept {
  return GetFileAttributesW(path.c_str());
}

bool Exists(const std::filesystem::path& path) noexcept {
  return Attributes(path) != INVALID_FILE_ATTRIBUTES;
}

bool IsRealFolder(const std::filesystem::path& path) noexcept {
  const DWORD attributes = Attributes(path);
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
         !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool IsRegularFile(const std::filesystem::path& path) noexcept {
  const DWORD attributes = Attributes(path);
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

// Takes the operation lock. S_OK also when it did not: *busy when another process keeps it,
// *refused when Windows does not let this process make it (logged). May throw std::bad_alloc.
HRESULT TakeLock(DWORD waitMs, std::optional<OperationLock>* lock, bool* busy, bool* refused) {
  *busy = false;
  *refused = false;
  const HRESULT hr = OperationLock::Acquire(EffectiveLockWaitMs(waitMs), lock);
  if (hr == OperationLock::kBusy || hr == OperationLock::kTakenByLowerIntegrity) {
    *busy = true;
    return S_OK;
  }
  if (hr == kNamedObjectsRefusedHr) {
    *refused = true;
    Log(LogLevel::kWarning, kLogComponent,
        L"uninstall result=failed named-objects-refused hr=" + HResultText(hr));
    return S_OK;
  }
  if (SUCCEEDED(hr) && lock->has_value() && (*lock)->sessionOnly()) {
    Log(LogLevel::kWarning, kLogComponent, L"uninstall lock=session-only");
  }
  return hr;
}

// The slots, the class and the product key in every loaded user hive below |users|. A failure
// in one hive is noted and the others still go.
//
// These hives are other users' own, and they may have made anything at these paths, links
// included. Checking the keys' owner would add nothing: a user owns what they make in their
// hive, links too. What keeps these deletions to our own keys is that they go only to fixed
// names (a slot only while it names the handler), that DeleteRegistryTree never follows a link
// (it leaves it, and the hive counts in hivesLeft) and stops at its bounds, so whatever a user
// made there neither leads this step elsewhere nor keeps it busy. A link on the way down to one
// of these keys can lead only elsewhere in that same user's hives, their own and their classes
// hive (Windows itself links Software\Classes there): Windows refuses links from a user's hives
// into another user's or into HKEY_LOCAL_MACHINE.
void CleanLoadedHives(HKEY users, MachineUninstallResult* result,
                      const std::function<void(const wchar_t*, HRESULT)>& note) {
  std::vector<std::wstring> names;
  for (DWORD index = 0;; ++index) {
    wchar_t name[256] = {};
    DWORD size = ARRAYSIZE(name);
    const LSTATUS status =
        RegEnumKeyExW(users, index, name, &size, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status == ERROR_MORE_DATA) continue;  // longer than any SID: not a user's hive
    if (status != ERROR_SUCCESS) {
      note(L"hives", HRESULT_FROM_WIN32(status));
      break;
    }
    if (IsUserHiveName(name)) names.emplace_back(name, size);
  }
  for (const std::wstring& name : names) {
    bool classesHive = false;
    IsUserHiveName(name, &classesHive);
    wil::unique_hkey hive;
    const LSTATUS status =
        RegOpenKeyExW(users, name.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, hive.put());
    if (status == ERROR_FILE_NOT_FOUND) continue;  // unloaded meanwhile
    if (status != ERROR_SUCCESS) {
      note(L"hive", HRESULT_FROM_WIN32(status));
      continue;
    }
    bool removed = false;
    bool left = false;
    const auto noteHive = [&](HRESULT hr) {
      if (hr == kRegistryTreeLeftHr) left = true;
      note(L"hive", hr);
    };
    if (classesHive) {
      ClassesHiveRemoval removal;
      noteHive(RemoveFromClassesHive(hive.get(), &removal));
      removed = removal.slots[0] || removal.slots[1] || removal.classDeleted;
    } else {
      // Our own known keys only: a slot only while it names the handler, the handler's class key
      // and the product key.
      const RegistryRoots hiveRoots{hive.get(), HKEY_LOCAL_MACHINE};
      std::array<bool, kSlotCount> slots = {};
      noteHive(RemoveSlotsIfOurs(hiveRoots, &slots));
      bool classDeleted = false;
      noteHive(DeleteRegistryTree(hive.get(), ClassKeyPath(), &classDeleted));
      bool settingsDeleted = false;
      noteHive(DeleteRegistryTree(hive.get(), kRegKey, &settingsDeleted));
      // What Active Setup recorded for this user: a later install sets them up again.
      bool activeSetupDeleted = false;
      noteHive(DeleteActiveSetup(hive.get(), &activeSetupDeleted));
      removed = slots[0] || slots[1] || classDeleted || settingsDeleted || activeSetupDeleted;
    }
    if (removed) ++result->hivesCleaned;
    if (left) ++result->hivesLeft;
  }
}

// One report line about a folder removal.
void ReportRoot(ElevatedReporter& report, const wchar_t* what, const RemoveOutcome& outcome) {
  if (outcome.kept > 0) {
    report.Line(std::wstring(what) + L" emptied but for the installer's own files, which its " +
                L"uninstaller removes" +
                (outcome.trash.empty() ? L"" : L"; files in use wait in a trash folder"));
  } else if (!outcome.trash.empty()) {
    report.Line(std::wstring(what) + L" removed; files in use wait in a trash folder");
  } else if (!outcome.left.empty()) {
    report.Line(std::wstring(what) + L" left: " + std::to_wstring(outcome.left.size()) +
                L" files in use");
  } else if (outcome.removed) {
    report.Line(std::wstring(what) + L" removed");
  }
}

}  // namespace

// ---- The per-user part ------------------------------------------------------------------------

HRESULT RemoveUserRegistration(const RegistryRoots& roots, const SystemProbes& probes,
                               const Snapshot& snapshot, UserRemoval* removal) noexcept try {
  *removal = UserRemoval{};
  std::vector<std::wstring>& failures = removal->failures;
  if (const HRESULT hr = RemoveSlotsIfOurs(roots, &removal->slots); FAILED(hr)) {
    Failed(&failures, L"slots", hr);
  }
  if (const HRESULT hr = DeleteRegistryTree(roots.user, ClassKeyPath(), &removal->classDeleted);
      FAILED(hr)) {
    Failed(&failures, L"user-class", hr);
  }
  if (!snapshot.context.elevated && !snapshot.taskLocation.name.empty()) {
    const HRESULT hr = probes.ApplyTaskOptions(
        snapshot.taskLocation, OptionsFromCheckboxes(false, false), {}, snapshot.context);
    removal->taskDeleted = hr == S_OK && snapshot.task.exists;
    if (FAILED(hr)) Failed(&failures, L"task", hr);
  }
  {
    const HRESULT hr = probes.DeleteLegacyTask();
    removal->legacyTaskDeleted = hr == S_OK;
    if (FAILED(hr)) Failed(&failures, L"legacy-task", hr);
  }
  if (const HRESULT hr = DeleteRegistryTree(roots.user, kRegKey, &removal->settingsDeleted);
      FAILED(hr)) {
    Failed(&failures, L"settings", hr);
  }
  // What Active Setup recorded for this user, so a later install sets them up again.
  if (const HRESULT hr = DeleteActiveSetup(roots.user); FAILED(hr)) {
    Failed(&failures, L"active-setup", hr);
  }
  removal->notified = NotifyAssocChanged(roots);
  return S_OK;
}
CATCH_RETURN();

std::span<const std::wstring_view> UserRootFiles() noexcept { return kUserRootFiles; }

HRESULT RemoveUserFiles(const RegistryRoots& roots, const std::filesystem::path& userRoot,
                        const std::filesystem::path& dataDir, UserFilesRemoval* removal) noexcept
    try {
  *removal = UserFilesRemoval{};
  std::vector<std::wstring>& failures = removal->failures;
  std::vector<std::filesystem::path> leftInRoot;
  std::vector<std::filesystem::path> leftInApp;
  std::vector<std::filesystem::path> leftInData;
  bool v1Manager = false;

  if (!userRoot.empty() && IsRealFolder(userRoot)) {
    std::vector<FolderEntry> entries;
    if (SUCCEEDED(ListFolderEntries(userRoot, &entries))) v1Manager = HasV1Manager(entries);
    for (const std::wstring_view relative : kUserRootFiles) {
      // 1.x's licenses folder shares these names: it stays while 1.x's manager does.
      if (v1Manager && relative.starts_with(kLicenses)) continue;
      const std::filesystem::path path = userRoot / relative;
      const DWORD attributes = Attributes(path);
      if (attributes == INVALID_FILE_ATTRIBUTES) continue;
      if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // never followed nor ours
      if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (relative == kLicensesPdfium) {
          RemoveOutcome outcome;
          if (const HRESULT hr = RemoveTreeOrTrash(path, {}, 0, &outcome); FAILED(hr)) {
            Failed(&failures, L"licenses", hr);
          }
          leftInRoot.insert(leftInRoot.end(), outcome.left.begin(), outcome.left.end());
        } else {
          RemoveDirectoryW(path.c_str());  // only when empty
        }
        continue;
      }
      ContentDelete result = ContentDelete::kDeleted;
      if (const HRESULT hr = DeleteFileIfContent(path, {}, &result); FAILED(hr)) {
        Failed(&failures, L"file", hr);
      } else if (result == ContentDelete::kInUse) {
        leftInRoot.push_back(path);
      }
    }
    const std::filesystem::path app = userRoot / kBuildsFolderName;
    if (IsRealFolder(app)) {
      RemoveOutcome outcome;
      if (const HRESULT hr = RemoveTreeOrTrash(app, {}, kUserRemoveRetryMs, &outcome); FAILED(hr)) {
        Failed(&failures, L"app", hr);
      }
      leftInApp = std::move(outcome.left);
    }
    RemoveDirectoryW(userRoot.c_str());  // only when nothing else is in it
  }
  if (!dataDir.empty() && IsRealFolder(dataDir)) {
    RemoveOutcome outcome;
    if (const HRESULT hr = RemoveTreeOrTrash(dataDir, {}, 0, &outcome); FAILED(hr)) {
      Failed(&failures, L"data", hr);
    }
    leftInData = std::move(outcome.left);
  }

  // What is in use goes at the next sign-in: the whole per-user root, unless 1.x's manager is
  // still in it (then only the app folder); the data folder as a whole.
  std::vector<std::filesystem::path> dirs;
  if (!leftInRoot.empty() || !leftInApp.empty()) {
    if (!v1Manager) {
      dirs.push_back(userRoot);
    } else if (!leftInApp.empty()) {
      dirs.push_back(userRoot / kBuildsFolderName);
    }
    if (v1Manager && !leftInRoot.empty()) removal->notScheduled.push_back(userRoot);
  }
  if (!leftInData.empty()) dirs.push_back(dataDir);
  removal->left = std::move(leftInRoot);
  removal->left.insert(removal->left.end(), leftInApp.begin(), leftInApp.end());
  removal->left.insert(removal->left.end(), leftInData.begin(), leftInData.end());
  if (!dirs.empty()) {
    std::vector<std::filesystem::path> notScheduled;
    if (const HRESULT hr = ScheduleRunOnceRemoval(roots, dirs, &notScheduled); FAILED(hr)) {
      Failed(&failures, L"runonce", hr);
      notScheduled = dirs;
    }
    for (const std::filesystem::path& dir : dirs) {
      const bool missed = std::any_of(notScheduled.begin(), notScheduled.end(),
                                      [&](const auto& other) { return other == dir; });
      (missed ? removal->notScheduled : removal->scheduled).push_back(dir);
    }
  }
  return S_OK;
}
CATCH_RETURN();

// ---- The part for all users -------------------------------------------------------------------

bool IsUserHiveName(std::wstring_view name, bool* classes) noexcept {
  if (classes) *classes = false;
  constexpr std::wstring_view kPrefix = L"S-1-5-21-";
  constexpr std::wstring_view kClassesSuffix = L"_Classes";
  if (!name.starts_with(kPrefix)) return false;
  bool isClasses = false;
  if (name.size() > kClassesSuffix.size() &&
      IEquals(name.substr(name.size() - kClassesSuffix.size()), kClassesSuffix)) {
    name.remove_suffix(kClassesSuffix.size());
    isClasses = true;
  }
  name.remove_prefix(kPrefix.size());
  if (name.empty() || name.front() == L'-' || name.back() == L'-') return false;
  bool previousDash = false;
  for (const wchar_t c : name) {
    if (c == L'-') {
      if (previousDash) return false;
      previousDash = true;
    } else if (c >= L'0' && c <= L'9') {
      previousDash = false;
    } else {
      return false;
    }
  }
  if (classes) *classes = isClasses;
  return true;
}

HRESULT UninstallForAllUsers(const MachineUninstallOptions& options, const RegistryRoots& roots,
                             MachineUninstallResult* result) noexcept try {
  *result = MachineUninstallResult{};
  RETURN_HR_IF(E_INVALIDARG,
               !options.machineRoot.is_absolute() || !options.machineRoot.has_relative_path());
  const auto note = [result](const wchar_t* step, HRESULT hr) {
    if (SUCCEEDED(hr)) return;
    try {
      Failed(&result->failures, step, hr);
    } catch (...) {
    }
    if (SUCCEEDED(result->firstError)) result->firstError = hr;
  };

  note(L"class", DeleteMachineClass(roots, &result->classDeleted));
  note(L"active-setup", DeleteActiveSetup(roots.machine));
  if (!options.taskFolder.empty()) {
    result->tasks = DeleteTaskFolderAllUsers(options.taskFolder, options.taskRootPrefix);
    note(L"tasks", result->tasks);
    if (options.legacyTask) {
      result->legacyTask = DeleteLegacyTask();
      note(L"legacy-task", result->legacyTask);
    }
  }

  // The install folders before the other users' hives: whatever those users made in their hives,
  // nothing of the part for all users is left waiting for it. While the installer has an install
  // for all users, this runs from its uninstaller, which still has its own files open in the
  // install folder and deletes them, and the folder, itself afterwards.
  const std::filesystem::path trashParent =
      options.trashParent.empty() ? options.machineRoot.parent_path() : options.trashParent;
  std::optional<InnoInstall> inno;
  if (const HRESULT hr = FindInnoInstall(roots, &inno); FAILED(hr)) note(L"installer", hr);
  result->installerFiles = inno && inno->scope == Scope::kMachine;
  if (const HRESULT hr = CheckNoReparsePoints(options.machineRoot, {}); FAILED(hr)) {
    note(L"folder", hr);
  } else {
    note(L"folder",
         RemoveTreeOrTrash(options.machineRoot, trashParent, options.retryMs, &result->root,
                           result->installerFiles ? IsInnoUninstallerFileName : nullptr));
  }
  if (!options.legacyMachineRoot.empty() && options.legacyMachineRoot.has_relative_path() &&
      !IEquals(options.legacyMachineRoot.lexically_normal().native(),
               options.machineRoot.lexically_normal().native())) {
    if (const HRESULT hr = CheckNoReparsePoints(options.legacyMachineRoot, {}); FAILED(hr)) {
      note(L"legacy-folder", hr);
    } else {
      note(L"legacy-folder", RemoveTreeOrTrash(options.legacyMachineRoot, trashParent,
                                               options.retryMs, &result->legacyRoot));
    }
  }
  if (options.users) CleanLoadedHives(options.users, result, note);
  result->restartPending = result->root.restartPending || result->legacyRoot.restartPending;
  return S_OK;
}
CATCH_RETURN();

HRESULT RunMachineUninstall(ElevatedReporter& report, bool* restartPending,
                            std::vector<std::wstring>* notDone) noexcept try {
  *restartPending = false;
  notDone->clear();
  TraceStep step(L"own-paths");
  MachineUninstallOptions options;
  RETURN_IF_FAILED(PDFT_TRACE(GetMachineRoot(&options.machineRoot)));
  RETURN_IF_FAILED(PDFT_TRACE(GetLegacyMachineRoot(&options.legacyMachineRoot)));
  step.Set(L"uninstall");
  MachineUninstallResult result;
  RETURN_IF_FAILED(PDFT_TRACE(UninstallForAllUsers(options, RegistryRoots{}, &result)));
  if (result.classDeleted) report.Line(L"class for all users removed");
  report.Line(L"check tasks hr=" + HResultText(result.tasks) + L", 1.x task hr=" +
              HResultText(result.legacyTask));
  report.Line(L"user hives cleaned: " + std::to_wstring(result.hivesCleaned));
  if (result.hivesLeft != 0) {
    report.Line(L"user hives where keys were left (a registry link, or a tree too large): " +
                std::to_wstring(result.hivesLeft));
  }
  ReportRoot(report, L"install folder", result.root);
  ReportRoot(report, L"1.x install folder", result.legacyRoot);
  *restartPending = result.restartPending;
  *notDone = result.failures;
  // Its failures are reported as not done, each with its part, and it ends with the first one's
  // HRESULT, as before. The trace names this place, never one of them that was tolerated.
  return PDFT_TRACE(result.firstError);
}
CATCH_RETURN();

// ---- --uninstall ----------------------------------------------------------------------------

DWORD InstallerWaitMs(bool yes) noexcept { return yes ? kInstallerWaitYesMs : kInstallerWaitMs; }

HRESULT RunProgramAndWait(const std::filesystem::path& exe, const std::wstring& arguments,
                          const ProgramWait& wait, DWORD* exitCode) noexcept try {
  *exitCode = 0;
  wchar_t system[MAX_PATH] = {};
  const UINT length = GetSystemDirectoryW(system, ARRAYSIZE(system));
  RETURN_LAST_ERROR_IF(length == 0 || length >= ARRAYSIZE(system));
  SHELLEXECUTEINFOW info = {sizeof(info)};
  info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_UNICODE |
               SEE_MASK_NO_CONSOLE;
  info.lpVerb = L"open";
  info.lpFile = exe.c_str();
  info.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
  info.lpDirectory = system;
  info.nShow = SW_SHOWNORMAL;
  RETURN_IF_WIN32_BOOL_FALSE(ShellExecuteExW(&info));
  wil::unique_handle process(info.hProcess);
  RETURN_HR_IF(E_UNEXPECTED, !process);
  wil::unique_event_nothrow stopped;
  RETURN_IF_FAILED(stopped.create(wil::EventOptions::ManualReset));
  const HANDLE stoppedHandle = stopped.get();
  std::stop_callback onStop(wait.stop, [stoppedHandle] { SetEvent(stoppedHandle); });
  const HANDLE handles[] = {process.get(), stopped.get()};
  switch (WaitForMultipleObjects(ARRAYSIZE(handles), handles, FALSE, wait.limitMs)) {
    case WAIT_OBJECT_0:
      RETURN_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(process.get(), exitCode));
      return S_OK;
    case WAIT_OBJECT_0 + 1:
      // A program that ended at the same moment still wins.
      if (WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0) {
        RETURN_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(process.get(), exitCode));
        return S_OK;
      }
      return kStoppedWaitingHr;
    case WAIT_TIMEOUT:
      return kWaitLimitHr;
    default:
      RETURN_LAST_ERROR();
  }
}
CATCH_RETURN();

HRESULT ReadInstallTraces(const RegistryRoots& roots, const std::filesystem::path& folder,
                          InstallTraces* traces) noexcept try {
  *traces = InstallTraces{};
  std::optional<ClassReg> machine;
  std::optional<ClassReg> user;
  RETURN_IF_FAILED(ReadClass(roots, Scope::kMachine, &machine));
  RETURN_IF_FAILED(ReadClass(roots, Scope::kUser, &user));
  SlotValues slots;
  RETURN_IF_FAILED(ReadSlots(roots, &slots));
  traces->machineClass = machine.has_value();
  traces->userClass = user.has_value();
  traces->slotOurs = std::any_of(slots.begin(), slots.end(),
                                 [](const auto& slot) { return slot && IsOurClsid(*slot); });
  traces->folder = !folder.empty() && Exists(folder);
  traces->read = true;
  return S_OK;
}
CATCH_RETURN();

bool TracesChanged(const InstallTraces& before, const InstallTraces& after) noexcept {
  if (!before.read || !after.read) return true;
  return (before.machineClass && !after.machineClass) || (before.userClass && !after.userClass) ||
         (before.slotOurs && !after.slotOurs) || (before.folder && !after.folder);
}

Scope HandlerLeft(const InstallTraces& traces) noexcept {
  if (traces.machineClass) return Scope::kMachine;
  if (traces.userClass || traces.slotOurs) return Scope::kUser;
  return Scope::kNone;
}

InstallerOutcome JudgeInstallerRun(HRESULT run, DWORD exitCode, bool recordLeft, bool changed,
                                   Scope handlerLeft) noexcept {
  if (run == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return InstallerOutcome::kCancelled;
  if (run == kStoppedWaitingHr) return InstallerOutcome::kStoppedWaiting;
  if (run == kWaitLimitHr) return InstallerOutcome::kTimedOut;
  if (FAILED(run)) return InstallerOutcome::kNotStarted;
  if (exitCode != 0) {
    return recordLeft && !changed ? InstallerOutcome::kNothingRemoved
                                  : InstallerOutcome::kStoppedPartWay;
  }
  if (recordLeft) return InstallerOutcome::kRecordLeft;
  if (handlerLeft != Scope::kNone) return InstallerOutcome::kHandlerLeft;
  return InstallerOutcome::kRemoved;
}

ExitCode InstallerUninstallExitCode(InstallerOutcome outcome, bool failures) noexcept {
  switch (outcome) {
    case InstallerOutcome::kCancelled:
    case InstallerOutcome::kNothingRemoved:
      return ExitCode::kCancelled;
    case InstallerOutcome::kNotStarted:
    case InstallerOutcome::kStoppedPartWay:
      return ExitCode::kFailed;
    case InstallerOutcome::kStoppedWaiting:
    case InstallerOutcome::kTimedOut:
    case InstallerOutcome::kRecordLeft:
    case InstallerOutcome::kHandlerLeft:
      return failures ? ExitCode::kFailed : ExitCode::kProblems;
    case InstallerOutcome::kRemoved:
    case InstallerOutcome::kNotRun:
      break;
  }
  return failures ? ExitCode::kFailed : ExitCode::kOk;
}

bool AnythingToUninstall(const Snapshot& snapshot, bool settingsKey, bool machineRootExists,
                         bool userFilesExist, bool dataDir) noexcept {
  const bool slotOurs = std::any_of(
      snapshot.slots.begin(), snapshot.slots.end(),
      [](const std::optional<std::wstring>& slot) { return slot && IsOurClsid(*slot); });
  return snapshot.machineClass || snapshot.userClass || machineRootExists || userFilesExist ||
         slotOurs || snapshot.task.exists || settingsKey || dataDir;
}

namespace {

const wchar_t* OutcomeToken(InstallerOutcome outcome) noexcept {
  switch (outcome) {
    case InstallerOutcome::kNotRun:
      return L"not-run";
    case InstallerOutcome::kCancelled:
      return L"cancelled";
    case InstallerOutcome::kNotStarted:
      return L"not-started";
    case InstallerOutcome::kStoppedWaiting:
      return L"stopped-waiting";
    case InstallerOutcome::kTimedOut:
      return L"timed-out";
    case InstallerOutcome::kNothingRemoved:
      return L"nothing-removed";
    case InstallerOutcome::kStoppedPartWay:
      return L"stopped-part-way";
    case InstallerOutcome::kRecordLeft:
      return L"record-left";
    case InstallerOutcome::kHandlerLeft:
      return L"handler-left";
    case InstallerOutcome::kRemoved:
      return L"removed";
  }
  return L"?";
}

// --uninstall where the installer has an install: its own uninstaller removes it (so its records
// stay right), and this waits for it. With --rebuild-thumbnails and a check that allows the
// rebuild, the operation lock is taken first, and once the uninstaller has finished with exit
// code 0, its record is gone and nothing of the handler is registered, this user's own part is
// removed (an administrator who answered the uninstaller's prompt may have cleaned another
// account's) and the thumbnail cache is rebuilt.
HRESULT UninstallByInstaller(const UninstallOptions& options, const RegistryRoots& roots,
                             const SystemProbes& probes, const ProcessContext& context,
                             UninstallResult* result) noexcept try {
  std::vector<std::wstring>& failures = result->failures;
  std::filesystem::path uninstaller;
  result->innoRun = ParseInnoUninstallString(result->inno->uninstallString, &uninstaller);
  if (FAILED(result->innoRun)) {
    Failed(&failures, L"uninstaller", result->innoRun);
    result->innoOutcome = InstallerOutcome::kNotStarted;
    result->code = ExitCode::kFailed;
    return S_OK;
  }

  // The rebuild's check: the command's own, or one made here. A refusal or failure is the report's
  // answer, and the uninstaller still runs, without the lock.
  const RebuildSystem& system =
      options.rebuildSystem ? *options.rebuildSystem : RealRebuildSystem();
  if (options.rebuildThumbnails) {
    RebuildPrecheck check;
    if (options.rebuildPrecheck) {
      check = *options.rebuildPrecheck;
    } else if (const HRESULT hr = PrecheckRebuild(context, system, &check); FAILED(hr)) {
      check.failure = hr;
    }
    if (FAILED(check.failure) || check.refusal != RebuildRefusal::kNone) {
      result->rebuild = RebuildNotStarted(check);
      Log(LogLevel::kWarning, L"refresh", RebuildPrecheckLogText(check));
    } else {
      result->rebuildPlanned = true;
    }
  }
  std::optional<OperationLock> lock;
  if (result->rebuildPlanned) {
    RETURN_IF_FAILED(
        TakeLock(options.lockWaitMs, &lock, &result->busy, &result->namedObjectsRefused));
    if (result->busy || result->namedObjectsRefused) {
      result->code = result->busy ? ExitCode::kBusy : ExitCode::kFailed;
      return S_OK;
    }
  }

  EnginePaths paths;
  if (options.paths) {
    paths = *options.paths;
  } else {
    RETURN_IF_FAILED(GetEnginePaths(&paths));
  }
  const std::filesystem::path folder =
      result->inno->scope == Scope::kMachine ? paths.machineRoot : paths.userRoot;
  InstallTraces before;
  if (const HRESULT hr = ReadInstallTraces(roots, folder, &before); FAILED(hr)) {
    Log(LogLevel::kWarning, kLogComponent, L"uninstall installer traces hr=" + HResultText(hr));
  }
  if (options.stop.stop_requested()) {
    result->refusal = UninstallRefusal::kCancelled;
    result->code = ExitCode::kCancelled;
    return S_OK;
  }

  result->innoLimitMs =
      options.installerWaitMs != 0 ? options.installerWaitMs : InstallerWaitMs(options.yes);
  if (options.beforeUninstaller) options.beforeUninstaller(result->rebuildPlanned);
  Log(LogLevel::kInfo, kLogComponent,
      std::wstring(L"uninstall installer start scope=") +
          (result->inno->scope == Scope::kMachine ? L"machine" : L"user") + L" silent=" +
          (options.yes ? L"yes" : L"no") + L" rebuild=" +
          (!options.rebuildThumbnails ? L"no"
           : result->rebuildPlanned   ? L"yes"
                                      : L"refused") +
          L" limit-ms=" + std::to_wstring(result->innoLimitMs));
  const ULONGLONG started = GetTickCount64();
  result->innoRun =
      options.runner
          ? options.runner(uninstaller, InnoUninstallArguments(options.yes),
                           ProgramWait{options.stop, result->innoLimitMs}, &result->innoExit)
          : E_INVALIDARG;
  result->innoWaitMs = GetTickCount64() - started;

  // What it left: its record, and what of the handler is still there.
  const bool ran = SUCCEEDED(result->innoRun);
  InstallTraces after;
  bool changed = true;
  if (ran) {
    if (const HRESULT hr = RegistryKeyExists(RootOf(roots, result->inno->scope), InnoUninstallKey(),
                                             &result->innoRecordLeft);
        FAILED(hr)) {
      Failed(&failures, L"check", hr);
      result->innoRecordLeft = true;
    }
    if (const HRESULT hr = ReadInstallTraces(roots, folder, &after); FAILED(hr)) {
      Failed(&failures, L"check", hr);
    }
    changed = TracesChanged(before, after);
  }
  if (ran && result->innoExit == 0 && !result->innoRecordLeft) {
    // This user's own part, unelevated; then look again.
    GatherOptions gather;
    gather.liveTest = false;
    gather.context = context;
    gather.paths = paths;
    Snapshot snapshot;
    if (const HRESULT hr = GatherSnapshot(roots, gather, probes, &snapshot); FAILED(hr)) {
      Failed(&failures, L"user", hr);
    } else {
      RETURN_IF_FAILED(RemoveUserRegistration(roots, probes, snapshot, &result->user));
      failures.insert(failures.end(), result->user.failures.begin(), result->user.failures.end());
    }
    if (const HRESULT hr = ReadInstallTraces(roots, folder, &after); FAILED(hr)) {
      Failed(&failures, L"check", hr);
    }
    result->handlerLeft = after.read ? HandlerLeft(after) : Scope::kNone;
  }
  result->innoOutcome = JudgeInstallerRun(result->innoRun, result->innoExit, result->innoRecordLeft,
                                          changed, result->handlerLeft);
  if (result->innoOutcome == InstallerOutcome::kNotStarted) {
    Failed(&failures, L"uninstaller", result->innoRun);
  }
  Log(LogLevel::kInfo, kLogComponent,
      std::wstring(L"uninstall installer end outcome=") + OutcomeToken(result->innoOutcome) +
          L" exit=" + std::to_wstring(result->innoExit) + L" hr=" + HResultText(result->innoRun) +
          L" wait-ms=" + std::to_wstring(result->innoWaitMs) + L" traces=" +
          (changed ? L"changed" : L"unchanged"));

  // The rebuild, under the lock, once nothing of the handler is left: File Explorer then makes
  // every thumbnail again without it.
  if (result->rebuildPlanned && result->innoOutcome == InstallerOutcome::kRemoved) {
    RebuildOptions rebuildOptions;
    rebuildOptions.context = context;
    RebuildResult rebuild;
    if (const HRESULT hr = RebuildThumbnailCache(rebuildOptions, system, options.rebuildProgress,
                                                 options.stop, &rebuild);
        FAILED(hr)) {
      rebuild = RebuildResult{};
      rebuild.failure = hr;
    }
    if (const std::optional<HRESULT> failure = RebuildStepFailure(rebuild)) {
      Failed(&failures, L"rebuild", *failure);
    }
    result->rebuild = std::move(rebuild);
  }
  result->code = InstallerUninstallExitCode(result->innoOutcome, !failures.empty());
  Log(failures.empty() ? LogLevel::kInfo : LogLevel::kWarning, kLogComponent,
      L"uninstall installer result=" + std::wstring(CheckResultName(result->code)) + L" failures=" +
          std::to_wstring(failures.size()));
  return S_OK;
}
CATCH_RETURN();

}  // namespace

HRESULT Uninstall(const UninstallOptions& options, const RegistryRoots& roots,
                  const SystemProbes& probes, UninstallResult* result) noexcept try {
  *result = UninstallResult{};
  ProcessContext context;
  if (options.context) {
    context = *options.context;
  } else {
    RETURN_IF_FAILED(probes.GetProcessContext(ContextProbe::kWrite, &context));
  }
  if (context.probe != ContextProbe::kWrite || RefusesWrites(context.virtualization)) {
    result->refusal = UninstallRefusal::kWrongContext;
    result->code = ExitCode::kWrongContext;
    return S_OK;
  }
  if (context.elevated) {
    result->refusal = UninstallRefusal::kElevated;
    result->code = ExitCode::kWrongContext;
    return S_OK;
  }
  std::vector<std::wstring>& failures = result->failures;

  // The installer's install is removed by its own uninstaller, which keeps its records right.
  RETURN_IF_FAILED(FindInnoInstall(roots, &result->inno));
  if (result->inno) return UninstallByInstaller(options, roots, probes, context, result);

  EnginePaths paths;
  if (options.paths) {
    paths = *options.paths;
  } else {
    RETURN_IF_FAILED(GetEnginePaths(&paths));
  }
  std::filesystem::path dataDir = options.dataDir;
  if (dataDir.empty()) RETURN_IF_FAILED(GetDataDir(&dataDir));

  std::optional<OperationLock> lock;
  RETURN_IF_FAILED(
      TakeLock(options.lockWaitMs, &lock, &result->busy, &result->namedObjectsRefused));
  if (result->busy || result->namedObjectsRefused) {
    result->code = result->busy ? ExitCode::kBusy : ExitCode::kFailed;
    return S_OK;
  }
  GatherOptions gather;
  gather.liveTest = false;
  gather.context = context;
  gather.paths = paths;
  Snapshot before;
  RETURN_IF_FAILED(GatherSnapshot(roots, gather, probes, &before));

  bool settingsKey = false;
  RETURN_IF_FAILED(RegistryKeyExists(roots.user, kRegKey, &settingsKey));
  const bool machineRootExists = Exists(paths.machineRoot);
  bool userFiles = !before.userBuilds.empty();
  for (const std::wstring_view relative : kUserRootFiles) {
    if (relative.starts_with(kLicenses)) continue;  // 1.x's too
    if (IsRegularFile(paths.userRoot / relative)) userFiles = true;
  }
  if (!AnythingToUninstall(before, settingsKey, machineRootExists, userFiles,
                           IsRealFolder(dataDir))) {
    result->refusal = UninstallRefusal::kNotInstalled;
    result->code = ExitCode::kNotInstalled;
    return S_OK;
  }
  // Stopped before anything is removed: nothing changed.
  if (options.stop.stop_requested()) {
    result->refusal = UninstallRefusal::kCancelled;
    result->code = ExitCode::kCancelled;
    return S_OK;
  }

  // 1. This user's registration.
  RETURN_IF_FAILED(RemoveUserRegistration(roots, probes, before, &result->user));
  failures.insert(failures.end(), result->user.failures.begin(), result->user.failures.end());

  // 2. The thumbnail cache, now that File Explorer no longer finds the handler for this user. The
  // lock is held, and the snapshot above ran no Explorer test, so this process holds no cache file.
  if (options.rebuildThumbnails) {
    RebuildResult rebuild;
    const std::optional<RebuildPrecheck>& precheck = options.rebuildPrecheck;
    if (precheck && (FAILED(precheck->failure) || precheck->refusal != RebuildRefusal::kNone)) {
      // The command's check refused it, or failed, before its question: nothing is asked again.
      rebuild = RebuildNotStarted(*precheck);
      Log(LogLevel::kWarning, L"refresh", RebuildPrecheckLogText(*precheck));
    } else {
      RebuildOptions rebuildOptions;
      rebuildOptions.context = context;
      const RebuildSystem& system =
          options.rebuildSystem ? *options.rebuildSystem : RealRebuildSystem();
      if (const HRESULT hr = RebuildThumbnailCache(rebuildOptions, system, options.rebuildProgress,
                                                   options.stop, &rebuild);
          FAILED(hr)) {
        // Out of memory before anything changed: not rebuilt, and the uninstall goes on.
        rebuild = RebuildResult{};
        rebuild.failure = hr;
      }
    }
    if (const std::optional<HRESULT> failure = RebuildStepFailure(rebuild)) {
      Failed(&failures, L"rebuild", *failure);
    }
    result->rebuild = std::move(rebuild);
  }

  // 3. The part for all users, by the settings executable installed there: the only one the
  // elevated side accepts.
  result->machinePart = before.machineClass.has_value() || machineRootExists;
  if (result->machinePart) {
    const std::filesystem::path exe = paths.machineRoot / kSettingsExe;
    if (!IsRegularFile(exe)) {
      result->machineNoProgram = true;
    } else if (!options.launcher) {
      Failed(&failures, L"machine", E_INVALIDARG);
    } else {
      ElevatedRun run;
      RETURN_IF_FAILED(
          RunElevated(ElevatedVerb::kMachineUninstall, exe, options.owner, options.launcher, &run));
      // A step that did not get administrator rights, or refused below an app package, has an
      // exit code of its own. One that names its parts lists them instead of the HRESULT it
      // ended with.
      if (NeedsStepFailure(run)) Failed(&failures, L"machine", run.failure);
      for (std::wstring& failure : NotDoneFailures(run, L"machine")) {
        failures.push_back(std::move(failure));
      }
      result->run = std::move(run);
    }
    std::optional<ClassReg> machineClass;
    if (FAILED(ReadClass(roots, Scope::kMachine, &machineClass))) machineClass.reset();
    result->machineLeft = machineClass.has_value() || Exists(paths.machineRoot);
  }

  Log(failures.empty() ? LogLevel::kInfo : LogLevel::kWarning, kLogComponent,
      std::wstring(L"uninstall machine=") +
          (!result->machinePart                               ? L"none"
           : result->run && RefusedBelowPackage(*result->run) ? L"refused"
           : result->machineLeft                              ? L"left"
           : result->run                                      ? L"removed"
                                                              : L"none") +
          L" failures=" + std::to_wstring(failures.size()));

  // 4. This user's files, the log's folder included: nothing is logged after this.
  if (options.dataDir.empty()) DisableLog();
  RETURN_IF_FAILED(RemoveUserFiles(roots, paths.userRoot, dataDir, &result->files));
  failures.insert(failures.end(), result->files.failures.begin(), result->files.failures.end());

  if (!failures.empty()) {
    result->code = ExitCode::kFailed;
  } else if (result->run && LackedAdministratorRights(*result->run)) {
    result->code = ExitCode::kNeedsAdmin;
  } else if (result->run && RefusedBelowPackage(*result->run)) {
    result->code = ExitCode::kWrongContext;
  } else if (result->machineLeft) {
    result->code = ExitCode::kProblems;
  } else {
    result->code = ExitCode::kOk;
  }
  return S_OK;
}
CATCH_RETURN();

// ---- --uninstall-cleanup --------------------------------------------------------------------

HRESULT UninstallCleanup(const UninstallCleanupOptions& options, const RegistryRoots& roots,
                         const SystemProbes& probes, UninstallCleanupResult* result) noexcept try {
  *result = UninstallCleanupResult{};
  std::vector<std::wstring>& failures = result->failures;
  EnginePaths paths;
  if (options.paths) {
    paths = *options.paths;
  } else {
    RETURN_IF_FAILED(GetEnginePaths(&paths));
  }
  std::filesystem::path dataDir = options.dataDir;
  if (dataDir.empty()) RETURN_IF_FAILED(GetDataDir(&dataDir));

  // The part for all users: only elevated, with the administrator rights the elevated side
  // requires of its own token; below an app package, or where the processes above it cannot be
  // read, whose file and registry views could be private copies, only where its own test writes
  // reach Windows.
  bool elevated = false;
  RETURN_IF_FAILED(GetTokenElevation(&elevated));
  if (options.runMachinePart && elevated) {
    bool rights = false;
    const HRESULT read = ReadAdministratorRights(&rights);
    if (FAILED(read) || !rights) {
      // An elevated token alone does not prove these rights; nothing is tested or written then.
      result->machineNoRights = SUCCEEDED(read);
      Failed(&failures, L"machine", FAILED(read) ? read : kNoAdministratorRightsHr);
    } else {
      if (options.machineWrites) {
        result->machineWrites = *options.machineWrites;
      } else {
        CheckMachineWrites(&result->machineWrites);
      }
      if (RefusesMachineWrites(result->machineWrites)) {
        result->machineRefused = true;
        Failed(&failures, L"machine", kBelowPackageHr);
      } else {
        MachineUninstallOptions machine = options.machine;
        if (machine.machineRoot.empty()) machine.machineRoot = paths.machineRoot;
        if (machine.legacyMachineRoot.empty()) machine.legacyMachineRoot = paths.legacyMachineRoot;
        if (const HRESULT hr = UninstallForAllUsers(machine, roots, &result->machine); FAILED(hr)) {
          Failed(&failures, L"machine", hr);
        }
        failures.insert(failures.end(), result->machine.failures.begin(),
                        result->machine.failures.end());
        result->machinePart = true;
      }
    }
  }

  // The per-user part, only where this process's registry writes reach the real hive.
  ProcessContext context;
  RETURN_IF_FAILED(probes.GetProcessContext(ContextProbe::kWrite, &context));
  result->userWrites = context.virtualization;
  result->userProbeError = context.probeError;
  if (context.probe == ContextProbe::kWrite && !RefusesWrites(context.virtualization)) {
    Snapshot snapshot;
    if (context.elevated || elevated) {
      // The removal needs only the context then: it deletes fixed key names and leaves the task
      // to the step for all users. A gathered snapshot would read paths from the user's own
      // registry (the per-user class's DLL) and open those files with administrator rights.
      snapshot.context = context;
    } else {
      GatherOptions gather;
      gather.liveTest = false;
      gather.context = context;
      gather.paths = paths;
      RETURN_IF_FAILED(GatherSnapshot(roots, gather, probes, &snapshot));
    }
    RETURN_IF_FAILED(RemoveUserRegistration(roots, probes, snapshot, &result->user));
    failures.insert(failures.end(), result->user.failures.begin(), result->user.failures.end());
    result->userPart = true;
    // The data folder goes at the next sign-in, in the user's own unelevated session.
    if (IsRealFolder(dataDir)) {
      const std::filesystem::path dirs[] = {dataDir};
      std::vector<std::filesystem::path> notScheduled;
      if (const HRESULT hr = ScheduleRunOnceRemoval(roots, dirs, &notScheduled); FAILED(hr)) {
        Failed(&failures, L"runonce", hr);
      } else if (notScheduled.empty()) {
        result->scheduled.push_back(dataDir);
      }
    }
  }
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
