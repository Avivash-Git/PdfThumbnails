// The check end to end (snapshot.h, evaluate.h, repair.h, check.h): gathering, judging,
// repairing and recording against a private registry key (ScopedRegistrySandbox, through
// RegistryRoots) and install roots in a folder under %TEMP%, with every Windows-wide question
// (the process context, what Windows resolves, Smart App Control, the Code Integrity log,
// PdfCraft, the tasks, the Explorer test) answered by a fake. Nothing here reads or writes the
// real classes, slots, settings or per-user folder, starts a thumbnail extraction or tells
// Explorer anything. Build folders hold copies of the built DLLs, which are read, never loaded.

#include <windows.h>

#include "check.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "child_process.h"
#include "log.h"
#include "pe_info.h"
#include "project_config.h"
#include "registry_sandbox.h"
#include "settings.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

const std::wstring kOurs = kHandlerClsidString;
const std::wstring kOther = L"{9C60DE1E-E5FC-40F4-A487-460851A8D915}";

#if defined(_M_ARM64)
constexpr Arch kNativeArch = Arch::kArm64;
#else
constexpr Arch kNativeArch = Arch::kX64;
#endif

class FakeQueries final : public AssociationQueries {
 public:
  HRESULT ThumbnailHandler(std::optional<std::wstring>* clsid) const noexcept override {
    ++calls;
    *clsid = handler;
    return S_OK;
  }
  HRESULT ProgId(std::optional<std::wstring>* id) const noexcept override {
    ++calls;
    *id = progId;
    return S_OK;
  }
  std::optional<std::wstring> handler = kOurs;
  std::optional<std::wstring> progId;
  mutable int calls = 0;
};

// Windows as the tests want it: by default a Medium process whose registry is Explorer's,
// Windows resolving our handler, the Explorer test passing, Smart App Control off, PdfCraft
// 0.14.0, no tasks.
class FakeProbes final : public SystemProbes {
 public:
  FakeProbes() {
    context.userSid = L"S-1-12-1-1-2-3-4";
    context.userAccount = L"PC\\someone";
    context.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
    context.probe = ContextProbe::kWrite;
    context.virtualization = Virtualization::kNo;
    context.processArch = kNativeArch;
    context.osArch = kNativeArch;
    sac.state = SacState::kOff;
    pdfcraft = {true, L"0.14.0", L"", PdfCraftSource::kUpgradeCode};
  }
  HRESULT GetProcessContext(ContextProbe, ProcessContext* out) const noexcept override {
    *out = context;
    return S_OK;
  }
  const AssociationQueries& Associations() const noexcept override { return queries; }
  HRESULT ReadSacState(const RegistryRoots&, SacReading* reading) const noexcept override {
    *reading = sac;
    return S_OK;
  }
  BlockCount CountCiBlocks(std::wstring_view leaf, int days) const noexcept override {
    countedLeaf = leaf;
    countedDays = days;
    const size_t call = countCalls++;
    return call < counts.size() ? counts[call] : BlockCount{true, 0, S_OK};
  }
  HRESULT DetectPdfCraft(PdfCraftInfo* info) const noexcept override {
    *info = pdfcraft;
    return S_OK;
  }
  HRESULT ReadTask(const TaskLocation&, TaskState* state) const noexcept override {
    *state = task;
    return S_OK;
  }
  HRESULT LegacyTaskExists(bool* exists) const noexcept override {
    *exists = legacyTask;
    return S_OK;
  }
  HRESULT RunLiveTest(UINT size, const std::filesystem::path* file, DWORD,
                      LiveTest* result) const noexcept override {
    ++liveRuns;
    liveSize = size;
    if (file && FAILED(testFileError)) return testFileError;
    *result = live;
    return S_OK;
  }
  void Wait(DWORD ms) const noexcept override { waits.push_back(ms); }

  ProcessContext context;
  FakeQueries queries;
  SacReading sac;
  std::vector<BlockCount> counts;
  PdfCraftInfo pdfcraft;
  TaskState task;
  bool legacyTask = false;
  LiveTest live{S_OK, {96, 96}, 150};
  HRESULT testFileError = S_OK;  // a test file's run cannot start (no probe thread, ...)

  mutable size_t countCalls = 0;
  mutable std::wstring countedLeaf;
  mutable int countedDays = 0;
  mutable int liveRuns = 0;
  mutable UINT liveSize = 0;
  mutable std::vector<DWORD> waits;
};

std::wstring BuiltFile(const wchar_t* name) { return test::SelfFolder() + L"\\" + name; }

