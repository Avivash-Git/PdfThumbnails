#include "snapshot.h"

#include <wil/result_macros.h>

#include <algorithm>
#include <utility>

#include "file_ops.h"
#include "paths.h"
#include "pe_info.h"
#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

// The trash folders next to the machine-wide install root |machineRoot| (real folders only).
HRESULT ListPendingTrash(const std::filesystem::path& machineRoot,
                         std::vector<std::wstring>* names) noexcept try {
  names->clear();
  if (!machineRoot.has_relative_path()) return S_OK;
  std::vector<FolderEntry> entries;
  RETURN_IF_FAILED(ListFolderEntries(machineRoot.parent_path(), &entries));
  for (const FolderEntry& entry : entries) {
    if (entry.directory && !entry.reparsePoint && IsTrashFolderName(entry.name)) {
      names->push_back(entry.name);
    }
  }
  return S_OK;
}
CATCH_RETURN();

// |path| normalized, without a trailing separator, for comparing folders as text.
std::filesystem::path NormalFolder(const std::filesystem::path& path) {
  std::filesystem::path normal = path.lexically_normal();
  if (normal.has_relative_path() && normal.filename().empty()) normal = normal.parent_path();
  return normal;
}

// A complete 2.x build folder: what a per-user class may point at and what counts as an old
// build. 1.x folders and folders whose names are not build names are never touched.
bool IsComplete2xBuild(const BuildFolder& folder) noexcept {
  return folder.complete && !folder.is1x && !folder.version.empty();
}

}  // namespace

HRESULT ListBuilds(const std::filesystem::path& appDir, std::vector<BuildInfo>* builds) noexcept
    try {
  builds->clear();
  std::vector<BuildFolder> folders;
  RETURN_IF_FAILED(ListBuildFolders(appDir, &folders));
  for (BuildFolder& folder : folders) {
    BuildInfo info;
    if (IsComplete2xBuild(folder)) {
      USHORT machine = 0;
      // A DLL whose headers cannot be read is never chosen; that is all this decides.
      if (SUCCEEDED(ReadPeMachine(folder.dir / kHandlerDll, &machine))) info.dllMachine = machine;
    }
    info.folder = std::move(folder);
    builds->push_back(std::move(info));
  }
  return S_OK;
}
CATCH_RETURN();

namespace {

// |path| in its long form, or as it is when that cannot be read.
std::filesystem::path LongPathOr(const std::filesystem::path& path) {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD needed = GetLongPathNameW(path.c_str(), buffer, ARRAYSIZE(buffer));
  if (needed > 0 && needed < ARRAYSIZE(buffer)) return buffer;
  if (needed >= ARRAYSIZE(buffer)) {
    std::wstring longer(needed, L'\0');
    const DWORD written = GetLongPathNameW(path.c_str(), longer.data(), needed);
    if (written > 0 && written < needed) {
      longer.resize(written);
      return longer;
    }
  }
  return path;
}

void AddIssue(Snapshot* snapshot, const wchar_t* part, HRESULT hr) {
  snapshot->issues.push_back(GatherIssue{part, hr});
}

// Describes |clsid| when it names something other than our handler; a failure only costs the
// description.
void DescribeIfForeign(const RegistryRoots& roots, const std::optional<std::wstring>& clsid,
                       ClsidDescription* description, Snapshot* snapshot) {
  if (SlotIsEmpty(clsid) || IsOurClsid(*clsid)) return;
  if (const HRESULT hr = DescribeClsid(roots, *clsid, description); FAILED(hr)) {
    AddIssue(snapshot, L"describe-clsid", hr);
  }
}

}  // namespace

HRESULT GetEnginePaths(EnginePaths* paths) noexcept try {
  *paths = {};
  RETURN_IF_FAILED(GetUserRoot(&paths->userRoot));
  RETURN_IF_FAILED(GetMachineRoot(&paths->machineRoot));
  RETURN_IF_FAILED(GetLegacyUserRoot(&paths->legacyUserRoot));
  RETURN_IF_FAILED(GetLegacyMachineRoot(&paths->legacyMachineRoot));
  RETURN_IF_FAILED(LegacyIniPath(&paths->legacyIni));
  RETURN_IF_FAILED(GetOwnExe(&paths->ownExe));
  // Started through an 8.3 path (a profile folder named "ABCDEF~1"), the program's path would
  // show a short form of the user's name, which the report would then have to mask as well.
  paths->ownExe = LongPathOr(paths->ownExe);
  return S_OK;
}
CATCH_RETURN();

