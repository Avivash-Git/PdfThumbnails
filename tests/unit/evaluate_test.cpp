// The check's judgement (evaluate.h) on made-up snapshots: one case per row of the findings
// table, the combinations seen in practice (this PC's own shape among them), the scope rule, the
// choice of the per-user build, when Windows' own checks are skipped, and the health and
// exit-code rules.

#include <windows.h>

#include "evaluate.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include "project_config.h"
#include "repair.h"

namespace pdft {
namespace {

const std::wstring kOurs = kHandlerClsidString;
const std::wstring kOther = L"{9C60DE1E-E5FC-40F4-A487-460851A8D915}";
const std::filesystem::path kUserRoot = L"D:\\Home\\someone\\AppData\\Local\\Programs\\Product";
const std::filesystem::path kMachineRoot = L"C:\\Program Files\\Product";

#if defined(_M_ARM64)
constexpr USHORT kNative = IMAGE_FILE_MACHINE_ARM64;
constexpr USHORT kForeign = IMAGE_FILE_MACHINE_AMD64;
constexpr Arch kNativeArch = Arch::kArm64;
#else
constexpr USHORT kNative = IMAGE_FILE_MACHINE_AMD64;
constexpr USHORT kForeign = IMAGE_FILE_MACHINE_ARM64;
constexpr Arch kNativeArch = Arch::kX64;
#endif

ClassReg Class(const std::filesystem::path& dll, const std::wstring& version = L"2.0.0.0") {
  ClassReg reg;
  reg.name = kHandlerClassName;
  reg.dll = dll.native();
  reg.threading = L"Apartment";
  reg.dllExists = true;
  reg.pdfiumExists = true;
  reg.peMachine = kNative;
  reg.fileVersion = version;
  return reg;
}

BuildInfo Build(const std::filesystem::path& root, const std::wstring& name, bool complete = true,
                USHORT machine = kNative) {
  BuildInfo info;
  info.folder.dir = root / L"app" / name;
  info.folder.name = name;
  if (const std::optional<BuildFolderNameParts> parts = ParseBuildFolderName(name)) {
    info.folder.version = parts->version;
  }
  info.folder.is1x = info.folder.version.starts_with(L"1.");
  info.folder.complete = complete;
  if (complete && !info.folder.is1x && !info.folder.version.empty()) info.dllMachine = machine;
  return info;
}

std::filesystem::path DllIn(const BuildInfo& build) { return build.folder.dir / kHandlerDll; }

// Nothing installed, nothing in the way: a process at Medium whose registry view is Explorer's.
Snapshot Empty() {
  Snapshot s;
  s.context.userSid = L"S-1-12-1-1-2-3-4";
  s.context.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
  s.context.probe = ContextProbe::kReadOnly;
  s.context.views = RegistryViews::kAgree;
  s.context.processArch = kNativeArch;
  s.context.osArch = kNativeArch;
  s.paths.userRoot = kUserRoot;
  s.paths.machineRoot = kMachineRoot;
  s.paths.legacyUserRoot = kUserRoot;
  s.paths.legacyMachineRoot = kMachineRoot;
  s.liveSkip = LiveTestSkip::kNotOurHandler;
  return s;
}

// A sound task for |s|'s install root.
void AddTask(Snapshot* s) {
  s->task.exists = true;
  s->task.enabled = true;
  s->task.afterPdfCraft = true;
  s->task.atSignIn = true;
  s->task.actionPath = (s->installRoot / kSettingsExe).native();
  s->task.actionArgs = kCheckTaskArguments;
  s->task.options = OptionsFromCheckboxes(true, true);
}

void Healthy(Snapshot* s) {
  s->settingsExeFound = true;  // a 2.x install has the program the task runs
  s->slots = {kOurs, kOurs};
  s->effective = kOurs;
  s->live = LiveTest{S_OK, {96, 96}, 200};
  s->liveSkip = LiveTestSkip::kNone;
  AddTask(s);
}

// A sound per-user 2.x install.
Snapshot UserInstall() {
  Snapshot s = Empty();
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-1a2b3c4d")};
  s.userClass = Class(DllIn(s.userBuilds[0]));
  s.scope = Scope::kUser;
  s.installRoot = kUserRoot;
  Healthy(&s);
  return s;
}

// A sound machine-wide 2.x install.
Snapshot MachineInstall() {
  Snapshot s = Empty();
  s.machineBuilds = {Build(kMachineRoot, L"2.0.0.0-1a2b3c4d")};
  s.machineClass = Class(DllIn(s.machineBuilds[0]));
  s.scope = Scope::kMachine;
  s.installRoot = kMachineRoot;
  Healthy(&s);
  return s;
}

std::vector<FindingId> Ids(const std::vector<Finding>& findings) {
  std::vector<FindingId> ids;
  for (const Finding& finding : findings) ids.push_back(finding.id);
  return ids;
}

const Finding* Find(const std::vector<Finding>& findings, FindingId id) {
  for (const Finding& finding : findings) {
    if (finding.id == id) return &finding;
  }
  return nullptr;
}

// Each finding's fixed properties, as the table gives them.
void ExpectKind(const Finding& finding, Severity severity, bool breaks, FixKind fix) {
  EXPECT_EQ(finding.severity, severity) << FindingName(finding.id);
  EXPECT_EQ(finding.breaksThumbnails, breaks) << FindingName(finding.id);
  EXPECT_EQ(finding.fix, fix) << FindingName(finding.id);
}

TEST(Evaluate, SoundInstallsAreHealthy) {
  for (const Snapshot& s : {UserInstall(), MachineInstall()}) {
    const std::vector<Finding> findings = Evaluate(s);
    EXPECT_TRUE(findings.empty()) << FindingName(findings.front().id);
    EXPECT_TRUE(HealthyFromFindings(findings));
    EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kOk);
  }
}

// This PC as it is: 1.1 installed machine-wide (its class in the machine-wide app\1.1.0.0-...
// folder), both slots ours, Windows resolving ours, the Explorer test passing, Smart App Control
// on, the 1.x task and settings.ini present, no 2.x task yet. Healthy, with the 1.x leftovers
// reported.
TEST(Evaluate, ThisPcsShapeIsHealthyWithV1Leftovers) {
  Snapshot s = Empty();
  s.machineBuilds = {Build(kMachineRoot, L"1.1.0.0-67a1323e")};
  s.machineClass = Class(DllIn(s.machineBuilds[0]), L"1.1.0.0");
  s.scope = Scope::kMachine;
  s.installRoot = kMachineRoot;
  s.slots = {kOurs, kOurs};
  s.effective = kOurs;
  s.defaultApp.progId = L"PdfCraft.Document";
  s.defaultApp.overlay = L"";
  s.live = LiveTest{S_OK, {96, 96}, 212};
  s.liveSkip = LiveTestSkip::kNone;
  s.sac.state = SacState::kOn;
  s.sacVerdict = DecideSac(SacState::kOn, S_OK, std::nullopt);
  s.legacy.legacyTask = true;
  s.legacy.ini = true;
  s.legacy.iniZoom = 130;
  s.legacy.files = {kUserRoot / L"settings.ini"};
  s.legacy.machineClassInV1 = true;
  s.zoom.decision = {130, ZoomOrigin::kLegacyIni, ZoomValueState::kMissing};
  s.pdfcraft = {true, L"0.14.0", L"{39F7F98F-3A8C-43B4-B6C3-778FB442B040}",
                PdfCraftSource::kUpgradeCode};

  const std::vector<Finding> findings = Evaluate(s);
  EXPECT_EQ(Ids(findings), (std::vector<FindingId>{FindingId::kSacOn, FindingId::kChecksOff,
                                                   FindingId::kV1Leftovers}));
  EXPECT_EQ(Find(findings, FindingId::kSacOn)->sac, SacVerdictKind::kOk);
  ExpectKind(*Find(findings, FindingId::kV1Leftovers), Severity::kInfo, false, FixKind::kNone);
  EXPECT_TRUE(HealthyFromFindings(findings));
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kOk);
  // Nothing for a check to do on its own: the 1.x clean-up is not part of it.
  EXPECT_TRUE(PlanRepairs(s, findings, RepairMode::kSafe).user.empty());
}