bool Exists(const std::filesystem::path& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// No subkeys and no values.
bool KeyIsEmpty(HKEY key) {
  DWORD subkeys = 1;
  DWORD values = 1;
  if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values, nullptr,
                       nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  return subkeys == 0 && values == 0;
}

std::optional<std::wstring> GetString(HKEY root, const std::wstring& key, const wchar_t* name) {
  std::optional<std::wstring> value;
  if (FAILED(ReadRegistryString(root, key, name, &value))) return std::nullopt;
  return value;
}

class CheckEngine : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sandbox_.ok()) << HResultText(sandbox_.error());
    ASSERT_TRUE(temp_.ok());
    roots_ = {sandbox_.user(), sandbox_.machine()};
    const std::filesystem::path base = temp_.path();
    paths_.userRoot = base / L"user-root";
    paths_.machineRoot = base / L"machine-root";
    paths_.legacyUserRoot = paths_.userRoot;
    paths_.legacyMachineRoot = paths_.machineRoot;
    paths_.legacyIni = base / L"no-settings.ini";
    paths_.ownExe = test::SelfPath();
  }

  // A build folder <root>\app\<name> with copies of the built DLLs (or, for 1.x names, small
  // stand-ins: nothing reads them).
  std::filesystem::path MakeBuild(const std::filesystem::path& root, const std::wstring& name) {
    const std::filesystem::path dir = root / L"app" / name;
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    EXPECT_FALSE(error) << error.message();
    if (name.starts_with(L"1.")) {
      std::ofstream(dir / kHandlerDll) << "1.x";
      std::ofstream(dir / (std::wstring(kLegacyRootName) + L"-pdfium.dll")) << "1.x";
    } else {
      EXPECT_TRUE(CopyFileW(BuiltFile(kHandlerDll).c_str(), (dir / kHandlerDll).c_str(), TRUE));
      EXPECT_TRUE(CopyFileW(BuiltFile(kPdfiumDll).c_str(), (dir / kPdfiumDll).c_str(), TRUE));
    }
    return dir;
  }

  void SetSlot(size_t slot, const std::wstring& value) {
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.user(), SlotKeyPath(slot), L"", value));
  }
  std::optional<std::wstring> Slot(size_t slot) {
    return GetString(sandbox_.user(), SlotKeyPath(slot), L"");
  }

  void RegisterMachineClass(const std::filesystem::path& dll) {
    const std::wstring inproc = ClassKeyPath() + L"\\InprocServer32";
    ASSERT_HRESULT_SUCCEEDED(
        WriteRegistryString(sandbox_.machine(), ClassKeyPath(), L"", kHandlerClassName));
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.machine(), inproc, L"", dll.native()));
    ASSERT_HRESULT_SUCCEEDED(
        WriteRegistryString(sandbox_.machine(), inproc, L"ThreadingModel", L"Apartment"));
  }

  // A sound per-user install on build |name|, both slots ours.
  std::filesystem::path InstallPerUser(const std::wstring& name = L"2.0.0.0-1a2b3c4d") {
    const std::filesystem::path dir = MakeBuild(paths_.userRoot, name);
    EXPECT_HRESULT_SUCCEEDED(WriteClass(roots_, Scope::kUser, dir / kHandlerDll));
    SetSlot(0, kOurs);
    SetSlot(1, kOurs);
    return dir;
  }

  CheckOptions Options(RepairMode mode = RepairMode::kSafe) const {
    CheckOptions options;
    options.repair = mode;
    options.context = probes_.context;
    options.paths = paths_;
    options.lockWaitMs = 5000;
    return options;
  }

  CheckResult Run(RepairMode mode = RepairMode::kSafe) {
    CheckResult result;
    EXPECT_HRESULT_SUCCEEDED(RunCheck(Options(mode), roots_, probes_, &result));
    return result;
  }

  Snapshot Gather() {
    GatherOptions options;
    options.context = probes_.context;
    options.paths = paths_;
    Snapshot snapshot;
    EXPECT_HRESULT_SUCCEEDED(GatherSnapshot(roots_, options, probes_, &snapshot));
    return snapshot;
  }

  bool StateRecorded() {
    bool exists = false;
    EXPECT_HRESULT_SUCCEEDED(RegistryKeyExists(sandbox_.user(), StateKeyPath(), &exists));
    return exists;
  }

  test::ScopedRegistrySandbox sandbox_;
  test::TempDir temp_{L"check"};
  RegistryRoots roots_;
  EnginePaths paths_;
  FakeProbes probes_;
};

