// Planning repairs (repair.h) and the check's exit code, summary and log fields (check.h), all
// pure: which findings each mode repairs, in which order, what is never done without asking
// (taking slots back) or never done here at all (the administrator step, the 1.x clean-up).

#include <windows.h>

#include "repair.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "check.h"
#include "evaluate.h"

namespace pdft {
namespace {

using Kind = RepairStep::Kind;

Finding Make(FindingId id, FixKind fix, bool breaks = true) {
  Finding finding;
  finding.id = id;
  finding.fix = fix;
  finding.breaksThumbnails = breaks;
  finding.severity = breaks ? Severity::kError : Severity::kWarning;
  return finding;
}

Finding Slot(FindingId id, int slot) {
  Finding finding = Make(id, id == FindingId::kSlotEmpty ? FixKind::kAuto : FixKind::kTakeBack);
  finding.slot = slot;
  return finding;
}

RepairPlan Plan(const std::vector<Finding>& findings, RepairMode mode) {
  return PlanRepairs(Snapshot{}, findings, mode);
}

std::vector<Kind> Kinds(const RepairPlan& plan) {
  std::vector<Kind> kinds;
  for (const RepairStep& step : plan.user) kinds.push_back(step.kind);
  return kinds;
}

const std::filesystem::path kDll =
    L"D:\\Home\\someone\\AppData\\Local\\Programs\\Product\\app"
    L"\\2.0.0.0-1a2b3c4d\\Handler.dll";
const std::filesystem::path kOld =
    L"D:\\Home\\someone\\AppData\\Local\\Programs\\Product\\app"
    L"\\2.0.0.0-00000001";

// Every finding a check can repair on its own, plus one it must not touch without a request.
std::vector<Finding> Everything() {
  std::vector<Finding> findings;
  Finding missing = Make(FindingId::kClassDamagedUser, FixKind::kAuto);
  missing.path = kDll;
  findings.push_back(missing);
  findings.push_back(Slot(FindingId::kSlotOtherHandler, 0));
  findings.push_back(Slot(FindingId::kSlotEmpty, 1));
  Finding stale = Make(FindingId::kTaskStaleAction, FixKind::kAuto, false);
  stale.path = L"C:\\Program Files\\Product\\Settings.exe";
  findings.push_back(stale);
  findings.push_back(Make(FindingId::kCleanupPending, FixKind::kAuto, false));
  findings.push_back(Make(FindingId::kV1Leftovers, FixKind::kNone, false));
  Finding old = Make(FindingId::kOldBuildFolders, FixKind::kAuto, false);
  old.paths = {kOld};
  findings.push_back(old);
  Finding zoom = Make(FindingId::kZoomInvalid, FixKind::kAuto, false);
  zoom.zoom = 130;
  findings.push_back(zoom);
  return findings;
}

TEST(PlanRepairs, NoneRepairsNothing) {
  const RepairPlan plan = Plan(Everything(), RepairMode::kNone);
  EXPECT_TRUE(plan.user.empty());
  EXPECT_FALSE(plan.machine);
}

TEST(PlanRepairs, SafeInItsOrderAndNeverTakesBack) {
  const RepairPlan plan = Plan(Everything(), RepairMode::kSafe);
  EXPECT_EQ(Kinds(plan),
            (std::vector<Kind>{Kind::kDeleteRunOnce, Kind::kRegisterUserClass, Kind::kFillSlot,
                               Kind::kRewriteZoom, Kind::kReRegisterTask, Kind::kRemoveUserBuild}));
  EXPECT_EQ(plan.user[1].path, kDll);
  EXPECT_EQ(plan.user[2].slot, 1);
  EXPECT_EQ(plan.user[3].zoom, 130);
  EXPECT_EQ(plan.user[5].path, kOld);
  EXPECT_FALSE(plan.machine);
}

TEST(PlanRepairs, TakeBackOnlyOnRequest) {
  const RepairPlan plan = Plan(Everything(), RepairMode::kSafeAndTakeBack);
  EXPECT_EQ(Kinds(plan),
            (std::vector<Kind>{Kind::kDeleteRunOnce, Kind::kRegisterUserClass, Kind::kTakeBackSlot,
                               Kind::kFillSlot, Kind::kRewriteZoom, Kind::kReRegisterTask,
                               Kind::kRemoveUserBuild}));
  EXPECT_EQ(plan.user[2].slot, 0);
}

TEST(PlanRepairs, NothingWhenNotInstalled) {
  std::vector<Finding> findings = Everything();
  findings.insert(findings.begin(), Make(FindingId::kNotInstalled, FixKind::kUserAction));
  EXPECT_TRUE(Plan(findings, RepairMode::kSafeAndTakeBack).user.empty());
}

TEST(PlanRepairs, OneClassRegistration) {
  // A missing class, a damaged one and a threading model all end in the same registration.
  Finding damaged = Make(FindingId::kClassDamagedUser, FixKind::kAuto);
  damaged.path = kDll;
  Finding threading = Make(FindingId::kThreadingModel, FixKind::kAuto);
  threading.scope = Scope::kUser;
  threading.path = kDll;
  const RepairPlan plan = Plan({damaged, threading}, RepairMode::kSafe);
  EXPECT_EQ(Kinds(plan), (std::vector<Kind>{Kind::kRegisterUserClass}));

  // Nothing to register (the user has to reinstall): no step.
  damaged.fix = FixKind::kUserAction;
  damaged.path.clear();
  EXPECT_TRUE(Plan({damaged}, RepairMode::kSafe).user.empty());
}

TEST(PlanRepairs, ShadowingClassOnlyWhenTheMachineClassIsSound) {
  EXPECT_EQ(
      Kinds(Plan({Make(FindingId::kUserClassShadowsMachine, FixKind::kAuto)}, RepairMode::kSafe)),
      (std::vector<Kind>{Kind::kDeleteUserClass}));
  EXPECT_TRUE(Plan({Make(FindingId::kUserClassShadowsMachine, FixKind::kNone)}, RepairMode::kSafe)
                  .user.empty());
}

TEST(PlanRepairs, MachinePartNamesTheAdministratorStepOnly) {
  for (const RepairMode mode : {RepairMode::kSafe, RepairMode::kSafeAndTakeBack}) {
    RepairPlan plan = Plan({Make(FindingId::kClassDamagedMachine, FixKind::kNeedsAdmin)}, mode);
    EXPECT_TRUE(plan.user.empty());
    EXPECT_EQ(plan.machine, ElevatedVerb::kMachineRepairClass);

    Finding threading = Make(FindingId::kThreadingModel, FixKind::kNeedsAdmin);
    threading.scope = Scope::kMachine;
    plan = Plan({threading}, mode);
    EXPECT_TRUE(plan.user.empty());
    EXPECT_EQ(plan.machine, ElevatedVerb::kMachineRepairClass);
  }
  EXPECT_FALSE(
      Plan({Make(FindingId::kClassDamagedMachine, FixKind::kNeedsAdmin)}, RepairMode::kNone)
          .machine);
}

TEST(PlanRepairs, ReportOnlyFindingsPlanNothing) {
  const std::vector<Finding> findings = {
      Make(FindingId::kEffectiveOther, FixKind::kNone),
      Make(FindingId::kDllArchMismatch, FixKind::kUserAction),
      Make(FindingId::kLiveTestFailed, FixKind::kNone),
      Make(FindingId::kPerUserClassIgnored, FixKind::kUserAction),
      Make(FindingId::kSacBlocked, FixKind::kUserAction),
      Make(FindingId::kIconsOnly, FixKind::kUserAction, false),
      Make(FindingId::kTaskDisabled, FixKind::kUserAction, false),
      Make(FindingId::kChecksOff, FixKind::kUserAction, false),
      // The installed program is not there: a task registered again would point at nothing.
      Make(FindingId::kTaskStaleAction, FixKind::kUserAction, false),
      Make(FindingId::kV1Leftovers, FixKind::kNone, false),
      Make(FindingId::kPdfCraftChanged, FixKind::kNone, false),
  };
  const RepairPlan plan = Plan(findings, RepairMode::kSafeAndTakeBack);
  EXPECT_TRUE(plan.user.empty());
  EXPECT_FALSE(plan.machine);
}

TEST(RepairOutcomes, Counting) {
  std::vector<RepairOutcome> outcomes(4);
  outcomes[0].step.kind = Kind::kRewriteZoom;
  outcomes[0].result = RepairResult::kDone;
  outcomes[1].step.kind = Kind::kFillSlot;
  outcomes[1].result = RepairResult::kUnchanged;
  outcomes[2].step.kind = Kind::kRemoveUserBuild;
  outcomes[2].result = RepairResult::kKept;
  outcomes[3].step.kind = Kind::kReRegisterTask;
  outcomes[3].result = RepairResult::kNotApplied;
  EXPECT_EQ(CountDone(outcomes), 1);
  EXPECT_FALSE(AnyFailed(outcomes));
  EXPECT_FALSE(ChangedSlotOrClass(outcomes));  // the slot was not written
  outcomes[1].result = RepairResult::kDone;
  EXPECT_TRUE(ChangedSlotOrClass(outcomes));
  outcomes[2].result = RepairResult::kFailed;
  EXPECT_TRUE(AnyFailed(outcomes));
  for (Kind kind : {Kind::kRegisterUserClass, Kind::kDeleteUserClass, Kind::kTakeBackSlot}) {
    std::vector<RepairOutcome> one(1);
    one[0].step.kind = kind;
    one[0].result = RepairResult::kDone;
    EXPECT_TRUE(ChangedSlotOrClass(one)) << RepairStepName(kind);
  }
}

TEST(RepairOutcomes, InUseErrors) {
  EXPECT_TRUE(IsInUseError(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)));
  EXPECT_TRUE(IsInUseError(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)));
  EXPECT_TRUE(IsInUseError(HRESULT_FROM_WIN32(ERROR_USER_MAPPED_FILE)));
  EXPECT_FALSE(IsInUseError(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)));
  EXPECT_FALSE(IsInUseError(E_INVALIDARG));
}