TEST(Evaluate, NotInstalled) {
  Snapshot s = Empty();
  s.legacy.runOnce = true;  // without an install it is only a 1.x leftover
  s.zoom.decision.registry = ZoomValueState::kWrongType;
  const std::vector<Finding> findings = Evaluate(s);
  EXPECT_EQ(Ids(findings),
            (std::vector<FindingId>{FindingId::kNotInstalled, FindingId::kV1Leftovers}));
  ExpectKind(findings[0], Severity::kError, true, FixKind::kUserAction);
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kNotInstalled);
  EXPECT_FALSE(HealthyFromFindings(findings));
}

TEST(Evaluate, ClassMissingUserWhenACompleteBuildIsThere) {
  Snapshot s = Empty();
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-1a2b3c4d")};
  const std::vector<Finding> findings = Evaluate(s);
  const Finding* missing = Find(findings, FindingId::kClassMissingUser);
  ASSERT_TRUE(missing);
  ExpectKind(*missing, Severity::kError, true, FixKind::kAuto);
  EXPECT_EQ(missing->path, DllIn(s.userBuilds[0]));
  EXPECT_FALSE(Find(findings, FindingId::kNotInstalled));
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kProblems);
  // An incomplete build, a 1.x build or a build for another architecture is not enough.
  for (const BuildInfo& build :
       {Build(kUserRoot, L"2.0.0.0-1a2b3c4d", false), Build(kUserRoot, L"1.1.0.0-67a1323e"),
        Build(kUserRoot, L"2.0.0.0-1a2b3c4d", true, kForeign)}) {
    s.userBuilds = {build};
    EXPECT_TRUE(Find(Evaluate(s), FindingId::kNotInstalled)) << build.folder.name;
  }
}

TEST(Evaluate, ClassDamagedUser) {
  Snapshot s = UserInstall();
  s.userClass->pdfiumExists = false;
  Finding damaged = *Find(Evaluate(s), FindingId::kClassDamagedUser);
  ExpectKind(damaged, Severity::kError, true, FixKind::kAuto);
  EXPECT_EQ(damaged.path, DllIn(s.userBuilds[0]));

  s.userBuilds.clear();  // nothing to register: reinstall
  s.userClass->dllExists = false;
  damaged = *Find(Evaluate(s), FindingId::kClassDamagedUser);
  ExpectKind(damaged, Severity::kError, true, FixKind::kUserAction);
  EXPECT_TRUE(damaged.path.empty());
}

TEST(Evaluate, ClassDamagedMachine) {
  Snapshot s = MachineInstall();
  s.machineClass->dllExists = false;
  const std::vector<Finding> findings = Evaluate(s);
  ExpectKind(*Find(findings, FindingId::kClassDamagedMachine), Severity::kError, true,
             FixKind::kNeedsAdmin);
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kProblems);
  EXPECT_TRUE(Find(findings, FindingId::kClassDamagedMachine)->path.empty());
}

