#include "install.h"

#include <wil/result_macros.h>

#include <algorithm>
#include <iterator>
#include <utility>

#include "failure_trace.h"
#include "instance.h"
#include "legacy.h"
#include "log.h"
#include "paths.h"
#include "pe_info.h"
#include "project_config.h"
#include "registration.h"
#include "repair.h"
#include "strings.h"
#include "tasks.h"

namespace pdft {
namespace {

constexpr wchar_t kLogComponent[] = L"install";

// How long a file in use at the target is retried before it goes to the trash (elevated) or the
// copy fails.
constexpr DWORD kRetryStepMs = 250;

constexpr std::wstring_view kBinaries[] = {kSettingsExe, kCliExe, kHandlerDll, kPdfiumDll};
constexpr std::wstring_view kTopNotices[] = {L"LICENSE.txt", L"THIRD-PARTY-NOTICES.md"};

void Failed(std::vector<std::wstring>* failures, const wchar_t* step, HRESULT hr) {
  failures->push_back(std::wstring(step) + L": " + HResultText(hr));
}

bool IsRegularFile(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

// Makes the folder |dir| unless it is there, and checks that it is a real folder (never a link or
// junction, which would lead the copy somewhere else). A failure's trace detail is the folder's
// own name: the product's, "app", a build's or a notice folder's, never a path.
HRESULT MakeRealFolder(const std::filesystem::path& dir) noexcept try {
  const std::wstring name = dir.filename().native();
  if (!CreateDirectoryW(dir.c_str(), nullptr)) {
    const DWORD error = GetLastError();
    if (error != ERROR_ALREADY_EXISTS) return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(error), name);
  }
  const DWORD attributes = GetFileAttributesW(dir.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(GetLastError()), name);
  }
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK), name);
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_DIRECTORY), name);
  }
  return S_OK;
}
CATCH_RETURN();

// |file| exists as a regular file whose SHA-256 is |sha|.
bool HasContents(const std::filesystem::path& file, const Sha256Digest& sha) noexcept {
  Sha256Digest digest = {};
  return IsRegularFile(file) && SUCCEEDED(Sha256File(file, &digest)) && digest == sha;
}

// |a| and |b| are regular files with the same contents.
bool SameContents(const std::filesystem::path& a, const std::filesystem::path& b) noexcept {
  Sha256Digest digest = {};
  return IsRegularFile(b) && SUCCEEDED(Sha256File(b, &digest)) && HasContents(a, digest);
}