TEST(CheckExitCode, Rules) {
  const std::vector<Finding> healthy = {Make(FindingId::kChecksOff, FixKind::kUserAction, false)};
  const std::vector<Finding> broken = {Slot(FindingId::kSlotOtherHandler, 0)};
  const std::vector<Finding> notInstalled = {Make(FindingId::kNotInstalled, FixKind::kUserAction)};
  const std::vector<Finding> machine = {
      Make(FindingId::kClassDamagedMachine, FixKind::kNeedsAdmin)};

  EXPECT_EQ(CheckExitCode(healthy, false, false, false), ExitCode::kOk);
  EXPECT_EQ(CheckExitCode(healthy, true, false, false), ExitCode::kProblems);  // a step failed
  EXPECT_EQ(CheckExitCode(broken, false, false, false), ExitCode::kProblems);
  EXPECT_EQ(CheckExitCode(broken, false, false, true), ExitCode::kProblems);
  EXPECT_EQ(CheckExitCode(notInstalled, false, false, true), ExitCode::kNotInstalled);
  // The machine-wide part: a quiet check cannot ask anybody (7); otherwise problems remain.
  EXPECT_EQ(CheckExitCode(machine, false, true, true), ExitCode::kNeedsAdmin);
  EXPECT_EQ(CheckExitCode(machine, false, true, false), ExitCode::kProblems);
  EXPECT_EQ(CheckExitCode(healthy, false, true, false), ExitCode::kProblems);
}