// The machine-wide class must point straight into a build folder of the machine-wide install
// (or of 1.x's machine-wide install), not into a folder a standard user can write.
TEST(Evaluate, ClassDamagedMachineWhenItPointsElsewhere) {
  const std::filesystem::path elsewhere[] = {
      kUserRoot / L"app" / L"2.0.0.0-1a2b3c4d" / kHandlerDll,  // a per-user build
      std::filesystem::path(L"C:\\Temp") / kHandlerDll,        // anywhere
      kMachineRoot / kHandlerDll,                              // the root, not a build folder
      kMachineRoot / L"app" / kHandlerDll,                     // app\ itself
      kMachineRoot / L"app" / L"not-a-build" / kHandlerDll,    // not a build folder name
      kMachineRoot / L"app" / L"2.0.0.0-1a2b3c4d" / L"sub" / kHandlerDll,  // one level down
      kMachineRoot / L"app" / L"2.0.0.0-1a2b3c4d" / L".." / L".." / L"x" / L"2.0.0.0-1a2b3c4d" /
          kHandlerDll,
      std::filesystem::path(L"C:\\Program Files\\Other\\app\\2.0.0.0-1a2b3c4d") / kHandlerDll,
  };
  for (const std::filesystem::path& dll : elsewhere) {
    Snapshot s = MachineInstall();
    s.machineClass->dll = dll.native();
    const std::vector<Finding> findings = Evaluate(s);
    const Finding* finding = Find(findings, FindingId::kClassDamagedMachine);
    ASSERT_TRUE(finding) << dll.native();
    ExpectKind(*finding, Severity::kError, true, FixKind::kNeedsAdmin);
    EXPECT_EQ(finding->path, dll);
    EXPECT_FALSE(HealthyFromFindings(findings));
    EXPECT_TRUE(PlanRepairs(s, findings, RepairMode::kSafe).machine) << dll.native();
  }

  // A 1.x build folder under 1.x's machine-wide root, and any case, are fine.
  Snapshot s = MachineInstall();
  s.paths.legacyMachineRoot = L"C:\\Program Files\\Legacy";
  s.machineClass->dll = L"C:\\Program Files\\Legacy\\app\\1.1.0.0-67a1323e\\PdfThumbnails.dll";
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kClassDamagedMachine));
  s.machineClass->dll = L"c:\\PROGRAM FILES\\product\\APP\\2.0.0.0-1a2b3c4d\\x.dll";
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kClassDamagedMachine));
  // A 2.x folder under 1.x's root is not 1.x's.
  s.machineClass->dll = L"C:\\Program Files\\Legacy\\app\\2.0.0.0-1a2b3c4d\\x.dll";
  EXPECT_TRUE(Find(Evaluate(s), FindingId::kClassDamagedMachine));

  // A per-user class is not removed while the machine-wide one points elsewhere.
  s = MachineInstall();
  s.machineClass->dll = (kUserRoot / L"app" / L"2.0.0.0-1a2b3c4d" / kHandlerDll).native();
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-99999999")};
  s.userClass = Class(DllIn(s.userBuilds[0]));
  ExpectKind(*Find(Evaluate(s), FindingId::kUserClassShadowsMachine), Severity::kWarning, true,
             FixKind::kNone);
}

TEST(Evaluate, UserClassShadowsMachine) {
  Snapshot s = MachineInstall();
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-99999999")};
  s.userClass = Class(DllIn(s.userBuilds[0]));
  std::vector<Finding> findings = Evaluate(s);
  ExpectKind(*Find(findings, FindingId::kUserClassShadowsMachine), Severity::kWarning, true,
             FixKind::kAuto);
  EXPECT_FALSE(HealthyFromFindings(findings));
  // The build the per-user class uses stays until the class is gone.
  EXPECT_FALSE(Find(findings, FindingId::kOldBuildFolders));

  // With the machine-wide class damaged, the per-user one stays (1.x did the same).
  s.machineClass->pdfiumExists = false;
  findings = Evaluate(s);
  ExpectKind(*Find(findings, FindingId::kUserClassShadowsMachine), Severity::kWarning, true,
             FixKind::kNone);

  // A per-user install of this version forced over 1.x's install for all users stays until this
  // version is installed for all users; a per-user 1.x class over it still goes.
  s = MachineInstall();
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-99999999")};
  s.userClass = Class(DllIn(s.userBuilds[0]));
  s.legacy.machineClassInV1 = true;
  ExpectKind(*Find(Evaluate(s), FindingId::kUserClassShadowsMachine), Severity::kWarning, true,
             FixKind::kNone);
  s.legacy.userClassInV1 = true;
  ExpectKind(*Find(Evaluate(s), FindingId::kUserClassShadowsMachine), Severity::kWarning, true,
             FixKind::kAuto);
}

TEST(Evaluate, ThreadingModel) {
  Snapshot user = UserInstall();
  user.userClass->threading = L"Both";
  Finding finding = *Find(Evaluate(user), FindingId::kThreadingModel);
  ExpectKind(finding, Severity::kError, true, FixKind::kAuto);
  EXPECT_EQ(finding.scope, Scope::kUser);
  EXPECT_EQ(finding.path, user.userClass->dll);  // rewritten in place

  user.userClass->threading = L"apartment";  // compared ignoring case, as 1.x did
  EXPECT_FALSE(Find(Evaluate(user), FindingId::kThreadingModel));

  Snapshot machine = MachineInstall();
  machine.machineClass->threading = L"";
  finding = *Find(Evaluate(machine), FindingId::kThreadingModel);
  ExpectKind(finding, Severity::kError, true, FixKind::kNeedsAdmin);
  EXPECT_EQ(finding.scope, Scope::kMachine);
}

TEST(Evaluate, DllArchMismatch) {
  Snapshot s = MachineInstall();
  s.machineClass->peMachine = kForeign;
  ExpectKind(*Find(Evaluate(s), FindingId::kDllArchMismatch), Severity::kError, true,
             FixKind::kUserAction);
  s.machineClass->peMachine.reset();  // unreadable: nothing to say
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kDllArchMismatch));
}