WindowsChecksSkip DecideWindowsChecksSkip(const ProcessContext& context) noexcept {
  switch (context.probe) {
    case ContextProbe::kWrite:
      return RefusesWrites(context.virtualization) ? WindowsChecksSkip::kVirtualized
                                                   : WindowsChecksSkip::kNone;
    case ContextProbe::kReadOnly:
      switch (context.views) {
        case RegistryViews::kAgree:
          return WindowsChecksSkip::kNone;
        case RegistryViews::kDiffer:
          return WindowsChecksSkip::kViewsDiffer;
        case RegistryViews::kUnknown:
          return WindowsChecksSkip::kViewsUnknown;
        case RegistryViews::kNotChecked:
          break;
      }
      break;
    case ContextProbe::kNone:
      break;
  }
  // No check ran: nothing redirects the registry of a process that no packaged app started.
  return context.packagedAncestor ? WindowsChecksSkip::kNotChecked : WindowsChecksSkip::kNone;
}

const ClassReg* Snapshot::ActiveClass() const noexcept {
  switch (scope) {
    case Scope::kMachine:
      return machineClass ? &*machineClass : nullptr;
    case Scope::kUser:
      return userClass ? &*userClass : nullptr;
    case Scope::kNone:
      break;
  }
  return nullptr;
}

bool Snapshot::EffectiveIsOurs() const noexcept {
  return !windowsChecksSkipped && effective && IsOurClsid(*effective);
}

Scope DecideScope(const std::optional<ClassReg>& machineClass,
                  const std::optional<ClassReg>& userClass) noexcept {
  if (machineClass) return Scope::kMachine;
  if (userClass) return Scope::kUser;
  return Scope::kNone;
}

bool DllIsInFolder(const std::filesystem::path& dll, const std::filesystem::path& folder) {
  if (dll.empty() || folder.empty()) return false;
  const std::filesystem::path parent = NormalFolder(dll.lexically_normal().parent_path());
  return IEquals(parent.native(), NormalFolder(folder).native());
}

bool UserInstallOverV1Machine(const std::optional<ClassReg>& machineClass,
                              const std::optional<ClassReg>& userClass, const EnginePaths& paths) {
  // The per-user class's path follows the rule a machine-wide class of this version follows, in
  // the per-user root.
  return machineClass && userClass && !paths.legacyMachineRoot.empty() &&
         IsInV1BuildFolder(machineClass->dll, paths.legacyMachineRoot) &&
         IsMachineClassPath(paths.userRoot, userClass->dll);
}

std::optional<std::filesystem::path> InstalledSettingsExe(const Snapshot& snapshot) {
  if (snapshot.scope == Scope::kMachine && !snapshot.settingsExeFound &&
      snapshot.userSettingsExeFound && snapshot.paths.userRoot.is_absolute() &&
      UserInstallOverV1Machine(snapshot.machineClass, snapshot.userClass, snapshot.paths)) {
    return NormalFolder(snapshot.paths.userRoot) / kSettingsExe;
  }
  if (!snapshot.settingsExeFound || snapshot.installRoot.empty()) return std::nullopt;
  const std::filesystem::path& root = snapshot.scope == Scope::kMachine ? snapshot.paths.machineRoot
                                      : snapshot.scope == Scope::kUser  ? snapshot.paths.userRoot
                                                                        : std::filesystem::path();
  if (root.empty() || !root.is_absolute() ||
      !IEquals(NormalFolder(root).native(), NormalFolder(snapshot.installRoot).native())) {
    return std::nullopt;
  }
  return NormalFolder(root) / kSettingsExe;
}

std::optional<std::filesystem::path> ChooseUserBuildDll(const Snapshot& snapshot) {
  return ChooseBuildDll(snapshot.userBuilds, snapshot.context.osArch);
}