TEST(CheckSummary, Wording) {
  EXPECT_EQ(CheckSummary(ExitCode::kOk, 0, 0), L"nothing to fix");
  EXPECT_EQ(CheckSummary(ExitCode::kOk, 2, 0), L"fixed 2");
  EXPECT_EQ(CheckSummary(ExitCode::kProblems, 0, 1), L"1 problem remains");
  EXPECT_EQ(CheckSummary(ExitCode::kProblems, 1, 2), L"2 problems remain; fixed 1");
  EXPECT_EQ(CheckSummary(ExitCode::kProblems, 1, 0), L"a repair did not complete; fixed 1");
  EXPECT_EQ(CheckSummary(ExitCode::kNeedsAdmin, 0, 1),
            L"the machine-wide part needs an administrator");
  EXPECT_EQ(CheckSummary(ExitCode::kNotInstalled, 0, 1), L"not installed");
  EXPECT_STREQ(CheckResultName(ExitCode::kOk), L"healthy");
  EXPECT_STREQ(CheckResultName(ExitCode::kNeedsAdmin), L"needs-admin");
}

// The Explorer test's outcome as the summary lines of --check, --fix and --install log it.
TEST(CheckLog, ExplorerTestFields) {
  Snapshot s;
  s.live = LiveTest{S_OK, {96, 96}, 212};
  EXPECT_EQ(LiveTestLogFields(s), L" live=0x00000000 live-ms=212");
  // The watchdog gave up.
  s.live = LiveTest{HRESULT_FROM_WIN32(ERROR_TIMEOUT), {}, 60000};
  EXPECT_EQ(LiveTestLogFields(s), L" live=0x800705B4 live-ms=60000");

  // Due, but it could not start.
  s.live.reset();
  s.liveError = E_ACCESSDENIED;
  EXPECT_EQ(LiveTestLogFields(s), L" live=could-not-start live-hr=0x80070005");

  s.liveError = S_OK;
  struct Case {
    LiveTestSkip skip;
    const wchar_t* fields;
  };
  for (const Case& c : {
           Case{LiveTestSkip::kNotRequested, L" live=skipped live-skip=not-requested"},
           Case{LiveTestSkip::kElevated, L" live=skipped live-skip=elevated"},
           Case{LiveTestSkip::kVirtualized, L" live=skipped live-skip=virtualized"},
           Case{LiveTestSkip::kNotOurHandler, L" live=skipped live-skip=not-our-handler"},
           Case{LiveTestSkip::kNoClass, L" live=skipped live-skip=no-class"},
       }) {
    s.liveSkip = c.skip;
    EXPECT_EQ(LiveTestLogFields(s), c.fields);
  }
}

}  // namespace
}  // namespace pdft