TEST(Evaluate, SlotEmpty) {
  Snapshot s = MachineInstall();
  s.slots[1].reset();
  const std::vector<Finding> findings = Evaluate(s);
  const Finding* empty = Find(findings, FindingId::kSlotEmpty);
  ASSERT_TRUE(empty);
  ExpectKind(*empty, Severity::kError, true, FixKind::kAuto);
  EXPECT_EQ(empty->slot, 1);
  s.slots[1] = L"";  // an empty string is empty too
  EXPECT_TRUE(Find(Evaluate(s), FindingId::kSlotEmpty));
}

TEST(Evaluate, SlotOtherHandler) {
  Snapshot s = MachineInstall();
  s.slots[0] = kOther;
  s.effective = kOther;
  const std::vector<Finding> findings = Evaluate(s);
  const Finding* other = Find(findings, FindingId::kSlotOtherHandler);
  ASSERT_TRUE(other);
  ExpectKind(*other, Severity::kError, true, FixKind::kTakeBack);
  EXPECT_EQ(other->slot, 0);
  EXPECT_EQ(other->clsid, kOther);
  EXPECT_EQ(Find(findings, FindingId::kEffectiveOther)->clsid, kOther);
  // The Explorer test was not run: Windows resolves the other handler.
  s.live.reset();
  s.liveSkip = LiveTestSkip::kNotOurHandler;
  EXPECT_EQ(Find(Evaluate(s), FindingId::kLiveTestSkipped)->liveSkip, LiveTestSkip::kNotOurHandler);
  // Our CLSID in another case is still ours.
  s.slots[0] = L"{" + std::wstring(kOurs).substr(1);
  for (wchar_t& c : *s.slots[0]) c = static_cast<wchar_t>(towlower(c));
  s.effective = kOurs;
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kSlotOtherHandler));
}

TEST(Evaluate, EffectiveOther) {
  Snapshot s = MachineInstall();
  s.effective.reset();  // Windows resolves nothing
  const Finding effective = *Find(Evaluate(s), FindingId::kEffectiveOther);
  ExpectKind(effective, Severity::kError, true, FixKind::kNone);
  EXPECT_EQ(effective.clsid, L"");
}

TEST(Evaluate, DefaultAppHandlerDoesNotBreak) {
  Snapshot s = MachineInstall();
  s.defaultApp.progId = L"PdfCraft.Document";
  s.defaultApp.handler = kOther;
  const std::vector<Finding> findings = Evaluate(s);
  const Finding* handler = Find(findings, FindingId::kDefaultAppHandler);
  ASSERT_TRUE(handler);
  ExpectKind(*handler, Severity::kWarning, false, FixKind::kNone);
  EXPECT_EQ(handler->clsid, kOther);
  EXPECT_TRUE(HealthyFromFindings(findings));
  s.defaultApp.handler = kOurs;
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kDefaultAppHandler));
}

TEST(Evaluate, LiveTestFailed) {
  Snapshot s = MachineInstall();
  s.live->hr = static_cast<HRESULT>(0x8004B200);
  const Finding failed = *Find(Evaluate(s), FindingId::kLiveTestFailed);
  ExpectKind(failed, Severity::kError, true, FixKind::kNone);
  EXPECT_EQ(failed.hr, static_cast<HRESULT>(0x8004B200));
  // "Class not registered" for a machine-wide install is an ordinary failure.
  s.live->hr = REGDB_E_CLASSNOTREG;
  EXPECT_TRUE(Find(Evaluate(s), FindingId::kLiveTestFailed));
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kPerUserClassIgnored));
}

TEST(Evaluate, PerUserClassIgnored) {
  Snapshot s = UserInstall();
  s.live->hr = REGDB_E_CLASSNOTREG;
  const std::vector<Finding> findings = Evaluate(s);
  ExpectKind(*Find(findings, FindingId::kPerUserClassIgnored), Severity::kError, true,
             FixKind::kUserAction);
  EXPECT_FALSE(Find(findings, FindingId::kLiveTestFailed));
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kProblems);
}

TEST(Evaluate, LiveTestSkipped) {
  Snapshot s = MachineInstall();
  s.live.reset();
  s.liveSkip = LiveTestSkip::kElevated;
  s.context.elevated = true;
  std::vector<Finding> findings = Evaluate(s);
  const Finding skipped = *Find(findings, FindingId::kLiveTestSkipped);
  ExpectKind(skipped, Severity::kInfo, false, FixKind::kNone);
  EXPECT_EQ(skipped.liveSkip, LiveTestSkip::kElevated);
  ExpectKind(*Find(findings, FindingId::kElevated), Severity::kInfo, false, FixKind::kNone);
  EXPECT_TRUE(HealthyFromFindings(findings));

  // Due but it could not start: the reason comes along.
  s.liveSkip = LiveTestSkip::kNone;
  s.liveError = E_OUTOFMEMORY;
  EXPECT_EQ(Find(Evaluate(s), FindingId::kLiveTestSkipped)->hr, E_OUTOFMEMORY);
}

TEST(Evaluate, TestFile) {
  Snapshot s = MachineInstall();
  s.testFile = TestFileRun{};
  s.testFile->result = LiveTest{static_cast<HRESULT>(0x8004B200), {}, 10};
  Finding finding = *Find(Evaluate(s), FindingId::kTestFileFailed);
  ExpectKind(finding, Severity::kError, true, FixKind::kNone);
  s.testFile->result.reset();
  s.testFile->skip = LiveTestSkip::kVirtualized;
  finding = *Find(Evaluate(s), FindingId::kTestFileSkipped);
  ExpectKind(finding, Severity::kInfo, false, FixKind::kNone);
  EXPECT_EQ(finding.liveSkip, LiveTestSkip::kVirtualized);
  s.testFile->problem = TestFileProblem::kNotFound;  // a usage error, not a finding
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kTestFileSkipped));

  // A test that could not even start gives no thumbnail either: 1.x counted that.
  s.testFile = TestFileRun{};
  s.testFile->error = E_OUTOFMEMORY;
  std::vector<Finding> findings = Evaluate(s);
  finding = *Find(findings, FindingId::kTestFileFailed);
  ExpectKind(finding, Severity::kError, true, FixKind::kNone);
  EXPECT_EQ(finding.hr, E_OUTOFMEMORY);
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kProblems);
  s.testFile->error = S_OK;
  EXPECT_EQ(Find(Evaluate(s), FindingId::kTestFileFailed)->hr, E_FAIL);
}