std::optional<std::filesystem::path> ChooseBuildDll(std::span<const BuildInfo> builds,
                                                    Arch osArch) {
  const BuildInfo* best = nullptr;
  std::wstring bestId;
  for (const BuildInfo& build : builds) {
    if (!IsComplete2xBuild(build.folder) || !build.dllMachine ||
        MachineToArch(*build.dllMachine) != osArch || osArch == Arch::kOther) {
      continue;
    }
    const std::optional<BuildFolderNameParts> parts = ParseBuildFolderName(build.folder.name);
    if (!parts) continue;
    if (best) {
      const int byVersion = CompareFileVersions(parts->version, best->folder.version);
      if (byVersion < 0 || (byVersion == 0 && parts->id <= bestId)) continue;
    }
    best = &build;
    bestId = parts->id;
  }
  if (!best) return std::nullopt;
  return best->folder.dir / kHandlerDll;
}

HRESULT GatherSnapshot(const RegistryRoots& roots, const GatherOptions& options,
                       const SystemProbes& probes, Snapshot* snapshot) noexcept try {
  *snapshot = Snapshot{};
  Snapshot& s = *snapshot;
  if (options.context) {
    s.context = *options.context;
  } else {
    RETURN_IF_FAILED(probes.GetProcessContext(ContextProbe::kReadOnly, &s.context));
  }

  // Own executable, install roots and build folders.
  if (options.paths) {
    s.paths = *options.paths;
  } else {
    RETURN_IF_FAILED(GetEnginePaths(&s.paths));
  }
  RETURN_IF_FAILED(ReadOsBuild(roots, &s.osBuild));
  RETURN_IF_FAILED(ListBuilds(s.paths.userRoot / kBuildsFolderName, &s.userBuilds));
  RETURN_IF_FAILED(ListBuilds(s.paths.machineRoot / kBuildsFolderName, &s.machineBuilds));
  if (const HRESULT hr = ListPendingTrash(s.paths.machineRoot, &s.pendingTrash); FAILED(hr)) {
    s.pendingTrash.clear();
    AddIssue(&s, L"trash", hr);
  }

  // Both classes, both slots (and the machine-wide ones, for information), then the scope.
  RETURN_IF_FAILED(ReadClass(roots, Scope::kMachine, &s.machineClass));
  RETURN_IF_FAILED(ReadClass(roots, Scope::kUser, &s.userClass));
  RETURN_IF_FAILED(ReadSlots(roots, &s.slots, &s.machineSlots));
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    DescribeIfForeign(roots, s.slots[slot], &s.slotOwners[slot], &s);
  }
  s.scope = DecideScope(s.machineClass, s.userClass);
  if (s.scope == Scope::kMachine) s.installRoot = s.paths.machineRoot;
  if (s.scope == Scope::kUser) s.installRoot = s.paths.userRoot;
  if (!s.installRoot.empty()) {
    const DWORD attributes = GetFileAttributesW((s.installRoot / kSettingsExe).c_str());
    s.settingsExeFound =
        attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
  }
  if (s.scope == Scope::kMachine &&
      UserInstallOverV1Machine(s.machineClass, s.userClass, s.paths)) {
    const DWORD attributes = GetFileAttributesW((s.paths.userRoot / kSettingsExe).c_str());
    s.userSettingsExeFound =
        attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
  }

  // What Windows itself resolves, unless this process may see another registry than Explorer.
  s.windowsChecksSkip = DecideWindowsChecksSkip(s.context);
  s.windowsChecksSkipped = s.windowsChecksSkip != WindowsChecksSkip::kNone;
  if (!s.windowsChecksSkipped) {
    if (const HRESULT hr = QueryEffectiveHandler(probes.Associations(), &s.effective); FAILED(hr)) {
      AddIssue(&s, L"effective", hr);
    }
    DescribeIfForeign(roots, s.effective, &s.effectiveOwner, &s);
    if (const HRESULT hr = QueryDefaultApp(roots, probes.Associations(), &s.defaultApp);
        FAILED(hr)) {
      AddIssue(&s, L"default-app", hr);
    }
    DescribeIfForeign(roots, s.defaultApp.handler, &s.defaultAppHandlerOwner, &s);
  }

  RETURN_IF_FAILED(ReadExplorerSettings(roots, &s.explorer));
  RETURN_IF_FAILED(LoadSettingsWithIni(roots, s.paths.legacyIni, &s.settings, &s.zoom));

  if (const HRESULT hr = DefaultTaskLocation(s.context.userSid, &s.taskLocation); FAILED(hr)) {
    AddIssue(&s, L"task-location", hr);
  } else if (const HRESULT read = probes.ReadTask(s.taskLocation, &s.task); FAILED(read)) {
    s.task = {};
    AddIssue(&s, L"task", read);
  }

  if (const HRESULT hr = probes.DetectPdfCraft(&s.pdfcraft); FAILED(hr)) {
    s.pdfcraft = {};
    AddIssue(&s, L"pdfcraft", hr);
  }
  RETURN_IF_FAILED(ReadCheck(roots, &s.last));
  RETURN_IF_FAILED(ReadMigratedFromV1(roots, &s.migratedFromV1));

  if (const HRESULT hr = probes.ReadSacState(roots, &s.sac); FAILED(hr)) {
    s.sac = {};
    s.sac.state = SacState::kUnknown;
    AddIssue(&s, L"sac", hr);
  }

  RETURN_IF_FAILED(
      ReadLegacyState(roots, s.paths.legacyUserRoot, s.paths.legacyMachineRoot, &s.legacy));
  bool legacyTask = false;
  if (const HRESULT hr = probes.LegacyTaskExists(&legacyTask); FAILED(hr)) {
    legacyTask = false;
    AddIssue(&s, L"legacy-task", hr);
  }
  s.legacy.legacyTask = legacyTask;

  // The Explorer test, only where its answer says something about Explorer.
  LiveTestConditions conditions;
  conditions.requested = options.liveTest;
  conditions.elevated = s.context.elevated;
  conditions.virtualized = s.windowsChecksSkipped;
  conditions.effectiveIsOurs = s.EffectiveIsOurs();
  conditions.classRegistered = s.scope != Scope::kNone;
  s.liveSkip = DecideLiveTestSkip(conditions);
  if (s.liveSkip == LiveTestSkip::kNone) {
    LiveTest live;
    const HRESULT hr = probes.RunLiveTest(kProbeThumbnailSize, nullptr, kLiveTestTimeoutMs, &live);
    if (SUCCEEDED(hr)) {
      s.live = live;
    } else {
      s.liveError = hr;
      AddIssue(&s, L"live-test", hr);
    }
  }

  if (options.testFile) {
    TestFileRun run;
    run.requested = *options.testFile;
    (void)ValidateTestFile(run.requested, &run.resolved, &run.problem);
    if (run.problem == TestFileProblem::kNone) {
      conditions.requested = true;
      conditions.testFile = true;
      run.skip = DecideLiveTestSkip(conditions);
      if (run.skip == LiveTestSkip::kNone) {
        LiveTest result;
        const HRESULT hr =
            probes.RunLiveTest(kTestFileThumbnailSize, &run.resolved, kLiveTestTimeoutMs, &result);
        if (SUCCEEDED(hr)) {
          run.result = result;
        } else {
          run.error = hr;
          AddIssue(&s, L"test-file", hr);
        }
      }
    }
    s.testFile = std::move(run);
  }

  // Code Integrity blocks of the registered build, only when the verdict needs them.
  const std::optional<HRESULT> probe =
      s.live ? std::optional<HRESULT>(s.live->hr) : std::optional<HRESULT>();
  const ClassReg* active = s.ActiveClass();
  if (ShouldCountBlocks(s.sac.state, active != nullptr && active->dllExists, probe)) {
    const std::filesystem::path buildFolder = std::filesystem::path(active->dll).parent_path();
    s.blocks = probes.CountCiBlocks(buildFolder, kSacBlockDays);
    if (ShouldRecountBlocks(probe, *s.blocks)) {
      // A block the Explorer test itself caused reaches the log within about a second.
      probes.Wait(kSacRecountDelayMs);
      s.blocks = probes.CountCiBlocks(buildFolder, kSacBlockDays);
    }
  }
  s.sacVerdict = DecideSac(s.sac.state, probe, s.blocks);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