TEST_F(CheckEngine, NothingInstalledExits6AndWritesNothing) {
  const CheckResult result = Run();
  EXPECT_EQ(result.code, ExitCode::kNotInstalled);
  EXPECT_TRUE(HasFinding(result.findings, FindingId::kNotInstalled));
  EXPECT_TRUE(result.repairs.empty());
  EXPECT_TRUE(KeyIsEmpty(sandbox_.user()));
  EXPECT_TRUE(KeyIsEmpty(sandbox_.machine()));
  EXPECT_FALSE(Exists(paths_.userRoot));
  // Windows resolves our handler (leftover slots), but without a class the Explorer test could
  // only report that.
  EXPECT_EQ(probes_.liveRuns, 0);
  EXPECT_EQ(result.after.liveSkip, LiveTestSkip::kNoClass);
}

TEST_F(CheckEngine, GatherReadsAPerUserInstall) {
  const std::filesystem::path dir = InstallPerUser();
  probes_.queries.progId = L"Some.App";
  const Snapshot s = Gather();
  EXPECT_EQ(s.scope, Scope::kUser);
  EXPECT_EQ(s.installRoot, paths_.userRoot);
  ASSERT_TRUE(s.userClass);
  EXPECT_TRUE(s.userClass->dllExists);
  EXPECT_TRUE(s.userClass->pdfiumExists);
  EXPECT_EQ(s.userClass->fileVersion, kFileVersion);
  ASSERT_EQ(s.userBuilds.size(), 1u);
  EXPECT_TRUE(s.userBuilds[0].folder.complete);
  ASSERT_TRUE(s.userBuilds[0].dllMachine);
  EXPECT_EQ(MachineToArch(*s.userBuilds[0].dllMachine), kNativeArch);
  EXPECT_EQ(s.effective, kOurs);
  EXPECT_EQ(s.defaultApp.progId, L"Some.App");
  EXPECT_EQ(s.pdfcraft.version, L"0.14.0");
  ASSERT_TRUE(s.live);
  EXPECT_EQ(probes_.liveSize, kProbeThumbnailSize);
  EXPECT_FALSE(s.blocks);  // Smart App Control is off: nothing counted
  EXPECT_EQ(s.sacVerdict.kind, SacVerdictKind::kNone);
  EXPECT_TRUE(s.issues.empty());
  EXPECT_EQ(ChooseUserBuildDll(s), dir / kHandlerDll);
  EXPECT_TRUE(HealthyFromFindings(Evaluate(s)));
}

TEST_F(CheckEngine, FillsAnEmptySlotAndRecordsTheCheck) {
  InstallPerUser();
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryTree(sandbox_.user(), SlotKeyPath(1)));
  const std::filesystem::path log = std::filesystem::path(temp_.path()) / L"check.log";
  SetLogFileForTesting(log, kLogRotateBytes);
  const CheckResult result = Run();
  SetLogFileForTesting({}, 0);

  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_TRUE(HasFinding(result.before, FindingId::kSlotEmpty));
  EXPECT_FALSE(HasFinding(result.findings, FindingId::kSlotEmpty));
  EXPECT_EQ(Slot(1), kOurs);
  ASSERT_EQ(result.repairs.size(), 1u);
  EXPECT_EQ(result.repairs[0].step.kind, RepairStep::Kind::kFillSlot);
  EXPECT_EQ(result.repairs[0].result, RepairResult::kDone);
  EXPECT_FALSE(result.notified);   // a sandbox: Explorer is never told
  EXPECT_EQ(probes_.liveRuns, 2);  // a slot changed: the test ran again

  std::optional<CheckRecord> record;
  ASSERT_HRESULT_SUCCEEDED(ReadCheck(roots_, &record));
  ASSERT_TRUE(record);
  EXPECT_EQ(record->result, ExitCode::kOk);
  EXPECT_EQ(record->summary, L"fixed 1");
  EXPECT_EQ(record->pdfcraftVersion, L"0.14.0");
  EXPECT_EQ(record->liveTest, S_OK);

  // One line per step and one for the check; no paths.
  std::ifstream file(log, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  const std::string lines = text.str();
  EXPECT_NE(lines.find("check INFO repair fill-slot slot=1 result=done"), std::string::npos)
      << lines;
  EXPECT_NE(lines.find("check INFO check result=healthy fixed=1 pdfcraft=0.14.0"),
            std::string::npos)
      << lines;
  EXPECT_EQ(lines.find(WideToUtf8Lossy(temp_.path())), std::string::npos) << lines;

  // A second check finds nothing to do and starts no repair.
  const CheckResult again = Run();
  EXPECT_EQ(again.code, ExitCode::kOk);
  EXPECT_TRUE(again.repairs.empty());
  ASSERT_HRESULT_SUCCEEDED(ReadCheck(roots_, &record));
  EXPECT_EQ(record->summary, L"nothing to fix");
}