TEST(Evaluate, SacBlockedByCountAndByError) {
  Snapshot s = MachineInstall();
  s.sac.state = SacState::kOn;
  s.live->hr = E_FAIL;
  s.blocks = BlockCount{true, 3, S_OK};
  s.sacVerdict = DecideSac(s.sac.state, s.live->hr, s.blocks);
  Finding blocked = *Find(Evaluate(s), FindingId::kSacBlocked);
  ExpectKind(blocked, Severity::kError, true, FixKind::kUserAction);
  EXPECT_EQ(blocked.count, 3);
  EXPECT_FALSE(blocked.hr);

  const HRESULT refused = HRESULT_FROM_WIN32(4551);
  s.live->hr = refused;
  s.blocks = BlockCount{true, 0, S_OK};
  s.sacVerdict = DecideSac(s.sac.state, s.live->hr, s.blocks);
  const std::vector<Finding> findings = Evaluate(s);
  blocked = *Find(findings, FindingId::kSacBlocked);
  EXPECT_EQ(blocked.count, 0);
  EXPECT_EQ(blocked.hr, refused);
  // The Explorer test's own failure is reported too, as 1.x did.
  EXPECT_EQ(Find(findings, FindingId::kLiveTestFailed)->hr, refused);
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kProblems);
}

TEST(Evaluate, SacInformation) {
  Snapshot s = MachineInstall();
  const struct {
    SacVerdictKind kind;
    FindingId id;
  } rows[] = {
      {SacVerdictKind::kEval, FindingId::kSacEvaluation},
      {SacVerdictKind::kOk, FindingId::kSacOn},
      {SacVerdictKind::kOn, FindingId::kSacOn},
      {SacVerdictKind::kOnUnknown, FindingId::kSacOn},
      {SacVerdictKind::kUnknown, FindingId::kSacUnknown},
  };
  for (const auto& row : rows) {
    s.sacVerdict = SacVerdict{row.kind, 0, std::nullopt};
    const std::vector<Finding> findings = Evaluate(s);
    const Finding* finding = Find(findings, row.id);
    ASSERT_TRUE(finding) << static_cast<int>(row.kind);
    ExpectKind(*finding, Severity::kInfo, false, FixKind::kNone);
    EXPECT_EQ(finding->sac, row.kind);
    EXPECT_TRUE(HealthyFromFindings(findings));
  }
  s.sacVerdict = SacVerdict{};
  EXPECT_TRUE(Evaluate(s).empty());
}

TEST(Evaluate, ExplorerSettingsWarnWithoutBreaking) {
  Snapshot s = MachineInstall();
  s.explorer.iconsOnly = true;
  s.explorer.thumbnailsOffByMachinePolicy = true;
  const std::vector<Finding> findings = Evaluate(s);
  ExpectKind(*Find(findings, FindingId::kIconsOnly), Severity::kWarning, false,
             FixKind::kUserAction);
  ExpectKind(*Find(findings, FindingId::kThumbnailsDisabledByPolicy), Severity::kWarning, false,
             FixKind::kUserAction);
  EXPECT_TRUE(HealthyFromFindings(findings));
  s.explorer = {};
  s.explorer.thumbnailsOffByUserPolicy = true;
  EXPECT_TRUE(Find(Evaluate(s), FindingId::kThumbnailsDisabledByPolicy));
}

TEST(Evaluate, Task) {
  Snapshot s = MachineInstall();
  s.task = {};
  ExpectKind(*Find(Evaluate(s), FindingId::kChecksOff), Severity::kInfo, false,
             FixKind::kUserAction);

  AddTask(&s);
  s.task.actionPath = L"\"" + s.task.actionPath + L"\"";  // quoted is the same program
  EXPECT_TRUE(Evaluate(s).empty());

  s.task.actionPath = (kUserRoot / kSettingsExe).native();  // another install's program
  Finding stale = *Find(Evaluate(s), FindingId::kTaskStaleAction);
  ExpectKind(stale, Severity::kWarning, false, FixKind::kAuto);
  EXPECT_EQ(stale.path, kMachineRoot / kSettingsExe);
  AddTask(&s);
  s.task.actionArgs = L"--check";
  EXPECT_TRUE(Find(Evaluate(s), FindingId::kTaskStaleAction));

  AddTask(&s);
  s.task.enabled = false;
  ExpectKind(*Find(Evaluate(s), FindingId::kTaskDisabled), Severity::kWarning, false,
             FixKind::kUserAction);

  // Without the installed program the task cannot be pointed at it: reported, not repaired.
  AddTask(&s);
  s.task.actionPath = (kUserRoot / kSettingsExe).native();
  s.settingsExeFound = false;
  stale = *Find(Evaluate(s), FindingId::kTaskStaleAction);
  ExpectKind(stale, Severity::kWarning, false, FixKind::kUserAction);
  EXPECT_EQ(stale.path, kMachineRoot / kSettingsExe);

  // Neither of the check's triggers (a daily one, or an event trigger with another query, reads
  // as none): registering it again with the same triggers would delete it, so --checks it is.
  AddTask(&s);
  s.settingsExeFound = true;
  s.task.actionPath = (kUserRoot / kSettingsExe).native();
  s.task.options = {TriggerSetting::kAbsent, TriggerSetting::kAbsent, true};
  stale = *Find(Evaluate(s), FindingId::kTaskStaleAction);
  ExpectKind(stale, Severity::kWarning, false, FixKind::kUserAction);
  EXPECT_TRUE(PlanRepairs(s, Evaluate(s), RepairMode::kSafe).user.empty());
  // One disabled trigger is still one to keep.
  s.task.options.atSignIn = TriggerSetting::kDisabled;
  ExpectKind(*Find(Evaluate(s), FindingId::kTaskStaleAction), Severity::kWarning, false,
             FixKind::kAuto);
}