// Every regular file below <base>\<relative>, as paths relative to |base|; links and junctions
// are skipped, and nothing deeper than kMaxNoticeDepth folders is read.
HRESULT CollectNotices(const std::filesystem::path& base, const std::filesystem::path& relative,
                       int depth, std::vector<std::filesystem::path>* found) noexcept try {
  std::vector<FolderEntry> entries;
  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(ListFolderEntries(base / relative, &entries),
                                     relative.filename().native()));
  std::sort(entries.begin(), entries.end(), [](const FolderEntry& a, const FolderEntry& b) {
    return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  for (const FolderEntry& entry : entries) {
    if (entry.reparsePoint) continue;
    if (entry.directory) {
      if (depth < kMaxNoticeDepth) {
        RETURN_IF_FAILED(CollectNotices(base, relative / entry.name, depth + 1, found));
      }
    } else {
      found->push_back(relative / entry.name);
    }
  }
  return S_OK;
}
CATCH_RETURN();

// The architecture of this Windows.
Arch NativeArch() noexcept {
  USHORT process = IMAGE_FILE_MACHINE_UNKNOWN;
  USHORT native = IMAGE_FILE_MACHINE_UNKNOWN;
  if (!IsWow64Process2(GetCurrentProcess(), &process, &native)) return Arch::kOther;
  return ArchFromMachine(native);
}

// |child| is |root| or lies inside it, as written and, where both exist, as Windows resolves
// them.
bool IsRootOrInside(const std::filesystem::path& child, const std::filesystem::path& root) {
  const std::filesystem::path a = child.lexically_normal();
  std::filesystem::path b = root.lexically_normal();
  while (b.has_relative_path() && !b.has_filename()) b = b.parent_path();
  std::filesystem::path c = a;
  while (c.has_relative_path() && !c.has_filename()) c = c.parent_path();
  if (IEquals(c.native(), b.native()) || IsUnderLexically(c, b)) return true;
  std::filesystem::path canonicalChild;
  std::filesystem::path canonicalRoot;
  if (SUCCEEDED(Canonicalize(child, &canonicalChild)) &&
      SUCCEEDED(Canonicalize(root, &canonicalRoot))) {
    return IEquals(canonicalChild.native(), canonicalRoot.native()) ||
           IsUnderLexically(canonicalChild, canonicalRoot);
  }
  return false;
}

// Copies |from| to |to|, retrying while |to| is in use. Still in use after the retry: elevated
// with a trash parent, |to| moves into the trash folder (made once per copy, queued for deletion
// at the next restart) and the copy is made again; otherwise the in-use error.
HRESULT CopyWithRetry(const std::filesystem::path& from, const std::filesystem::path& to,
                      const CopyBuildOptions& options, CopyBuildResult* result) noexcept try {
  const std::wstring name = to.filename().native();
  const ULONGLONG deadline = GetTickCount64() + options.retryMs;
  HRESULT hr = S_OK;
  for (;;) {
    hr = CopyFileVerified(from, to);
    if (SUCCEEDED(hr)) {
      ++result->copied;
      return S_OK;
    }
    if (!IsInUseError(hr)) return PDFT_TRACE_DETAIL(hr, name);
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) break;
    Sleep(static_cast<DWORD>(std::min<ULONGLONG>(kRetryStepMs, deadline - now)));
    NoteHandled();  // in use: tried again, so a new failure is kept where it starts
  }
  bool elevated = false;
  if (options.trashParent.empty() || FAILED(GetTokenElevation(&elevated)) || !elevated) {
    return PDFT_TRACE_DETAIL(hr, name);
  }
  // A running program can be renamed, not deleted or overwritten: it moves out of the way and
  // is deleted at the restart, never queued at its install path, which the new copy takes.
  NoteHandled();
  if (result->trash.empty()) {
    RETURN_IF_FAILED(PDFT_TRACE(MakeTrashFolder(options.trashParent, &result->trash)));
  }
  std::filesystem::path moved;
  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(MoveIntoTrash(to, result->trash, &moved), name));
  result->restartPending = true;
  LOG_IF_FAILED(ScheduleDeleteAtRestart(moved));
  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(CopyFileVerified(from, to), name));
  ++result->copied;
  return S_OK;
}
CATCH_RETURN();

// What is wrong with a build's folder, for the report.
const wchar_t* SourceProblemText(SourceProblem problem) noexcept {
  switch (problem) {
    case SourceProblem::kNone:
      break;
    case SourceProblem::kMissingFile:
      return L"missing";
    case SourceProblem::kWrongArch:
      return L"made for another architecture";
    case SourceProblem::kNot2x:
      return L"not a 2.x build";
    case SourceProblem::kUnreadable:
      return L"unreadable";
  }
  return L"";
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
        L"install result=failed named-objects-refused hr=" + HResultText(hr));
    return S_OK;
  }
  return hr;
}

// Ends this session's thumbnail processes that hold a DLL of a build other than the one
// |activeDll| lies in, so those builds can go and the new one loads.
void EndOldHolders(const Snapshot& before, const std::filesystem::path& activeDll,
                   const SystemProbes& probes, size_t* ended, std::vector<std::wstring>* failures) {
  std::vector<std::filesystem::path> files;
  for (std::filesystem::path& dll : OldBuildDlls(before, activeDll.native())) {
    if (IsRegularFile(dll)) files.push_back(std::move(dll));
  }
  if (files.empty()) return;
  std::vector<Holder> holders;
  if (const HRESULT hr = probes.FindHolders(files, &holders); FAILED(hr)) {
    Failed(failures, L"holders", hr);
    return;
  }
  for (const Holder& holder : holders) {
    if (probes.EndHolder(holder) == S_OK) ++*ended;
  }
}

// One report line per folder of a removal.
void ReportRemoval(ElevatedReporter& report, const MachineRemoval& removal) {
  for (const std::wstring& name : removal.removed) report.Line(L"removed " + name);
  for (const std::wstring& name : removal.left) report.Line(L"in use, left " + name);
  for (const std::wstring& name : removal.failed) report.Line(L"failed " + name);
}

}  // namespace