TEST_F(CheckEngine, AnotherHandlersSlotIsTakenBackOnlyOnRequest) {
  InstallPerUser();
  SetSlot(0, kOther);
  CheckResult result = Run(RepairMode::kSafe);
  EXPECT_EQ(result.code, ExitCode::kProblems);
  EXPECT_EQ(Slot(0), kOther);
  EXPECT_TRUE(HasFinding(result.findings, FindingId::kSlotOtherHandler));
  std::optional<CheckRecord> record;
  ASSERT_HRESULT_SUCCEEDED(ReadCheck(roots_, &record));
  EXPECT_EQ(record->result, ExitCode::kProblems);

  result = Run(RepairMode::kSafeAndTakeBack);
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(Slot(0), kOurs);
}

TEST_F(CheckEngine, ClassMissingRegistersTheNewestBuild) {
  const std::filesystem::path older = MakeBuild(paths_.userRoot, L"2.0.0.0-aaaaaaaa");
  const std::filesystem::path newer = MakeBuild(paths_.userRoot, L"2.0.1.0-bbbbbbbb");
  const CheckResult result = Run();
  EXPECT_EQ(result.code, ExitCode::kOk);
  std::optional<ClassReg> reg;
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  ASSERT_TRUE(reg);
  EXPECT_TRUE(IEquals(reg->dll, (newer / kHandlerDll).native())) << reg->dll;
  EXPECT_EQ(reg->threading, L"Apartment");
  EXPECT_EQ(Slot(0), kOurs);
  EXPECT_EQ(Slot(1), kOurs);
  EXPECT_FALSE(Exists(older));  // nothing used it
  EXPECT_TRUE(Exists(newer / kHandlerDll));
}

TEST_F(CheckEngine, PerUserClassShadowingTheMachineClassIsDeleted) {
  const std::filesystem::path machine = MakeBuild(paths_.machineRoot, L"2.0.0.0-1a2b3c4d");
  RegisterMachineClass(machine / kHandlerDll);
  const std::filesystem::path user = InstallPerUser(L"2.0.0.0-99999999");
  CheckResult result = Run();
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_TRUE(HasFinding(result.before, FindingId::kUserClassShadowsMachine));
  std::optional<ClassReg> reg;
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  EXPECT_FALSE(reg);
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg));
  EXPECT_TRUE(reg);
  // The build the per-user class used goes with the next check, once nothing points at it.
  EXPECT_TRUE(Exists(user));
  EXPECT_TRUE(HasFinding(result.findings, FindingId::kOldBuildFolders));
  result = Run();
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_FALSE(Exists(user));
  EXPECT_TRUE(Exists(machine / kHandlerDll));
}

TEST_F(CheckEngine, PendingCleanupIsDeletedWhileInstalled) {
  InstallPerUser();
  ASSERT_HRESULT_SUCCEEDED(
      WriteRegistryString(sandbox_.user(), kRunOnceKey, kLegacyRunOnce, L"cmd /c rd"));
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.user(), kRunOnceKey, L"Other", L"x"));
  const CheckResult result = Run();
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_FALSE(GetString(sandbox_.user(), kRunOnceKey, kLegacyRunOnce));
  EXPECT_EQ(GetString(sandbox_.user(), kRunOnceKey, L"Other"), L"x");
  EXPECT_FALSE(HasFinding(result.findings, FindingId::kCleanupPending));
}

TEST_F(CheckEngine, InvalidZoomIsRewritten) {
  InstallPerUser();
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.user(), kRegKey, L"Zoom", L"150"));
  CheckResult result = Run();
  EXPECT_EQ(result.code, ExitCode::kOk);
  std::optional<RegistryValue> value;
  ASSERT_HRESULT_SUCCEEDED(ReadRegistryValue(sandbox_.user(), kRegKey, L"Zoom", &value));
  ASSERT_TRUE(value);
  EXPECT_EQ(RegistryDword(*value), 100u);  // the zoom in effect: no settings file, so 100

  const DWORD tooBig = 500;
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryDword(sandbox_.user(), kRegKey, L"Zoom", tooBig));
  result = Run();
  ASSERT_HRESULT_SUCCEEDED(ReadRegistryValue(sandbox_.user(), kRegKey, L"Zoom", &value));
  EXPECT_EQ(RegistryDword(*value), 200u);
  EXPECT_FALSE(HasFinding(result.findings, FindingId::kZoomInvalid));
}