// The program a task may run is the installed settings executable, there, in the root of the
// scope the snapshot found.
TEST(Evaluate, InstalledSettingsExe) {
  Snapshot s = MachineInstall();
  EXPECT_EQ(InstalledSettingsExe(s), kMachineRoot / kSettingsExe);
  s.installRoot = kMachineRoot / L"";  // a trailing separator names the same folder
  EXPECT_EQ(InstalledSettingsExe(s), kMachineRoot / kSettingsExe);
  s = UserInstall();
  EXPECT_EQ(InstalledSettingsExe(s), kUserRoot / kSettingsExe);

  s.settingsExeFound = false;  // a 1.x build has none
  EXPECT_FALSE(InstalledSettingsExe(s));
  s = UserInstall();
  s.installRoot = kMachineRoot;  // not the root of its scope
  EXPECT_FALSE(InstalledSettingsExe(s));
  s = UserInstall();
  s.paths.userRoot = L"relative\root";
  s.installRoot = s.paths.userRoot;
  EXPECT_FALSE(InstalledSettingsExe(s));
  s = Empty();
  s.settingsExeFound = true;
  EXPECT_FALSE(InstalledSettingsExe(s));  // nothing installed
}

// A per-user install forced over 1.x's install for all users: the scope stays the machine-wide
// one, whose root has no settings executable, and the program is the per-user one.
TEST(Evaluate, InstalledSettingsExeOfAPerUserInstallOverV1) {
  Snapshot s = Empty();
  s.machineBuilds = {Build(kMachineRoot, L"1.1.0.0-67a1323e")};
  s.machineClass = Class(DllIn(s.machineBuilds[0]), L"1.1.0.0");
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-1a2b3c4d")};
  s.userClass = Class(DllIn(s.userBuilds[0]));
  s.scope = Scope::kMachine;
  s.installRoot = kMachineRoot;
  s.settingsExeFound = false;
  s.userSettingsExeFound = true;
  EXPECT_TRUE(UserInstallOverV1Machine(s.machineClass, s.userClass, s.paths));
  EXPECT_EQ(InstalledSettingsExe(s), kUserRoot / kSettingsExe);

  s.userSettingsExeFound = false;  // the per-user program is not there
  EXPECT_FALSE(InstalledSettingsExe(s));
  s.userSettingsExeFound = true;
  // A per-user class in a 1.x build folder, or outside the per-user root's build folders.
  s.userClass = Class(kUserRoot / L"app" / L"1.1.0.0-67a1323e" / kHandlerDll, L"1.1.0.0");
  EXPECT_FALSE(UserInstallOverV1Machine(s.machineClass, s.userClass, s.paths));
  EXPECT_FALSE(InstalledSettingsExe(s));
  s.userClass = Class(std::filesystem::path(L"D:\\Elsewhere\\app\\2.0.0.0-1a2b3c4d") / kHandlerDll);
  EXPECT_FALSE(InstalledSettingsExe(s));
  // A machine-wide class of this version: its own root's program or none.
  s = MachineInstall();
  s.userClass = Class(DllIn(Build(kUserRoot, L"2.0.0.0-1a2b3c4d")));
  s.settingsExeFound = false;
  s.userSettingsExeFound = true;
  EXPECT_FALSE(UserInstallOverV1Machine(s.machineClass, s.userClass, s.paths));
  EXPECT_FALSE(InstalledSettingsExe(s));
}

TEST(Evaluate, CleanupPendingOnlyWhileInstalled) {
  Snapshot s = UserInstall();
  s.legacy.runOnce = true;
  const std::vector<Finding> findings = Evaluate(s);
  ExpectKind(*Find(findings, FindingId::kCleanupPending), Severity::kWarning, false,
             FixKind::kAuto);
  EXPECT_TRUE(Find(findings, FindingId::kV1Leftovers));
  EXPECT_TRUE(HealthyFromFindings(findings));
}

// Files in use that an elevated step moved into trash folders: information, nothing to repair,
// and a pending sign-in clean-up still comes first.
TEST(Evaluate, CleanupPendingForTrashFolders) {
  Snapshot s = MachineInstall();
  s.pendingTrash = {L"x.trash-1", L"x.trash-2"};
  std::vector<Finding> findings = Evaluate(s);
  const Finding* finding = Find(findings, FindingId::kCleanupPending);
  ASSERT_TRUE(finding);
  ExpectKind(*finding, Severity::kInfo, false, FixKind::kNone);
  EXPECT_EQ(finding->count, 2);
  EXPECT_TRUE(HealthyFromFindings(findings));
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kOk);
  s.legacy.runOnceOthers = true;
  ExpectKind(*Find(Evaluate(s), FindingId::kCleanupPending), Severity::kWarning, false,
             FixKind::kAuto);
  s = Empty();
  s.pendingTrash = {L"x.trash-1"};
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kCleanupPending));
}

TEST(Evaluate, V1Leftovers) {
  Snapshot s = UserInstall();
  for (int which = 0; which < 6; ++which) {
    s.legacy = {};
    switch (which) {
      case 0:
        s.legacy.legacyTask = true;
        break;
      case 1:
        s.legacy.ini = true;
        break;
      case 2:
        s.legacy.files = {kUserRoot / L"bin"};
        break;
      case 3:
        s.legacy.userClassInV1 = true;
        break;
      case 4:
        s.legacy.runOnce = true;
        break;
      default:
        // The machine-wide class in a 1.x build is the install in use, not a leftover.
        s.legacy.machineClassInV1 = true;
        break;
    }
    const std::vector<Finding> findings = Evaluate(s);
    const Finding* finding = Find(findings, FindingId::kV1Leftovers);
    if (which == 5) {
      EXPECT_FALSE(finding);
      continue;
    }
    ASSERT_TRUE(finding) << which;
    ExpectKind(*finding, Severity::kInfo, false, FixKind::kNone);
  }
}