std::span<const std::wstring_view> BuildBinaries() noexcept { return kBinaries; }

std::span<const std::wstring_view> BuildTopNotices() noexcept { return kTopNotices; }

bool IsInstallableVersion(std::wstring_view version) noexcept {
  return IsBuildVersion(version) && CompareFileVersions(version, L"2") >= 0;
}

HRESULT ReadBuildSource(const std::filesystem::path& dir, Arch osArch, BuildSource* source,
                        SourceCheck* check) noexcept try {
  *source = BuildSource{};
  *check = SourceCheck{};
  RETURN_IF_FAILED(PDFT_TRACE(Canonicalize(dir, &source->dir)));
  for (const std::wstring_view name : kBinaries) {
    const std::filesystem::path file = source->dir / name;
    if (!IsRegularFile(file)) {
      check->problem = SourceProblem::kMissingFile;
      check->file = name;
      return S_OK;
    }
    USHORT machine = 0;
    if (const HRESULT hr = ReadPeMachine(file, &machine); FAILED(hr)) {
      check->problem = SourceProblem::kUnreadable;
      check->file = name;
      check->hr = hr;
      return S_OK;
    }
    const Arch arch = MachineToArch(machine);
    if (arch != osArch || osArch == Arch::kOther) {
      check->problem = SourceProblem::kWrongArch;
      check->file = name;
      check->arch = arch;
      return S_OK;
    }
  }
  const std::filesystem::path dll = source->dir / kHandlerDll;
  const std::filesystem::path pdfium = source->dir / kPdfiumDll;
  if (const HRESULT hr = ReadFileVersionString(dll, &source->version); FAILED(hr)) {
    check->problem = SourceProblem::kUnreadable;
    check->file = kHandlerDll;
    check->hr = hr;
    return S_OK;
  }
  if (!IsInstallableVersion(source->version)) {
    check->problem = SourceProblem::kNot2x;
    check->file = kHandlerDll;
    return S_OK;
  }
  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(Sha256File(dll, &source->dllSha), kHandlerDll));
  RETURN_IF_FAILED(PDFT_TRACE_DETAIL(Sha256File(pdfium, &source->pdfiumSha), kPdfiumDll));
  RETURN_IF_FAILED(PDFT_TRACE(
      BuildIdFromHashes(source->version, source->dllSha, source->pdfiumSha, &source->buildName)));
  for (const std::wstring_view name : kTopNotices) {
    if (IsRegularFile(source->dir / name)) source->notices.emplace_back(name);
  }
  const DWORD licenses = GetFileAttributesW((source->dir / kLicensesFolder).c_str());
  if (licenses != INVALID_FILE_ATTRIBUTES && (licenses & FILE_ATTRIBUTE_DIRECTORY) &&
      !(licenses & FILE_ATTRIBUTE_REPARSE_POINT)) {
    RETURN_IF_FAILED(PDFT_TRACE(CollectNotices(source->dir, kLicensesFolder, 1, &source->notices)));
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT CopyBuild(const BuildSource& source, const std::filesystem::path& root,
                  const CopyBuildOptions& options, CopyBuildResult* result) noexcept try {
  *result = CopyBuildResult{};
  if (!root.is_absolute() || source.buildName.empty() || source.dir.empty()) {
    RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  }
  RETURN_IF_FAILED(PDFT_TRACE(MakeRealFolder(root)));
  const std::filesystem::path app = root / kBuildsFolderName;
  RETURN_IF_FAILED(PDFT_TRACE(MakeRealFolder(app)));
  result->buildDir = app / source.buildName;
  result->dll = result->buildDir / kHandlerDll;

  // The DLLs first: a folder of this name with exactly these DLLs is the same build.
  const struct {
    const wchar_t* name;
    const Sha256Digest* sha;
  } dlls[] = {{kHandlerDll, &source.dllSha}, {kPdfiumDll, &source.pdfiumSha}};
  RETURN_IF_FAILED(PDFT_TRACE(MakeRealFolder(result->buildDir)));
  result->reused = true;
  for (const auto& dll : dlls) {
    const std::filesystem::path target = result->buildDir / dll.name;
    if (HasContents(target, *dll.sha)) continue;
    result->reused = false;
    Sha256Digest copied = {};
    RETURN_IF_FAILED(
        PDFT_TRACE_DETAIL(CopyFileVerified(source.dir / dll.name, target, &copied), dll.name));
    ++result->copied;
    if (copied != *dll.sha) {
      // The file changed after the build id was computed from it: never install that.
      DeleteFileW(target.c_str());
      return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_FILE_INVALID), dll.name);
    }
  }

  // The executables and the notices at the top.
  std::vector<std::filesystem::path> files = {kSettingsExe, kCliExe};
  files.insert(files.end(), source.notices.begin(), source.notices.end());
  for (const std::filesystem::path& relative : files) {
    std::filesystem::path folder = root;
    for (const std::filesystem::path& part : relative.parent_path()) {
      folder /= part;
      RETURN_IF_FAILED(PDFT_TRACE(MakeRealFolder(folder)));
    }
    const std::filesystem::path from = source.dir / relative;
    const std::filesystem::path to = root / relative;
    if (SameContents(from, to)) continue;
    RETURN_IF_FAILED(
        PDFT_TRACE_DETAIL(CopyWithRetry(from, to, options, result), relative.filename().native()));
  }
  // The trash folder goes once its files went.
  if (!result->trash.empty()) LOG_IF_FAILED(ScheduleDeleteAtRestart(result->trash));
  return S_OK;
}
CATCH_RETURN();