TEST_F(CheckEngine, OldBuildsGoButNot1xFoldersLinksOrFoldersInUse) {
  const std::filesystem::path current = InstallPerUser(L"2.0.1.0-cccccccc");
  const std::filesystem::path old = MakeBuild(paths_.userRoot, L"2.0.0.0-aaaaaaaa");
  const std::filesystem::path v1 = MakeBuild(paths_.userRoot, L"1.1.0.0-67a1323e");
  const std::filesystem::path busy = MakeBuild(paths_.userRoot, L"2.0.0.0-dddddddd");
  // A junction named like a build, pointing out of the root: never followed.
  const std::filesystem::path outside = std::filesystem::path(temp_.path()) / L"outside";
  std::error_code error;
  std::filesystem::create_directories(outside, error);
  std::ofstream(outside / L"keep.txt") << "keep";
  const std::filesystem::path link = paths_.userRoot / L"app" / L"2.0.0.0-bbbbbbbb";
  std::filesystem::create_directories(link, error);
  ASSERT_TRUE(test::MakeJunction(link.native(), outside.native()));

  {
    // A thumbnail process holding the DLL: it cannot be deleted while open.
    HANDLE held = CreateFileW((busy / kHandlerDll).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    ASSERT_NE(held, INVALID_HANDLE_VALUE);
    const CheckResult result = Run();
    CloseHandle(held);
    EXPECT_EQ(result.code, ExitCode::kOk);
    int kept = 0;
    for (const RepairOutcome& outcome : result.repairs) {
      EXPECT_NE(outcome.result, RepairResult::kFailed) << HResultText(outcome.hr);
      if (outcome.result == RepairResult::kKept) ++kept;
    }
    EXPECT_EQ(kept, 1);
  }
  EXPECT_FALSE(Exists(old));
  EXPECT_TRUE(Exists(current / kHandlerDll));
  EXPECT_TRUE(Exists(v1 / kHandlerDll));
  EXPECT_TRUE(Exists(busy / kHandlerDll));  // whole, for the next check
  EXPECT_TRUE(Exists(busy / kPdfiumDll));
  EXPECT_TRUE(Exists(outside / L"keep.txt"));

  const CheckResult again = Run();  // nothing holds it any more
  EXPECT_EQ(again.code, ExitCode::kOk);
  EXPECT_FALSE(Exists(busy));
  EXPECT_TRUE(Exists(v1));
  EXPECT_TRUE(Exists(outside / L"keep.txt"));
}

TEST_F(CheckEngine, RemovingABuildFolderRefusesWhatIsNotOne) {
  const std::filesystem::path v1 = MakeBuild(paths_.userRoot, L"1.1.0.0-67a1323e");
  bool kept = false;
  EXPECT_EQ(RemoveUserBuildFolder(paths_.userRoot, v1, &kept), E_INVALIDARG);
  EXPECT_EQ(RemoveUserBuildFolder(paths_.userRoot, paths_.userRoot / L"app", &kept), E_INVALIDARG);
  EXPECT_EQ(RemoveUserBuildFolder(paths_.userRoot,
                                  paths_.machineRoot / L"app" / L"2.0.0.0-aaaaaaaa", &kept),
            E_INVALIDARG);
  EXPECT_EQ(RemoveUserBuildFolder(paths_.userRoot, paths_.userRoot / L"app" / L"notes", &kept),
            E_INVALIDARG);
  EXPECT_TRUE(Exists(v1 / kHandlerDll));

  // A root that is a junction is never entered.
  const std::filesystem::path target = MakeBuild(paths_.machineRoot, L"2.0.0.0-aaaaaaaa");
  const std::filesystem::path linkedRoot = std::filesystem::path(temp_.path()) / L"linked-root";
  std::error_code error;
  std::filesystem::create_directories(linkedRoot, error);
  ASSERT_TRUE(test::MakeJunction(linkedRoot.native(), paths_.machineRoot.native()));
  EXPECT_EQ(RemoveUserBuildFolder(linkedRoot, linkedRoot / L"app" / L"2.0.0.0-aaaaaaaa", &kept),
            HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  EXPECT_TRUE(Exists(target / kHandlerDll));
}

TEST_F(CheckEngine, LockHeldElsewhereMeansBusy) {
  InstallPerUser();
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryTree(sandbox_.user(), SlotKeyPath(1)));
  test::PipedChild child;
  ASSERT_HRESULT_SUCCEEDED(
      child.Start(test::SelfPath(), L"--pdft-child hold-lock " + test::NamedObjectSuffix()));
  ASSERT_EQ(child.ReadLine(), "locked");

  CheckOptions options = Options();
  options.lockWaitMs = 500;
  CheckResult result;
  const ULONGLONG before = GetTickCount64();
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  const ULONGLONG elapsed = GetTickCount64() - before;
  EXPECT_EQ(result.code, ExitCode::kBusy);
  EXPECT_TRUE(result.busy);
  EXPECT_GE(elapsed, 400u);
  EXPECT_LT(elapsed, 10000u);
  EXPECT_FALSE(Slot(1));  // nothing repaired
  EXPECT_FALSE(StateRecorded());
  EXPECT_EQ(probes_.liveRuns, 0);

  child.CloseInput();
  EXPECT_EQ(child.Wait(30000), 0u);
  // A free lock is taken at once, whatever wait the caller asks for (the cap on the wait is
  // tested on its own: LockWaitIsCapped).
  options.lockWaitMs = INFINITE;
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kOk);
}