TEST(Evaluate, OldBuildFoldersAreOnlyUnused2xFolders) {
  Snapshot s = UserInstall();                                     // registered: 2.0.0.0-1a2b3c4d
  s.userBuilds.push_back(Build(kUserRoot, L"1.9.0.0-00000001"));  // 1.x: never
  s.userBuilds.push_back(Build(kUserRoot, L"1.1.0.0-67a1323e"));  // 1.x: never
  s.userBuilds.push_back(Build(kUserRoot, L"2.0.0.0-00000002", false));  // incomplete 2.x: old
  s.userBuilds.push_back(Build(kUserRoot, L"1.9.0.0-00000003"));         // another 1.x name
  s.userBuilds.push_back(Build(kUserRoot, L"notes"));                    // not a build name
  s.userBuilds.push_back(Build(kUserRoot, L"2.1.0.0-00000004"));         // newer, unused: old
  const Finding old = *Find(Evaluate(s), FindingId::kOldBuildFolders);
  ExpectKind(old, Severity::kInfo, false, FixKind::kAuto);
  EXPECT_EQ(old.count, 2);
  EXPECT_EQ(old.paths, (std::vector<std::filesystem::path>{s.userBuilds[3].folder.dir,
                                                           s.userBuilds[6].folder.dir}));
}

TEST(Evaluate, OldBuildFoldersKeepTheBuildARepairRegisters) {
  Snapshot s = Empty();  // the class went missing; the newest complete build gets it
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-00000001"), Build(kUserRoot, L"2.0.1.0-00000002")};
  const std::vector<Finding> findings = Evaluate(s);
  EXPECT_EQ(Find(findings, FindingId::kClassMissingUser)->path, DllIn(s.userBuilds[1]));
  EXPECT_EQ(Find(findings, FindingId::kOldBuildFolders)->paths,
            (std::vector<std::filesystem::path>{s.userBuilds[0].folder.dir}));
}

TEST(Evaluate, InAMachineInstallEveryPerUser2xBuildIsOld) {
  Snapshot s = MachineInstall();
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-1a2b3c4d")};
  EXPECT_EQ(Find(Evaluate(s), FindingId::kOldBuildFolders)->count, 1);
}

TEST(Evaluate, ZoomInvalid) {
  Snapshot s = MachineInstall();
  s.zoom.decision = {200, ZoomOrigin::kRegistry, ZoomValueState::kOutOfRange};
  Finding zoom = *Find(Evaluate(s), FindingId::kZoomInvalid);
  ExpectKind(zoom, Severity::kWarning, false, FixKind::kAuto);
  EXPECT_EQ(zoom.zoom, 200);
  // A value of the wrong type: the zoom in effect (here the 1.x file's) is what gets written.
  s.zoom.decision = {130, ZoomOrigin::kLegacyIni, ZoomValueState::kWrongType};
  EXPECT_EQ(Find(Evaluate(s), FindingId::kZoomInvalid)->zoom, 130);
  s.zoom.decision = {120, ZoomOrigin::kRegistry, ZoomValueState::kValid};
  EXPECT_FALSE(Find(Evaluate(s), FindingId::kZoomInvalid));
}

TEST(Evaluate, PdfCraftChanged) {
  Snapshot s = MachineInstall();
  s.pdfcraft = {true, L"0.15.0", L"", PdfCraftSource::kUpgradeCode};
  EXPECT_TRUE(Evaluate(s).empty());  // no earlier record: nothing to compare with
  s.last = CheckRecord{};
  s.last->pdfcraftVersion = L"0.14.0";
  const Finding changed = *Find(Evaluate(s), FindingId::kPdfCraftChanged);
  ExpectKind(changed, Severity::kInfo, false, FixKind::kNone);
  EXPECT_EQ(changed.from, L"0.14.0");
  EXPECT_EQ(changed.to, L"0.15.0");
  s.pdfcraft = {};  // uninstalled
  EXPECT_EQ(Find(Evaluate(s), FindingId::kPdfCraftChanged)->to, L"");
  s.last->pdfcraftVersion = L"";
  EXPECT_TRUE(Evaluate(s).empty());
}

TEST(Evaluate, WhereItRuns) {
  Snapshot s = MachineInstall();
  s.context.differentFromShellUser = true;
  ExpectKind(*Find(Evaluate(s), FindingId::kDifferentUser), Severity::kWarning, false,
             FixKind::kNone);

  // A process that may not see Explorer's registry: Windows' own checks are skipped and so is
  // the Explorer test; nothing that depends on them is claimed.
  s = MachineInstall();
  s.windowsChecksSkipped = true;
  s.windowsChecksSkip = WindowsChecksSkip::kViewsDiffer;
  s.effective.reset();
  s.live.reset();
  s.liveSkip = LiveTestSkip::kVirtualized;
  const std::vector<Finding> findings = Evaluate(s);
  EXPECT_EQ(Ids(findings),
            (std::vector<FindingId>{FindingId::kLiveTestSkipped, FindingId::kVirtualized}));
  EXPECT_EQ(findings[1].checksSkip, WindowsChecksSkip::kViewsDiffer);
  ExpectKind(findings[1], Severity::kWarning, false, FixKind::kNone);
  EXPECT_TRUE(HealthyFromFindings(findings));

  // The slots and the per-user class are read in-process too: there they may be the private
  // copy, which is not what File Explorer reads, so they are not judged either.
  for (const WindowsChecksSkip skip :
       {WindowsChecksSkip::kViewsDiffer, WindowsChecksSkip::kViewsUnknown,
        WindowsChecksSkip::kNotChecked, WindowsChecksSkip::kVirtualized}) {
    s.windowsChecksSkip = skip;
    s.slots = {std::nullopt, kOther};
    s.userBuilds = {Build(kUserRoot, L"2.0.0.0-99999999")};
    s.userClass = Class(DllIn(s.userBuilds[0]));
    const std::vector<Finding> skipped = Evaluate(s);
    EXPECT_EQ(Ids(skipped),
              (std::vector<FindingId>{FindingId::kLiveTestSkipped, FindingId::kVirtualized}));
    EXPECT_EQ(ExitCodeFromFindings(skipped), ExitCode::kOk);
  }
}