HRESULT InstallForAllUsers(const MachineInstallOptions& options, const RegistryRoots& roots,
                           MachineInstallResult* result) noexcept try {
  *result = MachineInstallResult{};
  // The parts of the step, for the report of where it failed (failure_trace.h).
  TraceStep step(L"arguments");
  if (!options.machineRoot.is_absolute() || !options.machineRoot.has_relative_path() ||
      !options.source.is_absolute()) {
    RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  }
  const std::filesystem::path trashParent =
      options.trashParent.empty() ? options.machineRoot.parent_path() : options.trashParent;

  // Checked again here: the unelevated side checked before asking, and the folder could have
  // changed since.
  if (options.checkSourceAccess) {
    step.Set(L"folder-access");
    const std::vector<std::wstring> binaries(kBinaries, kBinaries + std::size(kBinaries));
    RETURN_IF_FAILED(
        PDFT_TRACE(FindUnsafeBuildFolderAccess(options.source, binaries, &result->unsafe)));
    if (result->unsafe) return PDFT_TRACE(kUnsafeBuildFolderHr);
  }
  step.Set(L"read-build");
  RETURN_IF_FAILED(PDFT_TRACE(
      ReadBuildSource(options.source, options.osArch, &result->source, &result->sourceCheck)));
  if (result->sourceCheck.problem != SourceProblem::kNone) {
    return PDFT_TRACE_DETAIL(HRESULT_FROM_WIN32(ERROR_BAD_FORMAT), result->sourceCheck.file);
  }
  step.Set(L"machine-root");
  if (IsRootOrInside(result->source.dir, options.machineRoot)) RETURN_HR(PDFT_TRACE(E_INVALIDARG));
  RETURN_IF_FAILED(PDFT_TRACE(CheckNoReparsePoints(options.machineRoot, {})));

  step.Set(L"copy-build");
  CopyBuildOptions copy;
  copy.trashParent = trashParent;
  copy.retryMs = options.retryMs;
  RETURN_IF_FAILED(PDFT_TRACE(CopyBuild(result->source, options.machineRoot, copy, &result->copy)));
  // Again now that every folder on the way exists.
  step.Set(L"build-links");
  RETURN_IF_FAILED(PDFT_TRACE(CheckNoReparsePoints(result->copy.buildDir, {})));
  step.Set(L"machine-class");
  RETURN_IF_FAILED(PDFT_TRACE(WriteMachineClass(roots, options.machineRoot, result->copy.dll)));
  // From here on failures are listed, not returned.
  step.Set(L"after-class");

  std::vector<std::wstring>& failures = result->failures;
  if (!options.taskFolder.empty()) {
    result->taskFolder = EnsureTaskFolder(options.taskFolder);
    if (FAILED(result->taskFolder)) Failed(&failures, L"task-folder", result->taskFolder);
  }
  // Last: the old builds, once the class points at the new one.
  if (!options.legacyMachineRoot.empty()) {
    if (const HRESULT hr =
            MigrateV1Machine(roots, options.legacyMachineRoot, trashParent, &result->v1);
        FAILED(hr)) {
      Failed(&failures, L"migrate-v1", hr);
    }
    if (FAILED(result->v1.firstError)) Failed(&failures, L"migrate-v1", result->v1.firstError);
  }
  if (const HRESULT hr =
          RemoveOldMachineBuilds(roots, options.machineRoot, trashParent, &result->old);
      FAILED(hr)) {
    Failed(&failures, L"old-builds", hr);
  }
  if (FAILED(result->old.firstError)) Failed(&failures, L"old-builds", result->old.firstError);
  result->restartPending =
      result->copy.restartPending || result->v1.restartPending || result->old.restartPending;
  NoteHandled();  // the failures since the class are listed, not returned
  return S_OK;
}
CATCH_RETURN();