// However long the caller asks to wait, a check waits 30 seconds at most, so a scheduled check
// cannot hang on a lock that stays taken.
TEST(CheckLock, LockWaitIsCapped) {
  EXPECT_EQ(EffectiveLockWaitMs(0), 0u);
  EXPECT_EQ(EffectiveLockWaitMs(500), 500u);
  EXPECT_EQ(EffectiveLockWaitMs(kCheckLockWaitMs - 1), kCheckLockWaitMs - 1);
  EXPECT_EQ(EffectiveLockWaitMs(kCheckLockWaitMs), kCheckLockWaitMs);
  EXPECT_EQ(EffectiveLockWaitMs(kCheckLockWaitMs + 1), kCheckLockWaitMs);
  EXPECT_EQ(EffectiveLockWaitMs(INFINITE), kCheckLockWaitMs);
  EXPECT_EQ(kCheckLockWaitMs, 30000u);
}

TEST_F(CheckEngine, RefusedWhereWritesMayNotReachTheRealRegistry) {
  InstallPerUser();
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryTree(sandbox_.user(), SlotKeyPath(1)));
  for (const Virtualization virtualization : {Virtualization::kYes, Virtualization::kUnknown}) {
    probes_.context.virtualization = virtualization;
    const CheckResult result = Run();
    EXPECT_EQ(result.code, ExitCode::kWrongContext);
    EXPECT_TRUE(result.refused);
  }
  // A context from the read-only comparison says nothing about writes: refused as well.
  probes_.context.virtualization = Virtualization::kNo;
  probes_.context.probe = ContextProbe::kReadOnly;
  probes_.context.views = RegistryViews::kAgree;
  EXPECT_EQ(Run().code, ExitCode::kWrongContext);
  // And without a context given, the probes' canary decides.
  probes_.context.probe = ContextProbe::kWrite;
  probes_.context.virtualization = Virtualization::kYes;
  CheckOptions options = Options();
  options.context.reset();
  CheckResult result;
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kWrongContext);

  EXPECT_FALSE(Slot(1));
  EXPECT_FALSE(StateRecorded());
  EXPECT_EQ(probes_.liveRuns, 0);
  EXPECT_EQ(probes_.queries.calls, 0);
}

