// The check end to end (snapshot.h, evaluate.h, repair.h, check.h) and --checks
// (check_schedule.h): gathering, judging, repairing and recording against a private registry key
// (ScopedRegistrySandbox, through RegistryRoots) and install roots in a folder under %TEMP%, with
// every Windows-wide question (the process context, what Windows resolves, Smart App Control, the
// Code Integrity log, PdfCraft, the tasks, the Explorer test) answered by a fake, which also
// records what would be written to Task Scheduler instead of writing it. Nothing here reads or
// writes the real classes, slots, settings, per-user folder or tasks, starts a thumbnail
// extraction or tells Explorer anything. Build folders hold copies of the built DLLs, which are
// read, never loaded; settings executables are empty files, never started.

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

#include "check_schedule.h"
#include "child_process.h"
#include "fake_probes.h"
#include "log.h"
#include "pe_info.h"
#include "project_config.h"
#include "registry_sandbox.h"
#include "settings.h"
#include "status_format.h"
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

using test::FakeProbes;
using test::FakeQueries;

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

  // The settings executable of the install at |root|: an empty file, never started.
  std::filesystem::path PlaceSettingsExe(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::create_directories(root, error);
    const std::filesystem::path exe = root / kSettingsExe;
    std::ofstream(exe) << "";
    EXPECT_TRUE(Exists(exe));
    return exe;
  }

  // The fake user's check task, as it would be registered.
  TaskLocation Location() const {
    TaskLocation location;
    EXPECT_HRESULT_SUCCEEDED(DefaultTaskLocation(probes_.context.userSid, &location));
    return location;
  }

  ScheduleResult Schedule(bool afterPdfCraft, bool atSignIn) {
    ScheduleOptions options;
    options.afterPdfCraft = afterPdfCraft;
    options.atSignIn = atSignIn;
    options.context = probes_.context;
    options.paths = paths_;
    options.lockWaitMs = 5000;
    ScheduleResult result;
    EXPECT_HRESULT_SUCCEEDED(ChangeCheckSchedule(options, roots_, probes_, &result));
    return result;
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
  EXPECT_NE(lines.find("check INFO check result=healthy fixed=1 pdfcraft=0.14.0 live=0x00000000 "
                       "live-ms=150 ms="),
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

// The check's log line says what became of the Explorer test. One that could not start makes the
// line a warning, but the check does not need the test's answer: it stays healthy, and records
// no test result.
TEST_F(CheckEngine, TheLogLineNamesTheExplorerTestsOutcome) {
  InstallPerUser();
  probes_.liveError = E_ACCESSDENIED;
  const std::filesystem::path log = std::filesystem::path(temp_.path()) / L"check.log";
  SetLogFileForTesting(log, kLogRotateBytes);
  const CheckResult result = Run();
  std::optional<CheckRecord> record;
  EXPECT_HRESULT_SUCCEEDED(ReadCheck(roots_, &record));
  CheckOptions options = Options();
  options.liveTest = false;
  CheckResult skipped;
  EXPECT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &skipped));
  SetLogFileForTesting({}, 0);

  EXPECT_EQ(result.code, ExitCode::kOk);
  const Finding* notRun = nullptr;
  for (const Finding& finding : result.findings) {
    if (finding.id == FindingId::kLiveTestSkipped) notRun = &finding;
  }
  ASSERT_TRUE(notRun);
  EXPECT_EQ(notRun->severity, Severity::kInfo);
  EXPECT_EQ(notRun->hr, E_ACCESSDENIED);
  ASSERT_TRUE(record);
  EXPECT_EQ(record->result, ExitCode::kOk);
  EXPECT_FALSE(record->liveTest);
  EXPECT_EQ(skipped.code, ExitCode::kOk);

  std::ifstream file(log, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  const std::string lines = text.str();
  EXPECT_NE(lines.find("check WARN check result=healthy fixed=0 pdfcraft=0.14.0 "
                       "live=could-not-start live-hr=0x80070005 ms="),
            std::string::npos)
      << lines;
  EXPECT_NE(lines.find("check INFO check result=healthy fixed=0 pdfcraft=0.14.0 live=skipped "
                       "live-skip=not-requested ms="),
            std::string::npos)
      << lines;
  EXPECT_EQ(lines.find(WideToUtf8Lossy(temp_.path())), std::string::npos) << lines;

  // The gathering needs the test's answer only when asked to, and only with the test.
  EXPECT_FALSE(Gather().liveRequired);
  GatherOptions gather;
  gather.context = probes_.context;
  gather.paths = paths_;
  gather.liveTestRequired = true;
  Snapshot s;
  ASSERT_HRESULT_SUCCEEDED(GatherSnapshot(roots_, gather, probes_, &s));
  EXPECT_TRUE(s.liveRequired);
  EXPECT_EQ(s.liveError, E_ACCESSDENIED);
  gather.liveTest = false;
  ASSERT_HRESULT_SUCCEEDED(GatherSnapshot(roots_, gather, probes_, &s));
  EXPECT_FALSE(s.liveRequired);
  EXPECT_EQ(s.liveSkip, LiveTestSkip::kNotRequested);
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
  ASSERT_HRESULT_SUCCEEDED(ApplyUserRepairs(roots_, probes_, s, plan, &outcomes));
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
  const std::filesystem::path build = InstallPerUser(L"2.0.0.0-1a2b3c4d");
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
  EXPECT_TRUE(IEquals(probes_.countedFolder.native(), build.native())) << probes_.countedFolder;
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

// The registered DLL's folder is gone: there is no folder to look for in the log, so nothing is
// counted or waited for, and the verdict says nothing about blocks (not that the log could not
// be read).
TEST_F(CheckEngine, SmartAppControlCountsNothingForABuildThatIsGone) {
  const std::filesystem::path missing = paths_.machineRoot / L"app" / L"2.0.0.0-1a2b3c4d";
  RegisterMachineClass(missing / kHandlerDll);
  SetSlot(0, kOurs);
  SetSlot(1, kOurs);
  probes_.sac.state = SacState::kOn;
  probes_.live.hr = E_FAIL;
  const Snapshot s = Gather();
  EXPECT_EQ(probes_.countCalls, 0u);
  EXPECT_TRUE(probes_.waits.empty());
  EXPECT_FALSE(s.blocks);
  EXPECT_EQ(s.sacVerdict.kind, SacVerdictKind::kOn);
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

// ---- --checks ---------------------------------------------------------------------------------

TEST_F(CheckEngine, ChecksRegisterTheTaskForTheInstalledSettingsExe) {
  InstallPerUser();
  const std::filesystem::path exe = PlaceSettingsExe(paths_.userRoot);
  const struct {
    bool afterPdfCraft, atSignIn;
  } cases[] = {{true, true}, {true, false}, {false, true}};
  for (const auto& c : cases) {
    probes_.applied.clear();
    const ScheduleResult result = Schedule(c.afterPdfCraft, c.atSignIn);
    EXPECT_EQ(result.code, ExitCode::kOk);
    EXPECT_EQ(result.refusal, ScheduleRefusal::kNone);
    ASSERT_EQ(probes_.applied.size(), 1u);
    const FakeProbes::Applied& applied = probes_.applied[0];
    EXPECT_EQ(applied.location.folder, Location().folder);
    EXPECT_EQ(applied.location.name, Location().name);
    EXPECT_EQ(applied.location.fallbackName, Location().fallbackName);
    EXPECT_EQ(applied.options.afterPdfCraft,
              c.afterPdfCraft ? TriggerSetting::kEnabled : TriggerSetting::kAbsent);
    EXPECT_EQ(applied.options.atSignIn,
              c.atSignIn ? TriggerSetting::kEnabled : TriggerSetting::kAbsent);
    EXPECT_TRUE(applied.options.enabled);
    EXPECT_TRUE(IEquals(applied.program.native(), exe.native())) << applied.program;
    EXPECT_FALSE(applied.elevated);
    // The task as read back afterwards, and what --checks prints from it.
    EXPECT_TRUE(result.after.exists);
    EXPECT_EQ(result.after.afterPdfCraft, c.afterPdfCraft);
    EXPECT_EQ(result.after.atSignIn, c.atSignIn);
    const ScheduleReport report = ScheduleResultReport(result);
    EXPECT_EQ(report.error, L"");
    ASSERT_FALSE(report.out.empty());
    EXPECT_EQ(report.out[0], L"Checks: " + ChecksStateText(c.afterPdfCraft, c.atSignIn));
  }
  EXPECT_FALSE(StateRecorded());  // --checks records no check
}

// A machine-wide install's task runs the program in the machine-wide root, never the per-user
// one or a build tree.
TEST_F(CheckEngine, ChecksUseTheMachineWideProgramForAMachineWideInstall) {
  const std::filesystem::path machine = MakeBuild(paths_.machineRoot, L"2.0.0.0-1a2b3c4d");
  RegisterMachineClass(machine / kHandlerDll);
  SetSlot(0, kOurs);
  SetSlot(1, kOurs);
  const std::filesystem::path exe = PlaceSettingsExe(paths_.machineRoot);
  PlaceSettingsExe(paths_.userRoot);
  const ScheduleResult result = Schedule(true, true);
  EXPECT_EQ(result.code, ExitCode::kOk);
  ASSERT_EQ(probes_.applied.size(), 1u);
  EXPECT_TRUE(IEquals(probes_.applied[0].program.native(), exe.native()))
      << probes_.applied[0].program;
}

TEST_F(CheckEngine, ChecksNeedTheInstalledSettingsExeToRegister) {
  InstallPerUser();  // no settings executable in the root
  ScheduleResult result = Schedule(true, true);
  EXPECT_EQ(result.code, ExitCode::kNotInstalled);
  EXPECT_EQ(result.refusal, ScheduleRefusal::kNoSettingsExe);
  EXPECT_TRUE(probes_.applied.empty());

  // A folder of that name is not the program either.
  std::error_code error;
  std::filesystem::create_directories(paths_.userRoot / kSettingsExe, error);
  result = Schedule(false, true);
  EXPECT_EQ(result.refusal, ScheduleRefusal::kNoSettingsExe);
  EXPECT_TRUE(probes_.applied.empty());

  // Turning the checks off needs no program: the task goes whatever is installed.
  probes_.task.exists = true;
  probes_.task.actionPath = (paths_.userRoot / L"gone.exe").native();
  result = Schedule(false, false);
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_TRUE(result.remove);
  ASSERT_EQ(probes_.applied.size(), 1u);
  EXPECT_TRUE(HasNoTriggers(probes_.applied[0].options));
  EXPECT_TRUE(probes_.applied[0].program.empty());
  EXPECT_FALSE(result.after.exists);
  EXPECT_EQ(ScheduleResultReport(result).out[0], L"Checks: off (the check task was removed)");
}

// This PC's shape: a 1.x build installed machine-wide, which has no settings executable.
TEST_F(CheckEngine, ChecksRefuseToRegisterForA1xBuild) {
  const std::filesystem::path v1 = MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  RegisterMachineClass(v1 / kHandlerDll);
  SetSlot(0, kOurs);
  SetSlot(1, kOurs);
  probes_.legacyTask = true;
  for (const bool signIn : {true, false}) {
    const ScheduleResult result = Schedule(true, signIn);
    EXPECT_EQ(result.code, ExitCode::kNotInstalled);
    EXPECT_EQ(result.refusal, ScheduleRefusal::kNoSettingsExe);
  }
  EXPECT_TRUE(probes_.applied.empty());
  const ScheduleResult off = Schedule(false, false);
  EXPECT_EQ(off.code, ExitCode::kOk);
  const ScheduleReport report = ScheduleResultReport(off);
  ASSERT_EQ(report.out.size(), 2u);
  EXPECT_EQ(report.out[0], L"Checks: off (there was no check task)");
  EXPECT_NE(report.out[1].find(kLegacyTask), std::wstring::npos);
}

TEST_F(CheckEngine, ChecksNeedAnInstall) {
  PlaceSettingsExe(paths_.userRoot);
  for (const bool on : {true, false}) {
    const ScheduleResult result = Schedule(on, on);
    EXPECT_EQ(result.code, ExitCode::kNotInstalled);
    EXPECT_EQ(result.refusal, ScheduleRefusal::kNotInstalled);
  }
  EXPECT_TRUE(probes_.applied.empty());
  EXPECT_TRUE(KeyIsEmpty(sandbox_.user()));
}

TEST_F(CheckEngine, ChecksRefusedWhereTheyMayNotRun) {
  InstallPerUser();
  PlaceSettingsExe(paths_.userRoot);
  for (const Virtualization virtualization : {Virtualization::kYes, Virtualization::kUnknown}) {
    probes_.context.virtualization = virtualization;
    const ScheduleResult result = Schedule(true, true);
    EXPECT_EQ(result.code, ExitCode::kWrongContext);
    EXPECT_EQ(result.refusal, ScheduleRefusal::kWrongContext);
  }
  probes_.context.virtualization = Virtualization::kNo;
  probes_.context.probe = ContextProbe::kReadOnly;
  probes_.context.views = RegistryViews::kAgree;
  EXPECT_EQ(Schedule(true, true).refusal, ScheduleRefusal::kWrongContext);
  probes_.context.probe = ContextProbe::kWrite;
  probes_.context.elevated = true;
  probes_.context.integrityRid = SECURITY_MANDATORY_HIGH_RID;
  for (const bool on : {true, false}) {
    const ScheduleResult result = Schedule(on, on);
    EXPECT_EQ(result.code, ExitCode::kWrongContext);
    EXPECT_EQ(result.refusal, ScheduleRefusal::kElevated);
  }
  EXPECT_TRUE(probes_.applied.empty());
  EXPECT_EQ(probes_.queries.calls, 0);  // refused before anything was read
}

TEST_F(CheckEngine, ChecksWaitForTheLock) {
  InstallPerUser();
  PlaceSettingsExe(paths_.userRoot);
  test::PipedChild child;
  ASSERT_HRESULT_SUCCEEDED(
      child.Start(test::SelfPath(), L"--pdft-child hold-lock " + test::NamedObjectSuffix()));
  ASSERT_EQ(child.ReadLine(), "locked");
  ScheduleOptions options;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 500;
  ScheduleResult result;
  ASSERT_HRESULT_SUCCEEDED(ChangeCheckSchedule(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kBusy);
  EXPECT_TRUE(result.busy);
  EXPECT_TRUE(probes_.applied.empty());
  child.CloseInput();
  EXPECT_EQ(child.Wait(30000), 0u);
}

TEST_F(CheckEngine, ChecksReportAFailedChange) {
  InstallPerUser();
  PlaceSettingsExe(paths_.userRoot);
  probes_.applyError = kSomeTriggersFailed;
  const ScheduleResult result = Schedule(true, true);
  EXPECT_EQ(result.code, ExitCode::kFailed);
  EXPECT_EQ(result.error, kSomeTriggersFailed);
  EXPECT_EQ(probes_.applied.size(), 1u);
  EXPECT_FALSE(ScheduleResultReport(result).error.empty());
}

// Another account's tasks hold every name the task may have: exit 5, and the report says so in
// words, names those tasks and says who can remove them.
TEST_F(CheckEngine, ChecksSayWhenAnotherAccountHoldsTheTaskNames) {
  InstallPerUser();
  PlaceSettingsExe(paths_.userRoot);
  probes_.applyError = kTaskTakenHr;
  const ScheduleResult result = Schedule(true, true);
  EXPECT_EQ(result.code, ExitCode::kFailed);
  EXPECT_EQ(result.error, kTaskTakenHr);
  EXPECT_EQ(probes_.applied.size(), 1u);
  const ScheduleReport report = ScheduleResultReport(result);
  EXPECT_TRUE(report.out.empty());
  const std::string error = WideToUtf8Lossy(report.error);
  EXPECT_NE(report.error.find(TaskTakenText()), std::wstring::npos) << error;
  EXPECT_NE(report.error.find(L"An administrator can delete "), std::wstring::npos) << error;
  const TaskLocation location = Location();
  EXPECT_NE(report.error.find(location.folder + L"\\" + location.name), std::wstring::npos)
      << error;
  EXPECT_NE(report.error.find(L"\\" + location.fallbackName), std::wstring::npos) << error;
}

// ---- The stale task -----------------------------------------------------------------------------

// A task that runs another program is registered again for the installed one, with its triggers
// and enabled state as they are: what someone turned off in Task Scheduler stays off.
TEST_F(CheckEngine, StaleTaskIsRegisteredAgainWithItsTriggersAsTheyAre) {
  InstallPerUser();
  const std::filesystem::path exe = PlaceSettingsExe(paths_.userRoot);
  probes_.task.exists = true;
  probes_.task.enabled = false;
  probes_.task.atSignIn = true;
  probes_.task.actionPath = (std::filesystem::path(temp_.path()) / L"old" / kSettingsExe).native();
  probes_.task.actionArgs = kCheckTaskArguments;
  probes_.task.options = {TriggerSetting::kDisabled, TriggerSetting::kEnabled, false};

  const CheckResult result = Run();
  EXPECT_TRUE(HasFinding(result.before, FindingId::kTaskStaleAction));
  EXPECT_FALSE(HasFinding(result.findings, FindingId::kTaskStaleAction));
  EXPECT_TRUE(HasFinding(result.findings, FindingId::kTaskDisabled));  // still as it was
  ASSERT_EQ(result.repairs.size(), 1u);
  EXPECT_EQ(result.repairs[0].step.kind, RepairStep::Kind::kReRegisterTask);
  EXPECT_EQ(result.repairs[0].result, RepairResult::kDone);
  ASSERT_EQ(probes_.applied.size(), 1u);
  const FakeProbes::Applied& applied = probes_.applied[0];
  EXPECT_EQ(applied.location.name, Location().name);
  EXPECT_EQ(applied.options.afterPdfCraft, TriggerSetting::kDisabled);
  EXPECT_EQ(applied.options.atSignIn, TriggerSetting::kEnabled);
  EXPECT_FALSE(applied.options.enabled);
  EXPECT_TRUE(IEquals(applied.program.native(), exe.native())) << applied.program;
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(probes_.liveRuns, 1);  // no slot or class changed: the test is not run again

  // Sound now: the next check does nothing.
  EXPECT_TRUE(Run().repairs.empty());
  EXPECT_EQ(probes_.applied.size(), 1u);
}

TEST_F(CheckEngine, StaleTaskStaysWithoutTheInstalledProgram) {
  InstallPerUser();  // no settings executable
  probes_.task.exists = true;
  probes_.task.enabled = true;
  probes_.task.actionPath = L"C:\\Elsewhere\\" + std::wstring(kSettingsExe);
  probes_.task.actionArgs = kCheckTaskArguments;
  probes_.task.options = OptionsFromCheckboxes(true, true);
  const CheckResult result = Run();
  EXPECT_EQ(result.code, ExitCode::kOk);  // reported, not a problem for thumbnails
  EXPECT_TRUE(result.repairs.empty());
  EXPECT_TRUE(probes_.applied.empty());
  bool reported = false;
  for (const Finding& finding : result.findings) {
    if (finding.id != FindingId::kTaskStaleAction) continue;
    reported = true;
    EXPECT_EQ(finding.fix, FixKind::kUserAction);
  }
  EXPECT_TRUE(reported);
}

// Each step looks at the task and the program again just before it changes anything.
TEST_F(CheckEngine, StaleTaskRepairChecksAgainFirst) {
  InstallPerUser();
  const std::filesystem::path exe = PlaceSettingsExe(paths_.userRoot);
  probes_.task.exists = true;
  probes_.task.enabled = true;
  probes_.task.actionPath = L"C:\\Elsewhere\\" + std::wstring(kSettingsExe);
  probes_.task.actionArgs = kCheckTaskArguments;
  probes_.task.options = OptionsFromCheckboxes(true, false);
  const TaskState stale = probes_.task;
  Snapshot s = Gather();
  const RepairPlan plan = PlanRepairs(s, Evaluate(s), RepairMode::kSafe);
  ASSERT_EQ(plan.user.size(), 1u);
  ASSERT_EQ(plan.user[0].kind, RepairStep::Kind::kReRegisterTask);
  EXPECT_TRUE(IEquals(plan.user[0].path.native(), exe.native()));
  std::vector<RepairOutcome> outcomes;
  const auto applyOnce = [&]() -> RepairOutcome {
    EXPECT_HRESULT_SUCCEEDED(ApplyUserRepairs(roots_, probes_, s, plan, &outcomes));
    EXPECT_EQ(outcomes.size(), 1u);
    return outcomes.empty() ? RepairOutcome{} : outcomes[0];
  };

  // Elevated: never.
  s.context.elevated = true;
  RepairOutcome outcome = applyOnce();
  EXPECT_EQ(outcome.result, RepairResult::kNotApplied);
  EXPECT_EQ(outcome.notApplied, NotApplied::kElevated);
  s.context.elevated = false;

  // The program went away meanwhile.
  ASSERT_TRUE(DeleteFileW(exe.c_str()));
  outcome = applyOnce();
  EXPECT_EQ(outcome.result, RepairResult::kNotApplied);
  EXPECT_EQ(outcome.notApplied, NotApplied::kNoProgram);
  PlaceSettingsExe(paths_.userRoot);

  // The task has neither trigger now: registering it "with the same triggers" would delete it.
  probes_.task.options = {TriggerSetting::kAbsent, TriggerSetting::kAbsent, true};
  outcome = applyOnce();
  EXPECT_EQ(outcome.result, RepairResult::kNotApplied);
  EXPECT_EQ(outcome.notApplied, NotApplied::kNoTrigger);

  // Deleted meanwhile, or already registered again: nothing to do.
  probes_.task = {};
  EXPECT_EQ(applyOnce().result, RepairResult::kUnchanged);
  probes_.task = stale;
  probes_.task.actionPath = L"\"" + exe.native() + L"\"";
  EXPECT_EQ(applyOnce().result, RepairResult::kUnchanged);
  EXPECT_TRUE(probes_.applied.empty());

  // Task Scheduler refuses: a failed step.
  probes_.task = stale;
  probes_.applyError = E_ACCESSDENIED;
  outcome = applyOnce();
  EXPECT_EQ(outcome.result, RepairResult::kFailed);
  EXPECT_EQ(outcome.hr, E_ACCESSDENIED);
  EXPECT_EQ(probes_.applied.size(), 1u);
}

}  // namespace
}  // namespace pdft