HRESULT RepairMachineClass(const RegistryRoots& roots, const std::filesystem::path& machineRoot,
                           Arch osArch, std::filesystem::path* dll) noexcept try {
  if (dll) dll->clear();
  std::vector<BuildInfo> builds;
  RETURN_IF_FAILED(ListBuilds(machineRoot / kBuildsFolderName, &builds));
  const std::optional<std::filesystem::path> chosen = ChooseBuildDll(builds, osArch);
  if (!chosen) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  RETURN_IF_FAILED(CheckNoReparsePoints(chosen->parent_path(), {}));
  RETURN_IF_FAILED(WriteMachineClass(roots, machineRoot, *chosen));
  if (dll) *dll = *chosen;
  return S_OK;
}
CATCH_RETURN();

HRESULT RunMachineInstall(ElevatedReporter& report, bool* restartPending) noexcept try {
  *restartPending = false;
  TraceStep step(L"own-paths");
  std::filesystem::path own;
  RETURN_IF_FAILED(PDFT_TRACE(GetOwnExe(&own)));
  MachineInstallOptions options;
  options.source = own.parent_path();
  RETURN_IF_FAILED(PDFT_TRACE(GetMachineRoot(&options.machineRoot)));
  RETURN_IF_FAILED(PDFT_TRACE(GetLegacyMachineRoot(&options.legacyMachineRoot)));
  options.osArch = NativeArch();
  MachineInstallResult result;
  // InstallForAllUsers names its own parts; this one covers what it returns untraced.
  step.Set(L"install");
  const HRESULT hr = PDFT_TRACE(InstallForAllUsers(options, RegistryRoots{}, &result));
  // No paths: the build's folder lies in the user's profile, and the parent logs these lines.
  if (result.unsafe) {
    report.Line(L"the build's folder (or a folder above it) can be changed by " +
                result.unsafe->access.principal);
  }
  if (result.sourceCheck.problem != SourceProblem::kNone) {
    std::wstring line = L"not a build to install: " + result.sourceCheck.file + L" (" +
                        SourceProblemText(result.sourceCheck.problem);
    if (FAILED(result.sourceCheck.hr)) line += L", hr=" + HResultText(result.sourceCheck.hr);
    report.Line(line + L")");
  }
  if (!result.copy.buildDir.empty() && SUCCEEDED(hr)) {
    report.Line((result.copy.reused ? L"reused app\\" : L"copied app\\") + result.source.buildName +
                L" (" + std::to_wstring(result.copy.copied) + L" files written)");
    report.Line(L"class for all users -> app\\" + result.source.buildName);
  }
  if (result.copy.restartPending) report.Line(L"a program in use went to a trash folder");
  if (FAILED(result.taskFolder)) {
    report.Line(L"task folder failed hr=" + HResultText(result.taskFolder));
  }
  ReportRemoval(report, result.v1);
  ReportRemoval(report, result.old);
  *restartPending = result.restartPending;
  return hr;
}
CATCH_RETURN();