// The repairs change the user's classes and slots and delete folders in the user's profile,
// which the user's own unelevated programs control: an elevated process does none of it (and,
// elevated as another account, would change the wrong profile).
TEST_F(CheckEngine, RefusedWhenElevated) {
  InstallPerUser(L"2.0.1.0-cccccccc");
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryTree(sandbox_.user(), SlotKeyPath(1)));
  const std::filesystem::path old = MakeBuild(paths_.userRoot, L"2.0.0.0-aaaaaaaa");
  probes_.context.elevated = true;
  probes_.context.integrityRid = SECURITY_MANDATORY_HIGH_RID;
  for (const RepairMode mode : {RepairMode::kSafe, RepairMode::kSafeAndTakeBack}) {
    const CheckResult result = Run(mode);
    EXPECT_EQ(result.code, ExitCode::kWrongContext);
    EXPECT_TRUE(result.refused);
    EXPECT_TRUE(result.refusedElevated);
    EXPECT_TRUE(result.repairs.empty());
  }
  EXPECT_FALSE(Slot(1));
  EXPECT_TRUE(Exists(old / kHandlerDll));
  EXPECT_FALSE(StateRecorded());
  EXPECT_EQ(probes_.liveRuns, 0);
  EXPECT_EQ(probes_.queries.calls, 0);

  // Where the registry may also be virtualized, that is the reason given.
  probes_.context.virtualization = Virtualization::kYes;
  const CheckResult virtualized = Run();
  EXPECT_TRUE(virtualized.refused);
  EXPECT_FALSE(virtualized.refusedElevated);

  // Even handed an elevated snapshot directly, the repairs leave the profile's folders alone.
  probes_.context.virtualization = Virtualization::kNo;
  probes_.context.elevated = false;
  Snapshot s = Gather();
  const std::vector<Finding> findings = Evaluate(s);
  const RepairPlan plan = PlanRepairs(s, findings, RepairMode::kSafe);
  ASSERT_FALSE(plan.user.empty());
  s.context.elevated = true;
  std::vector<RepairOutcome> outcomes;
  ASSERT_HRESULT_SUCCEEDED(ApplyUserRepairs(roots_, s, plan, &outcomes));
  bool removalSeen = false;
  for (const RepairOutcome& outcome : outcomes) {
    if (outcome.step.kind != RepairStep::Kind::kRemoveUserBuild) continue;
    removalSeen = true;
    EXPECT_EQ(outcome.result, RepairResult::kNotApplied);
  }
  EXPECT_TRUE(removalSeen);
  EXPECT_TRUE(Exists(old / kHandlerDll));
}

// A link inside an old build folder is removed as a link: what it points at stays.
TEST_F(CheckEngine, LinksInsideAnOldBuildAreRemovedNotFollowed) {
  InstallPerUser(L"2.0.1.0-cccccccc");
  const std::filesystem::path old = MakeBuild(paths_.userRoot, L"2.0.0.0-aaaaaaaa");
  const std::filesystem::path outside = std::filesystem::path(temp_.path()) / L"outside";
  std::error_code error;
  std::filesystem::create_directories(outside / L"sub", error);
  ASSERT_FALSE(error) << error.message();
  std::ofstream(outside / L"keep.txt") << "keep";
  std::ofstream(outside / L"sub" / L"keep-too.txt") << "keep";

  std::filesystem::create_directories(old / L"junction", error);
  ASSERT_TRUE(test::MakeJunction((old / L"junction").native(), outside.native()));
  std::filesystem::create_directories(old / L"nested", error);
  std::filesystem::create_directories(old / L"nested" / L"deeper", error);
  ASSERT_TRUE(test::MakeJunction((old / L"nested" / L"deeper").native(), outside.native()));
  // Symbolic links too, where this account may create them (developer mode or the privilege).
  const bool directoryLink =
      CreateSymbolicLinkW(
          (old / L"dirlink").c_str(), outside.c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
  const bool fileLink =
      CreateSymbolicLinkW((old / L"filelink.txt").c_str(), (outside / L"keep.txt").c_str(),
                          SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
  if (!directoryLink || !fileLink) {
    std::printf("symbolic links: not allowed here, only junctions are tried\n");
  }

  const CheckResult result = Run();
  EXPECT_EQ(result.code, ExitCode::kOk);
  for (const RepairOutcome& outcome : result.repairs) {
    EXPECT_NE(outcome.result, RepairResult::kFailed) << HResultText(outcome.hr);
  }
  EXPECT_FALSE(Exists(old));
  EXPECT_TRUE(Exists(outside / L"keep.txt"));
  EXPECT_TRUE(Exists(outside / L"sub" / L"keep-too.txt"));
}

// A test file whose run could not even start counts against health, as in 1.x.
TEST_F(CheckEngine, ATestFileThatCannotRunIsAProblem) {
  InstallPerUser();
  const std::filesystem::path pdf = std::filesystem::path(temp_.path()) / L"test.pdf";
  std::ofstream(pdf) << "%PDF-1.4";
  probes_.testFileError = E_OUTOFMEMORY;
  GatherOptions options;
  options.context = probes_.context;
  options.paths = paths_;
  options.testFile = pdf;
  Snapshot s;
  ASSERT_HRESULT_SUCCEEDED(GatherSnapshot(roots_, options, probes_, &s));
  ASSERT_TRUE(s.testFile);
  EXPECT_FALSE(s.testFile->result);
  EXPECT_EQ(s.testFile->error, E_OUTOFMEMORY);
  const std::vector<Finding> findings = Evaluate(s);
  const Finding* failed = nullptr;
  for (const Finding& finding : findings) {
    if (finding.id == FindingId::kTestFileFailed) failed = &finding;
  }
  ASSERT_TRUE(failed);
  EXPECT_EQ(failed->hr, E_OUTOFMEMORY);
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kProblems);
}

TEST_F(CheckEngine, MachinePartNeedsAnAdministrator) {
  const std::filesystem::path missing = paths_.machineRoot / L"app" / L"2.0.0.0-1a2b3c4d";
  RegisterMachineClass(missing / kHandlerDll);
  SetSlot(0, kOurs);
  SetSlot(1, kOurs);
  CheckOptions options = Options();
  options.quiet = true;
  CheckResult result;
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kNeedsAdmin);
  EXPECT_TRUE(result.adminStepNeeded);
  EXPECT_EQ(result.plan.machine, ElevatedVerb::kMachineRepairClass);
  options.quiet = false;
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kProblems);
  std::optional<ClassReg> reg;
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg));
  EXPECT_TRUE(IEquals(reg->dll, (missing / kHandlerDll).native()));  // never touched
}