TEST(Evaluate, AnotherHandlerInASlotWithAPerUserClassShadowing) {
  Snapshot s = MachineInstall();
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-99999999")};
  s.userClass = Class(DllIn(s.userBuilds[0]));
  s.slots[1] = kOther;
  const std::vector<Finding> findings = Evaluate(s);
  EXPECT_EQ(Ids(findings), (std::vector<FindingId>{FindingId::kUserClassShadowsMachine,
                                                   FindingId::kSlotOtherHandler}));
  EXPECT_EQ(ExitCodeFromFindings(findings), ExitCode::kProblems);
}

TEST(Evaluate, FindingNamesAreDistinct) {
  std::vector<std::wstring> names;
  for (int id = 0; id <= static_cast<int>(FindingId::kVirtualized); ++id) {
    names.push_back(FindingName(static_cast<FindingId>(id)));
    EXPECT_NE(names.back(), L"Unknown") << id;
  }
  std::sort(names.begin(), names.end());
  EXPECT_EQ(std::adjacent_find(names.begin(), names.end()), names.end());
}

TEST(Snapshot, ScopeRule) {
  const std::optional<ClassReg> some = ClassReg{};
  EXPECT_EQ(DecideScope(std::nullopt, std::nullopt), Scope::kNone);
  EXPECT_EQ(DecideScope(std::nullopt, some), Scope::kUser);
  EXPECT_EQ(DecideScope(some, std::nullopt), Scope::kMachine);
  EXPECT_EQ(DecideScope(some, some), Scope::kMachine);
}

TEST(Snapshot, WindowsChecksSkip) {
  ProcessContext context;
  EXPECT_EQ(DecideWindowsChecksSkip(context), WindowsChecksSkip::kNone);
  context.packagedAncestor = L"Some.App_1.0.0.0_x64__abc";
  EXPECT_EQ(DecideWindowsChecksSkip(context), WindowsChecksSkip::kNotChecked);

  context.probe = ContextProbe::kReadOnly;
  context.views = RegistryViews::kAgree;
  EXPECT_EQ(DecideWindowsChecksSkip(context), WindowsChecksSkip::kNone);
  context.views = RegistryViews::kDiffer;
  EXPECT_EQ(DecideWindowsChecksSkip(context), WindowsChecksSkip::kViewsDiffer);
  context.views = RegistryViews::kUnknown;
  EXPECT_EQ(DecideWindowsChecksSkip(context), WindowsChecksSkip::kViewsUnknown);

  context.probe = ContextProbe::kWrite;
  context.virtualization = Virtualization::kNo;
  EXPECT_EQ(DecideWindowsChecksSkip(context), WindowsChecksSkip::kNone);
  context.virtualization = Virtualization::kYes;
  EXPECT_EQ(DecideWindowsChecksSkip(context), WindowsChecksSkip::kVirtualized);
  context.virtualization = Virtualization::kUnknown;
  EXPECT_EQ(DecideWindowsChecksSkip(context), WindowsChecksSkip::kVirtualized);
}

TEST(Snapshot, ChooseUserBuildByVersionThenId) {
  Snapshot s = Empty();
  s.userBuilds = {Build(kUserRoot, L"2.0.0.0-ffffffff"), Build(kUserRoot, L"2.10.0.0-00000001"),
                  Build(kUserRoot, L"2.9.0.0-00000002"), Build(kUserRoot, L"2.10.0.0-00000000")};
  EXPECT_EQ(ChooseUserBuildDll(s), DllIn(s.userBuilds[1]));  // 2.10 > 2.9; then the higher id
  s.userBuilds[1].folder.complete = false;
  EXPECT_EQ(ChooseUserBuildDll(s), DllIn(s.userBuilds[3]));
  s.userBuilds[3].dllMachine = kForeign;
  EXPECT_EQ(ChooseUserBuildDll(s), DllIn(s.userBuilds[2]));
  s.userBuilds[2].dllMachine.reset();  // headers unreadable
  EXPECT_EQ(ChooseUserBuildDll(s), DllIn(s.userBuilds[0]));
  s.context.osArch = Arch::kOther;
  EXPECT_FALSE(ChooseUserBuildDll(s));
}

TEST(Snapshot, DllIsInFolder) {
  EXPECT_TRUE(DllIsInFolder(L"C:\\a\\app\\b\\x.dll", L"C:\\a\\app\\b"));
  EXPECT_TRUE(DllIsInFolder(L"C:\\a\\app\\b\\x.dll", L"c:\\A\\app\\B\\"));
  EXPECT_TRUE(DllIsInFolder(L"C:\\a\\app\\c\\..\\b\\x.dll", L"C:\\a\\app\\b"));
  EXPECT_FALSE(DllIsInFolder(L"C:\\a\\app\\bc\\x.dll", L"C:\\a\\app\\b"));
  EXPECT_FALSE(DllIsInFolder(L"C:\\a\\app\\b\\sub\\x.dll", L"C:\\a\\app\\b"));
  EXPECT_FALSE(DllIsInFolder(L"", L"C:\\a"));
}

}  // namespace
}  // namespace pdft