HRESULT RunMachineRepairClass(ElevatedReporter& report) noexcept try {
  TraceStep step(L"repair-class");
  std::filesystem::path root;
  RETURN_IF_FAILED(PDFT_TRACE(GetMachineRoot(&root)));
  std::filesystem::path dll;
  const HRESULT hr = PDFT_TRACE(RepairMachineClass(RegistryRoots{}, root, NativeArch(), &dll));
  if (SUCCEEDED(hr)) {
    report.Line(L"class for all users -> app\\" + dll.parent_path().filename().native());
  } else if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
    report.Line(L"no complete build for all users to point the class at");
  }
  return hr;
}
CATCH_RETURN();

std::optional<ExitCode> InstallStepExitCode(ElevatedOutcome outcome, HRESULT failure) noexcept {
  switch (outcome) {
    case ElevatedOutcome::kDone:
    case ElevatedOutcome::kRestartPending:
      return std::nullopt;
    case ElevatedOutcome::kCancelled:
      return ExitCode::kCancelled;
    case ElevatedOutcome::kFailed:
      break;
  }
  if (failure == kUnsafeBuildFolderHr) return ExitCode::kWrongContext;
  if (failure == kNoAdministratorRightsHr) return ExitCode::kNeedsAdmin;
  return ExitCode::kFailed;
}