TEST_F(CheckEngine, SmartAppControlCountsAndRecountsLike1x) {
  InstallPerUser(L"2.0.0.0-1a2b3c4d");
  probes_.sac.state = SacState::kOn;

  // The test passed: nothing counted.
  Snapshot s = Gather();
  EXPECT_EQ(probes_.countCalls, 0u);
  EXPECT_EQ(s.sacVerdict.kind, SacVerdictKind::kOk);

  // It failed with an ordinary error and the first count found nothing: count again after
  // the pause, and the second count decides.
  probes_.live.hr = E_FAIL;
  probes_.counts = {BlockCount{true, 0, S_OK}, BlockCount{true, 2, S_OK}};
  s = Gather();
  EXPECT_EQ(probes_.countCalls, 2u);
  EXPECT_EQ(probes_.waits, (std::vector<DWORD>{kSacRecountDelayMs}));
  EXPECT_EQ(probes_.countedLeaf, L"2.0.0.0-1a2b3c4d");
  EXPECT_EQ(probes_.countedDays, kSacBlockDays);
  EXPECT_EQ(s.sacVerdict.kind, SacVerdictKind::kBlocked);
  EXPECT_EQ(s.sacVerdict.blocks, 2);
  EXPECT_TRUE(HasFinding(Evaluate(s), FindingId::kSacBlocked));

  // A load-blocked error settles it without a second count.
  probes_.countCalls = 0;
  probes_.waits.clear();
  probes_.live.hr = HRESULT_FROM_WIN32(4551);
  probes_.counts = {BlockCount{true, 0, S_OK}};
  s = Gather();
  EXPECT_EQ(probes_.countCalls, 1u);
  EXPECT_TRUE(probes_.waits.empty());
  EXPECT_EQ(s.sacVerdict.kind, SacVerdictKind::kBlocked);
  EXPECT_EQ(s.sacVerdict.evidence, HRESULT_FROM_WIN32(4551));
}

TEST_F(CheckEngine, WindowsChecksAndTheExplorerTestNeedExplorersView) {
  InstallPerUser();
  probes_.context.probe = ContextProbe::kReadOnly;
  probes_.context.views = RegistryViews::kDiffer;
  Snapshot s = Gather();
  EXPECT_TRUE(s.windowsChecksSkipped);
  EXPECT_EQ(s.windowsChecksSkip, WindowsChecksSkip::kViewsDiffer);
  EXPECT_FALSE(s.effective);
  EXPECT_EQ(s.liveSkip, LiveTestSkip::kVirtualized);
  EXPECT_EQ(probes_.queries.calls, 0);
  EXPECT_EQ(probes_.liveRuns, 0);

  // Elevated: Windows' answers are read, but the Explorer test is not run.
  probes_.context.views = RegistryViews::kAgree;
  probes_.context.elevated = true;
  s = Gather();
  EXPECT_FALSE(s.windowsChecksSkipped);
  EXPECT_EQ(s.effective, kOurs);
  EXPECT_EQ(s.liveSkip, LiveTestSkip::kElevated);
  EXPECT_EQ(probes_.liveRuns, 0);

  // Windows resolves another handler: the probe would test that one.
  probes_.context.elevated = false;
  probes_.queries.handler = kOther;
  s = Gather();
  EXPECT_EQ(s.liveSkip, LiveTestSkip::kNotOurHandler);
  EXPECT_EQ(probes_.liveRuns, 0);
}

}  // namespace
}  // namespace pdft