HRESULT InstallFromOwnFolder(const InstallOptions& options, const RegistryRoots& roots,
                             const SystemProbes& probes, InstallResult* result) noexcept try {
  *result = InstallResult{};
  result->scope = options.scope;
  const auto refuse = [result](InstallRefusal refusal) {
    result->refusal = refusal;
    result->code = ExitCode::kWrongContext;
    return S_OK;
  };

  ProcessContext context;
  if (options.context) {
    context = *options.context;
  } else {
    RETURN_IF_FAILED(probes.GetProcessContext(ContextProbe::kWrite, &context));
  }
  if (context.probe != ContextProbe::kWrite || RefusesWrites(context.virtualization)) {
    return refuse(InstallRefusal::kWrongContext);
  }
  // The per-user half belongs to the signed-in user; the half for all users asks for an
  // administrator itself.
  if (context.elevated) return refuse(InstallRefusal::kElevated);
  RETURN_IF_FAILED(FindInnoInstall(roots, &result->inno));
  if (result->inno) return refuse(InstallRefusal::kInnoInstall);

  std::filesystem::path source = options.source;
  if (source.empty()) {
    std::filesystem::path own;
    RETURN_IF_FAILED(GetOwnExe(&own));
    source = own.parent_path();
  }
  RETURN_IF_FAILED(ReadBuildSource(source, context.osArch, &result->source, &result->sourceCheck));
  if (result->sourceCheck.problem != SourceProblem::kNone) {
    return refuse(InstallRefusal::kNotABuild);
  }
  EnginePaths paths;
  if (options.paths) {
    paths = *options.paths;
  } else {
    RETURN_IF_FAILED(GetEnginePaths(&paths));
  }

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

  // 1.x's rule: a class for all users makes this an install for all users, unless that class is
  // 1.x's own, which a per-user install would not replace (the thumbnail process prefers it).
  Scope scope = options.scope;
  if (scope == Scope::kUser && before.machineClass) {
    if (PerUserInstallBlockedByV1Machine(before.legacy)) {
      if (!options.force) return refuse(InstallRefusal::kV1Machine);
    } else {
      scope = Scope::kMachine;
      result->switchedToMachine = true;
    }
  }
  result->scope = scope;
  const std::filesystem::path root = scope == Scope::kMachine ? paths.machineRoot : paths.userRoot;
  if (IsRootOrInside(result->source.dir, root)) return refuse(InstallRefusal::kInsideRoot);
  const std::filesystem::path targetDll =
      root / kBuildsFolderName / result->source.buildName / kHandlerDll;
  std::vector<std::wstring>& failures = result->failures;
  // Either scope: what another account put in the folder would be registered for all users, or
  // loaded by this user's thumbnail process and started by their sign-in task.
  {
    const std::vector<std::wstring> binaries(kBinaries, kBinaries + std::size(kBinaries));
    RETURN_IF_FAILED(FindUnsafeBuildFolderAccess(result->source.dir, binaries, &result->unsafe));
    if (result->unsafe) return refuse(InstallRefusal::kUnsafeFolder);
  }

  if (scope == Scope::kMachine) {
    // Before the step: the step removes the old builds last, which only works once nothing of
    // this session holds them.
    EndOldHolders(before, targetDll, probes, &result->holdersEnded, &failures);
    if (!options.launcher) {
      Failed(&failures, L"machine-install", E_INVALIDARG);
      result->code = ExitCode::kFailed;
      return S_OK;
    }
    ElevatedRun run;
    RETURN_IF_FAILED(RunElevated(ElevatedVerb::kMachineInstall, result->source.dir / kSettingsExe,
                                 options.owner, options.launcher, &run));
    const std::optional<ExitCode> stop = InstallStepExitCode(run.outcome, run.failure);
    result->restartPending = run.outcome == ElevatedOutcome::kRestartPending;
    result->run = std::move(run);
    if (stop) {
      if (*stop == ExitCode::kWrongContext) result->refusal = InstallRefusal::kUnsafeFolder;
      if (*stop == ExitCode::kFailed) Failed(&failures, L"machine-install", result->run->failure);
      result->code = *stop;
      Log(LogLevel::kWarning, kLogComponent,
          L"install scope=machine result=" + std::wstring(CheckResultName(*stop)));
      return S_OK;
    }
  } else {
    // A pending sign-in clean-up (1.x's, or one an uninstall left) would delete the per-user
    // folder this install is about to fill: it goes before anything is copied there.
    if (const HRESULT hr = DeleteRunOnceRemovals(roots, &result->runOnceDeleted); FAILED(hr)) {
      Failed(&failures, L"runonce", hr);
    }
    // The files 1.x left in the per-user folder go before anything of this version is copied
    // there: once this version's files are in it, 1.x's shared names could be either's.
    if (const HRESULT hr =
            RemoveV1UserFiles(roots, probes, before, &result->v1Plan, &result->v1Files);
        FAILED(hr)) {
      Failed(&failures, L"v1-files", hr);
    }
    CopyBuildOptions copy;
    if (const HRESULT hr = CopyBuild(result->source, root, copy, &result->userCopy); FAILED(hr)) {
      Failed(&failures, L"copy", hr);
      result->code = ExitCode::kFailed;
      return S_OK;
    }
    if (const HRESULT hr = WriteClass(roots, Scope::kUser, result->userCopy.dll); FAILED(hr)) {
      Failed(&failures, L"class", hr);
      result->code = ExitCode::kFailed;
      return S_OK;
    }
  }

  // The per-user step, under the lock this process holds.
  SetupUserOptions setup;
  setup.checks = options.checks;
  setup.zoom = options.zoom;
  setup.context = context;
  setup.paths = paths;
  setup.takeLock = false;
  RETURN_IF_FAILED(SetupUser(setup, roots, probes, &result->setup));
  if (result->setup.refusal != SetupRefusal::kNone) {
    Failed(&failures, L"setup-user", E_UNEXPECTED);
  }
  failures.insert(failures.end(), result->setup.failures.begin(), result->setup.failures.end());

  GatherOptions final;
  final.liveTest = options.liveTest;
  final.context = context;
  final.paths = paths;
  RETURN_IF_FAILED(GatherSnapshot(roots, final, probes, &result->after));
  result->findings = Evaluate(result->after);
  const ExitCode status = ExitCodeFromFindings(result->findings);
  if (status == ExitCode::kNotInstalled) {
    result->code = ExitCode::kFailed;
  } else if (status == ExitCode::kOk && !failures.empty()) {
    result->code = ExitCode::kProblems;
  } else {
    result->code = status;
  }
  Log(failures.empty() ? LogLevel::kInfo : LogLevel::kWarning, kLogComponent,
      std::wstring(L"install scope=") + (scope == Scope::kMachine ? L"machine" : L"user") +
          L" build=" + result->source.buildName + L" result=" + CheckResultName(result->code) +
          L" holders-ended=" + std::to_wstring(result->holdersEnded) + L" restart-pending=" +
          (result->restartPending ? L"yes" : L"no") + L" failures=" +
          std::to_wstring(failures.size()));
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
