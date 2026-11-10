// The pure parts of the elevation boundary (elevation.h): the step names and the one accepted
// command line, the elevated side's entry checks in their order (administrator rights in every
// combination of the token's facts; below an app package, its test writes tested once and last,
// with a fake probe), how an elevated run's exit is read, how the report pipe's bytes become lines,
// which pipe the elevated side writes to, a report channel another process took, and the parts a
// step did not complete: the lines and the end line it writes, and how the asking side reads them
// into its failures (this build's report, an older build's, one cut short, malformed or forged
// lines). Also the installer's records (inno.h) and which holders of a DLL may be ended
// (processes.h).

#include <windows.h>

#include "elevation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "inno.h"
#include "processes.h"
#include "project_config.h"
#include "registry_roots.h"
#include "strings.h"

namespace pdft {
namespace {

std::optional<ElevatedVerb> Parse(std::vector<std::wstring_view> args, bool devInstall = true) {
  return ParseElevatedArguments(args, devInstall);
}

TEST(ElevatedSteps, NamesRoundTrip) {
  for (const ElevatedVerb verb :
       {ElevatedVerb::kMachineInstall, ElevatedVerb::kMachineRepairClass,
        ElevatedVerb::kMachineUninstall, ElevatedVerb::kMachineMigrateV1}) {
    EXPECT_EQ(ElevatedVerbFromName(ElevatedVerbName(verb), true), verb);
  }
  EXPECT_EQ(ElevatedVerbName(ElevatedVerb::kMachineRepairClass), L"machine-repair-class");
  EXPECT_EQ(ElevatedParameters(ElevatedVerb::kMachineUninstall), L"--elevated machine-uninstall");
}

TEST(ElevatedSteps, MachineInstallOnlyInDeveloperBuilds) {
  EXPECT_EQ(ElevatedVerbFromName(L"machine-install", true), ElevatedVerb::kMachineInstall);
  EXPECT_FALSE(ElevatedVerbFromName(L"machine-install", false));
  EXPECT_EQ(ElevatedVerbFromName(L"machine-uninstall", false), ElevatedVerb::kMachineUninstall);
}

TEST(ElevatedSteps, FromTheCommandLineParser) {
  EXPECT_EQ(ElevatedVerbFromStep(ElevatedStep::kMachineInstall), ElevatedVerb::kMachineInstall);
  EXPECT_EQ(ElevatedVerbFromStep(ElevatedStep::kMachineRepairClass),
            ElevatedVerb::kMachineRepairClass);
  EXPECT_EQ(ElevatedVerbFromStep(ElevatedStep::kMachineUninstall), ElevatedVerb::kMachineUninstall);
  EXPECT_EQ(ElevatedVerbFromStep(ElevatedStep::kMachineMigrateV1), ElevatedVerb::kMachineMigrateV1);
}

TEST(ElevatedArguments, ExactlyTheStep) {
  EXPECT_EQ(Parse({L"--elevated", L"machine-repair-class"}), ElevatedVerb::kMachineRepairClass);
  EXPECT_EQ(Parse({L"--elevated", L"machine-migrate-v1"}), ElevatedVerb::kMachineMigrateV1);
  EXPECT_FALSE(Parse({}));
  EXPECT_FALSE(Parse({L"--elevated"}));
  EXPECT_FALSE(Parse({L"--elevated=machine-repair-class"}));
  EXPECT_FALSE(Parse({L"--elevated", L"machine-repair-class", L"--yes"}));
  EXPECT_FALSE(Parse({L"--elevated", L"machine-repair-class", L""}));
  EXPECT_FALSE(Parse({L"--relaunched", L"--elevated", L"machine-repair-class"}));
  EXPECT_FALSE(Parse({L"--Elevated", L"machine-repair-class"}));
  EXPECT_FALSE(Parse({L"--elevated", L"Machine-Repair-Class"}));
  EXPECT_FALSE(Parse({L"--elevated", L"machine-delete-everything"}));
  EXPECT_FALSE(Parse({L"--elevated", L"machine-install"}, false));
}

ElevatedEntryFacts Passing(ElevatedVerb verb = ElevatedVerb::kMachineRepairClass) {
  ElevatedEntryFacts facts;
  facts.tokenElevated = true;
  facts.administratorsEnabled = true;
  facts.integrityRid = SECURITY_MANDATORY_HIGH_RID;
  facts.verb = verb;
  facts.runsFromMachineRoot = true;
  return facts;
}

TEST(ElevatedEntry, AllChecksPass) {
  EXPECT_EQ(CheckElevatedEntry(Passing()), S_OK);
  EXPECT_EQ(FindElevatedRefusal(Passing()), ElevatedRefusal::kNone);
}

TEST(ElevatedEntry, RefusalsInOrder) {
  // Below an app package, its test writes not run yet: the last check refuses.
  ElevatedEntryFacts facts = Passing();
  facts.machineWrites.packageAbove = true;
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kPackaged);
  EXPECT_EQ(CheckElevatedEntry(facts), kBelowPackageHr);
  EXPECT_EQ(CheckElevatedEntry(facts), static_cast<HRESULT>(0x8007109C));
  // Each earlier check wins over it, in turn.
  facts.reparsePoints = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kReparsePoint);
  EXPECT_EQ(CheckElevatedEntry(facts), HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  facts.runsFromMachineRoot = false;
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kNotFromMachineRoot);
  EXPECT_EQ(CheckElevatedEntry(facts), E_ACCESSDENIED);
  facts.verb.reset();
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kNotOneStep);
  EXPECT_EQ(CheckElevatedEntry(facts), E_INVALIDARG);
  facts.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kIntegrityBelowHigh);
  EXPECT_EQ(CheckElevatedEntry(facts), kNoAdministratorRightsHr);
  facts.administratorsEnabled = false;
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kAdministratorsNotEnabled);
  EXPECT_EQ(CheckElevatedEntry(facts), kNoAdministratorRightsHr);
  facts.tokenElevated = false;
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kNotElevated);
  EXPECT_EQ(CheckElevatedEntry(facts), kNoAdministratorRightsHr);
  // The app's own code: no Windows error, so no failed launch or call inside a step gives it.
  EXPECT_TRUE(FAILED(kNoAdministratorRightsHr));
  EXPECT_EQ(HRESULT_FACILITY(kNoAdministratorRightsHr), FACILITY_ITF);
  EXPECT_NE(kNoAdministratorRightsHr, HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED));
}

// Every combination of the three token facts: only an elevated token with the Administrators
// group enabled at High integrity or above may go on, whatever else holds. The token's refusal
// comes before every other one.
TEST(ElevatedEntry, AdministratorRightsInEveryCombination) {
  const DWORD levels[] = {SECURITY_MANDATORY_UNTRUSTED_RID,
                          SECURITY_MANDATORY_LOW_RID,
                          SECURITY_MANDATORY_MEDIUM_RID,
                          SECURITY_MANDATORY_MEDIUM_PLUS_RID,
                          SECURITY_MANDATORY_HIGH_RID,
                          SECURITY_MANDATORY_SYSTEM_RID,
                          SECURITY_MANDATORY_PROTECTED_PROCESS_RID};
  for (const bool elevated : {false, true}) {
    for (const bool administrators : {false, true}) {
      for (const DWORD level : levels) {
        ElevatedEntryFacts facts = Passing();
        facts.tokenElevated = elevated;
        facts.administratorsEnabled = administrators;
        facts.integrityRid = level;
        ElevatedRefusal expected = ElevatedRefusal::kNone;
        if (!elevated) {
          expected = ElevatedRefusal::kNotElevated;
        } else if (!administrators) {
          expected = ElevatedRefusal::kAdministratorsNotEnabled;
        } else if (level < static_cast<DWORD>(SECURITY_MANDATORY_HIGH_RID)) {
          expected = ElevatedRefusal::kIntegrityBelowHigh;
        }
        const std::string label = "elevated " + std::to_string(elevated) + ", administrators " +
                                  std::to_string(administrators) + ", level " +
                                  std::to_string(level);
        EXPECT_EQ(FindElevatedRefusal(facts), expected) << label;
        EXPECT_EQ(CheckElevatedEntry(facts),
                  expected == ElevatedRefusal::kNone ? S_OK : kNoAdministratorRightsHr)
            << label;
        // The steps for all users that run in the installer's own process judge it the same way.
        EXPECT_EQ(HasAdministratorRights(facts), expected == ElevatedRefusal::kNone) << label;

        // Everything else wrong too: the token still answers first.
        facts.machineWrites.packageAbove = true;
        facts.verb.reset();
        facts.runsFromMachineRoot = false;
        facts.reparsePoints = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
        EXPECT_EQ(FindElevatedRefusal(facts),
                  expected == ElevatedRefusal::kNone ? ElevatedRefusal::kNotOneStep : expected)
            << label;
        EXPECT_EQ(HasAdministratorRights(facts), expected == ElevatedRefusal::kNone) << label;
      }
    }
  }
}

// Below an app package (or with the processes above unreadable) a step goes on only when both of
// its test writes ran and came back real; without a package above, nothing about them matters.
TEST(ElevatedEntry, BelowAPackageOnlyWithBothWritesShownReal) {
  const Virtualization verdicts[] = {Virtualization::kNo, Virtualization::kYes,
                                     Virtualization::kUnknown};
  for (const int above : {0, 1, 2}) {  // none, a package, a walk that failed
    for (const bool probed : {false, true}) {
      for (const Virtualization machine : verdicts) {
        for (const Virtualization user : verdicts) {
          ElevatedEntryFacts facts = Passing();
          facts.machineWrites.packageAbove = above == 1;
          facts.machineWrites.walk = above == 2 ? E_ACCESSDENIED : S_OK;
          facts.machineWrites.probed = probed;
          facts.machineWrites.machine = machine;
          facts.machineWrites.user = user;
          const bool shownReal =
              probed && machine == Virtualization::kNo && user == Virtualization::kNo;
          const bool refused = above != 0 && !shownReal;
          const std::string label = "above " + std::to_string(above) + ", probed " +
                                    std::to_string(probed) + ", machine " +
                                    std::to_string(static_cast<int>(machine)) + ", user " +
                                    std::to_string(static_cast<int>(user));
          EXPECT_EQ(BelowPackage(facts.machineWrites), above != 0) << label;
          EXPECT_EQ(RefusesMachineWrites(facts.machineWrites), refused) << label;
          EXPECT_EQ(FindElevatedRefusal(facts),
                    refused ? ElevatedRefusal::kPackaged : ElevatedRefusal::kNone)
              << label;
          EXPECT_EQ(CheckElevatedEntry(facts), refused ? kBelowPackageHr : S_OK) << label;
        }
      }
    }
  }
}

// A fake probe: answers |machine| and |user|, and counts its runs in |*runs|.
std::function<void(MachineWriteCheck*)> CountingProbe(int* runs, Virtualization machine,
                                                      Virtualization user) {
  return [runs, machine, user](MachineWriteCheck* check) {
    ++*runs;
    check->probed = true;
    check->machine = machine;
    check->user = user;
  };
}

// The test writes run once, only below a package (or where the walk failed), and only for a step
// that passed every other check; their answer decides.
TEST(ElevatedEntry, TheWritesAreTestedOnceAndLast) {
  constexpr Virtualization kNo = Virtualization::kNo;
  int runs = 0;
  ElevatedEntryFacts facts = Passing();
  facts.machineWrites.packageAbove = true;
  EXPECT_EQ(DecideElevatedEntry(&facts, CountingProbe(&runs, kNo, kNo)), ElevatedRefusal::kNone);
  EXPECT_EQ(runs, 1);
  EXPECT_TRUE(facts.machineWrites.probed);
  EXPECT_EQ(CheckElevatedEntry(facts), S_OK);
  // Probed already: not again.
  EXPECT_EQ(DecideElevatedEntry(&facts, CountingProbe(&runs, kNo, kNo)), ElevatedRefusal::kNone);
  EXPECT_EQ(runs, 1);

  // A walk that failed, and the developer install's exemption from the place, are tested too.
  runs = 0;
  facts = Passing();
  facts.machineWrites.walk = E_ACCESSDENIED;
  EXPECT_EQ(DecideElevatedEntry(&facts, CountingProbe(&runs, kNo, kNo)), ElevatedRefusal::kNone);
  EXPECT_EQ(runs, 1);
  facts = Passing(ElevatedVerb::kMachineInstall);
  facts.runsFromMachineRoot = false;
  facts.devInstall = true;
  facts.machineWrites.packageAbove = true;
  EXPECT_EQ(DecideElevatedEntry(&facts, CountingProbe(&runs, kNo, kNo)), ElevatedRefusal::kNone);
  EXPECT_EQ(runs, 2);

  // An answer other than real, for either half, refuses.
  const std::pair<Virtualization, Virtualization> answers[] = {
      {Virtualization::kYes, kNo},
      {kNo, Virtualization::kYes},
      {Virtualization::kUnknown, kNo},
      {kNo, Virtualization::kUnknown},
  };
  for (const auto& [machine, user] : answers) {
    runs = 0;
    facts = Passing();
    facts.machineWrites.packageAbove = true;
    EXPECT_EQ(DecideElevatedEntry(&facts, CountingProbe(&runs, machine, user)),
              ElevatedRefusal::kPackaged);
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(CheckElevatedEntry(facts), kBelowPackageHr);
  }

  // No package above: never.
  runs = 0;
  facts = Passing();
  EXPECT_EQ(DecideElevatedEntry(&facts, CountingProbe(&runs, kNo, kNo)), ElevatedRefusal::kNone);
  EXPECT_EQ(runs, 0);
  EXPECT_FALSE(facts.machineWrites.probed);

  // Another refusal applies: never, whatever is above.
  const std::vector<std::function<void(ElevatedEntryFacts*)>> refusals = {
      [](ElevatedEntryFacts* f) { f->tokenElevated = false; },
      [](ElevatedEntryFacts* f) { f->administratorsEnabled = false; },
      [](ElevatedEntryFacts* f) { f->integrityRid = SECURITY_MANDATORY_MEDIUM_RID; },
      [](ElevatedEntryFacts* f) { f->verb.reset(); },
      [](ElevatedEntryFacts* f) { f->runsFromMachineRoot = false; },
      [](ElevatedEntryFacts* f) {
        f->reparsePoints = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
      },
  };
  for (size_t i = 0; i < refusals.size(); ++i) {
    facts = Passing();
    facts.machineWrites.packageAbove = true;
    refusals[i](&facts);
    const ElevatedRefusal refusal = DecideElevatedEntry(&facts, CountingProbe(&runs, kNo, kNo));
    EXPECT_NE(refusal, ElevatedRefusal::kNone) << i;
    EXPECT_NE(refusal, ElevatedRefusal::kPackaged) << i;
    EXPECT_EQ(runs, 0) << i;
    EXPECT_FALSE(facts.machineWrites.probed) << i;
  }
  // Without a probe, a step below a package is refused.
  facts = Passing();
  facts.machineWrites.packageAbove = true;
  EXPECT_EQ(DecideElevatedEntry(&facts, {}), ElevatedRefusal::kPackaged);
}

// The exemption of machine-install in developer builds is from the place only: never from the
// token checks.
TEST(ElevatedEntry, TheDeveloperInstallNeedsAdministratorRightsToo) {
  ElevatedEntryFacts facts = Passing(ElevatedVerb::kMachineInstall);
  facts.runsFromMachineRoot = false;
  facts.devInstall = true;
  ASSERT_EQ(CheckElevatedEntry(facts), S_OK);
  facts.administratorsEnabled = false;
  EXPECT_EQ(CheckElevatedEntry(facts), kNoAdministratorRightsHr);
  facts.administratorsEnabled = true;
  facts.integrityRid = SECURITY_MANDATORY_MEDIUM_RID;
  EXPECT_EQ(CheckElevatedEntry(facts), kNoAdministratorRightsHr);
}

// What the refusal line says: each refusal in words of its own, never a path; the three of the
// token say that the step has no administrator rights.
TEST(ElevatedEntry, RefusalTexts) {
  EXPECT_TRUE(ElevatedRefusalText(ElevatedRefusal::kNone).empty());
  const ElevatedRefusal refusals[] = {
      ElevatedRefusal::kNotElevated,        ElevatedRefusal::kAdministratorsNotEnabled,
      ElevatedRefusal::kIntegrityBelowHigh, ElevatedRefusal::kPackaged,
      ElevatedRefusal::kNotOneStep,         ElevatedRefusal::kNotFromMachineRoot,
      ElevatedRefusal::kReparsePoint};
  std::vector<std::wstring> seen;
  for (const ElevatedRefusal refusal : refusals) {
    const std::wstring text(ElevatedRefusalText(refusal));
    EXPECT_FALSE(text.empty()) << static_cast<int>(refusal);
    EXPECT_EQ(text.find_first_of(L"\\/:"), std::wstring::npos) << WideToUtf8Lossy(text);
    EXPECT_EQ(std::find(seen.begin(), seen.end(), text), seen.end()) << WideToUtf8Lossy(text);
    seen.push_back(text);
  }
  for (const ElevatedRefusal refusal :
       {ElevatedRefusal::kNotElevated, ElevatedRefusal::kAdministratorsNotEnabled,
        ElevatedRefusal::kIntegrityBelowHigh}) {
    EXPECT_EQ(ElevatedRefusalText(refusal).rfind(L"no administrator rights (", 0), 0u)
        << static_cast<int>(refusal);
  }
  EXPECT_NE(ElevatedRefusalText(ElevatedRefusal::kAdministratorsNotEnabled).find(L"Administrators"),
            std::wstring_view::npos);
  EXPECT_NE(ElevatedRefusalText(ElevatedRefusal::kIntegrityBelowHigh).find(L"High"),
            std::wstring_view::npos);

  // Below a package: what the step found, in brackets after the refusal; nothing for the others.
  ElevatedEntryFacts facts = Passing();
  facts.machineWrites.packageAbove = true;
  facts.machineWrites.probed = true;
  facts.machineWrites.machine = Virtualization::kUnknown;
  facts.machineWrites.user = Virtualization::kNo;
  facts.machineWrites.probeError = static_cast<HRESULT>(0x80041003);
  EXPECT_EQ(ElevatedRefusalReason(ElevatedRefusal::kPackaged, facts),
            L"inside or below an app package, or with the processes above it unreadable, and its "
            L"test writes did not come back (app package above; HKLM unknown, HKCU real, "
            L"probe-hr=0x80041003)");
  EXPECT_EQ(ElevatedRefusalReason(ElevatedRefusal::kNotOneStep, facts),
            ElevatedRefusalText(ElevatedRefusal::kNotOneStep));
  facts.machineWrites.machine = Virtualization::kNo;
  facts.machineWrites.probeError = S_OK;
  EXPECT_EQ(ElevatedWritesCheckedLine(L"machine-install", facts.machineWrites),
            L"elevated machine-install writes checked: app package above; HKLM real, HKCU real");
}

TEST(ElevatedEntry, OnlyMachineInstallInDeveloperBuildsMayRunElsewhere) {
  ElevatedEntryFacts facts = Passing(ElevatedVerb::kMachineInstall);
  facts.runsFromMachineRoot = false;
  facts.devInstall = true;
  EXPECT_EQ(CheckElevatedEntry(facts), S_OK);
  facts.devInstall = false;
  EXPECT_EQ(CheckElevatedEntry(facts), E_ACCESSDENIED);
  for (const ElevatedVerb verb :
       {ElevatedVerb::kMachineRepairClass, ElevatedVerb::kMachineUninstall,
        ElevatedVerb::kMachineMigrateV1}) {
    facts = Passing(verb);
    facts.runsFromMachineRoot = false;
    facts.devInstall = true;
    EXPECT_EQ(CheckElevatedEntry(facts), E_ACCESSDENIED);
  }
  // The exemption is from the place only, never from the reparse point check.
  facts = Passing(ElevatedVerb::kMachineInstall);
  facts.runsFromMachineRoot = false;
  facts.devInstall = true;
  facts.reparsePoints = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  EXPECT_EQ(CheckElevatedEntry(facts), HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
}

// The install for all users' own executable: run from the machine root, with no link on the way.
// The settings window starts again for another account only from there.
TEST(MachineInstall, TheMachineRootWithNoLinkOnTheWay) {
  EXPECT_TRUE(IsMachineInstall(MachineRootCheck{true, S_OK}));
  EXPECT_FALSE(IsMachineInstall(MachineRootCheck{false, S_OK}));
  EXPECT_FALSE(
      IsMachineInstall(MachineRootCheck{true, HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK)}));
  EXPECT_FALSE(IsMachineInstall(MachineRootCheck{true, E_UNEXPECTED}));  // no machine root
  EXPECT_FALSE(IsMachineInstall(MachineRootCheck{}));
}

TEST(ElevatedExit, HowARunEnded) {
  HRESULT failure = E_FAIL;
  EXPECT_EQ(ClassifyElevatedExit(S_OK, 0, &failure), ElevatedOutcome::kDone);
  EXPECT_EQ(failure, S_OK);
  EXPECT_EQ(ClassifyElevatedExit(S_OK, 1, &failure), ElevatedOutcome::kRestartPending);
  EXPECT_EQ(ClassifyElevatedExit(HRESULT_FROM_WIN32(ERROR_CANCELLED), 0, &failure),
            ElevatedOutcome::kCancelled);
  EXPECT_EQ(failure, S_OK);
  EXPECT_EQ(ClassifyElevatedExit(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), 0, &failure),
            ElevatedOutcome::kFailed);
  EXPECT_EQ(failure, HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
  EXPECT_EQ(ClassifyElevatedExit(S_OK, 0x80070020, &failure), ElevatedOutcome::kFailed);
  EXPECT_EQ(failure, HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));
  // Neither 0, 1 nor an HRESULT (a process ended from outside, say).
  EXPECT_EQ(ClassifyElevatedExit(S_OK, 2, &failure), ElevatedOutcome::kFailed);
  EXPECT_EQ(failure, E_UNEXPECTED);
}

// A step that refused for want of administrator rights, told apart from a declined prompt and
// from every other failure.
TEST(ElevatedExit, WithoutAdministratorRights) {
  ElevatedRun run;
  run.outcome =
      ClassifyElevatedExit(S_OK, static_cast<DWORD>(kNoAdministratorRightsHr), &run.failure);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kFailed);
  EXPECT_TRUE(LackedAdministratorRights(run));
  run.failure = E_ACCESSDENIED;
  EXPECT_FALSE(LackedAdministratorRights(run));
  run.failure = kNoAdministratorRightsHr;
  for (const ElevatedOutcome outcome :
       {ElevatedOutcome::kDone, ElevatedOutcome::kRestartPending, ElevatedOutcome::kCancelled}) {
    run.outcome = outcome;
    EXPECT_FALSE(LackedAdministratorRights(run)) << static_cast<int>(outcome);
  }
  run = ElevatedRun{};
  run.outcome = ClassifyElevatedExit(HRESULT_FROM_WIN32(ERROR_CANCELLED), 0, &run.failure);
  EXPECT_FALSE(LackedAdministratorRights(run));
  // Only the elevated process's own exit code says so, never a launch that failed.
  run = ElevatedRun{};
  run.launched = kNoAdministratorRightsHr;
  run.outcome = ClassifyElevatedExit(run.launched, 0, &run.failure);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kFailed);
  EXPECT_EQ(run.failure, kNoAdministratorRightsHr);
  EXPECT_FALSE(LackedAdministratorRights(run));
}

// A step that refused to run below an app package, told apart from every other failure: such a
// step never ran, so it names no parts and needs no failure of its own (its exit code is 8).
TEST(ElevatedExit, RefusedBelowPackage) {
  // The code earlier builds refuse with below any package, so both builds' parents read it.
  EXPECT_EQ(kBelowPackageHr, static_cast<HRESULT>(0x8007109C));
  EXPECT_EQ(kBelowPackageHr, HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED_IN_APPCONTAINER));
  ElevatedRun run;
  run.outcome = ClassifyElevatedExit(S_OK, static_cast<DWORD>(kBelowPackageHr), &run.failure);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kFailed);
  EXPECT_TRUE(RefusedBelowPackage(run));
  EXPECT_FALSE(LackedAdministratorRights(run));
  run.report = {L"not done: class: 0x8007109C", L"elevated machine-uninstall end hr=0x8007109C"};
  run.parts = ReadReportParts(ElevatedVerb::kMachineUninstall, run.report);
  EXPECT_TRUE(NotDoneFailures(run, L"machine").empty());
  EXPECT_FALSE(NeedsStepFailure(run));
  EXPECT_EQ(CountPartsNotDone(run), 0u);
  run.failure = E_ACCESSDENIED;
  EXPECT_FALSE(RefusedBelowPackage(run));
  run.failure = kBelowPackageHr;
  for (const ElevatedOutcome outcome :
       {ElevatedOutcome::kDone, ElevatedOutcome::kRestartPending, ElevatedOutcome::kCancelled}) {
    run.outcome = outcome;
    EXPECT_FALSE(RefusedBelowPackage(run)) << static_cast<int>(outcome);
  }
  // Only the elevated process's own exit code says so, never a launch that failed: that one keeps
  // its failure.
  run = ElevatedRun{};
  run.launched = kBelowPackageHr;
  run.outcome = ClassifyElevatedExit(run.launched, 0, &run.failure);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kFailed);
  EXPECT_EQ(run.failure, kBelowPackageHr);
  EXPECT_FALSE(RefusedBelowPackage(run));
  EXPECT_TRUE(NeedsStepFailure(run));
}

TEST(ElevatedReport, PipeName) {
  EXPECT_EQ(ElevationPipeName(4242),
            std::wstring(L"\\\\.\\pipe\\") + kObjectPrefix + L".Elevated.4242");
}

TEST(ElevatedReport, BytesToLines) {
  EXPECT_TRUE(SplitReportLines("").empty());
  EXPECT_EQ(SplitReportLines("one\ntwo\r\nthree"),
            (std::vector<std::wstring>{L"one", L"two", L"three"}));
  EXPECT_EQ(SplitReportLines("a\n\nb\n"), (std::vector<std::wstring>{L"a", L"", L"b"}));
  // UTF-8, malformed bytes, control characters.
  EXPECT_EQ(SplitReportLines("caf\xC3\xA9\n"), (std::vector<std::wstring>{L"caf\u00E9"}));
  EXPECT_EQ(SplitReportLines("x\xFFy\n"), (std::vector<std::wstring>{L"x\uFFFDy"}));
  EXPECT_EQ(SplitReportLines("a\tb\x1B[31mc\n"), (std::vector<std::wstring>{L"a b [31mc"}));
}

TEST(ElevatedReport, Bounded) {
  const std::string longLine(kMaxReportLineChars + 50, 'x');
  const std::vector<std::wstring> cut = SplitReportLines(longLine + "\n");
  ASSERT_EQ(cut.size(), 1u);
  EXPECT_EQ(cut[0].size(), kMaxReportLineChars);
  std::string many;
  for (size_t i = 0; i < kMaxReportLines + 10; ++i) many += "l\n";
  EXPECT_EQ(SplitReportLines(many).size(), kMaxReportLines);
}

ReportServerFacts ParentsPipe() {
  ReportServerFacts facts;
  facts.parentId = 4242;
  facts.parentSession = 1;
  facts.serverId = 4242;
  facts.serverSession = 1;
  facts.ownerIsParentsUser = true;
  return facts;
}

// The elevated side writes only to a pipe its parent made, in the parent's session, owned by the
// parent's user: each of the three alone is not enough.
TEST(ElevatedReport, OnlyThePipeOfTheParent) {
  EXPECT_TRUE(TrustReportServer(ParentsPipe()));

  ReportServerFacts facts = ParentsPipe();
  facts.serverId = 4243;
  EXPECT_FALSE(TrustReportServer(facts));
  facts = ParentsPipe();
  facts.serverSession = 2;  // another signed-in user's process that once had the parent's id
  EXPECT_FALSE(TrustReportServer(facts));
  facts = ParentsPipe();
  facts.ownerIsParentsUser = false;
  EXPECT_FALSE(TrustReportServer(facts));
  facts = ParentsPipe();
  facts.parentSession = 0;
  EXPECT_FALSE(TrustReportServer(facts));

  // Without a parent nothing is its pipe, even a server whose id reads as 0.
  facts = ParentsPipe();
  facts.parentId = 0;
  facts.serverId = 0;
  EXPECT_FALSE(TrustReportServer(facts));
}

// A pipe of the report's name that another process made first: the step runs without a report,
// and the run says why, apart from every other reason there was no pipe.
TEST(ElevatedReport, AChannelTakenByAnotherProcess) {
  ElevatedRun run;
  EXPECT_FALSE(ReportChannelTaken(run));
  run.reportServer = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  EXPECT_TRUE(ReportChannelTaken(run));
  run.reportServer = HRESULT_FROM_WIN32(ERROR_PIPE_BUSY);
  EXPECT_TRUE(ReportChannelTaken(run));
  for (const HRESULT other :
       {E_OUTOFMEMORY, HRESULT_FROM_WIN32(ERROR_INVALID_OWNER), E_FAIL, S_FALSE}) {
    run.reportServer = other;
    EXPECT_FALSE(ReportChannelTaken(run)) << std::hex << static_cast<unsigned long>(other);
  }
}

// ---- the parts a step did not complete ----

// A run of |verb| that was launched, ended with |exitCode| and sent |report|, read the way
// RunElevated reads it.
ElevatedRun RunWithReport(ElevatedVerb verb, DWORD exitCode, std::vector<std::wstring> report) {
  ElevatedRun run;
  run.exitCode = exitCode;
  run.outcome = ClassifyElevatedExit(S_OK, exitCode, &run.failure);
  run.reportChannel = !report.empty();
  run.report = std::move(report);
  run.parts = ReadReportParts(verb, run.report);
  return run;
}

// A run whose launch failed with |launched| (ERROR_CANCELLED: a declined prompt), although lines
// came back.
ElevatedRun NotLaunched(HRESULT launched, std::vector<std::wstring> report) {
  ElevatedRun run;
  run.launched = launched;
  run.outcome = ClassifyElevatedExit(launched, 0, &run.failure);
  run.report = std::move(report);
  run.parts = ReadReportParts(ElevatedVerb::kMachineUninstall, run.report);
  return run;
}

constexpr ElevatedVerb kInstall = ElevatedVerb::kMachineInstall;
constexpr ElevatedVerb kUninstall = ElevatedVerb::kMachineUninstall;

TEST(ElevatedReport, EndAndNotDoneLines) {
  EXPECT_EQ(ElevatedEndLine(kInstall, S_OK, false, 0),
            L"elevated machine-install end hr=0x00000000 not-done=0");
  EXPECT_EQ(ElevatedEndLine(kUninstall, E_ACCESSDENIED, true, 2),
            L"elevated machine-uninstall end hr=0x80070005 restart-pending not-done=2");
  EXPECT_EQ(NotDoneLine(L"task-folder: 0x80070005"), L"not done: task-folder: 0x80070005");
}

// The lines a step of this build writes, from failures built the way the steps list them, read
// back in order from between its other lines.
TEST(ElevatedReport, ReadsPartsOfThisBuild) {
  EXPECT_EQ(HResultText(kRegistryTreeLeftHr), L"0x800402F0");
  const std::vector<std::wstring> failures = {
      L"task-folder: " + HResultText(E_ACCESSDENIED),
      L"restart-delete: " + HResultText(HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)),
      L"hive left: " + HResultText(kRegistryTreeLeftHr)};
  std::vector<std::wstring> lines = {
      L"elevated machine-install start (token: elevated yes, elevation type 2 (full), integrity "
      L"0x3000, Administrators enabled)",
      L"copied app\\2.0.0.0-1a2b3c4d (4 files written)"};
  for (const std::wstring& failure : failures) lines.push_back(NotDoneLine(failure));
  lines.push_back(ElevatedEndLine(kInstall, S_OK, true, failures.size()));
  const ReportParts parts = ReadReportParts(kInstall, lines);
  EXPECT_TRUE(parts.ended);
  EXPECT_EQ(parts.counted, 3u);
  ASSERT_EQ(parts.named.size(), 3u);
  EXPECT_EQ(parts.named[0].part, L"task-folder");
  EXPECT_EQ(parts.named[0].hr, E_ACCESSDENIED);
  EXPECT_EQ(parts.named[1].part, L"restart-delete");
  EXPECT_EQ(parts.named[1].hr, HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));
  EXPECT_EQ(parts.named[2].part, L"hive left");
  EXPECT_EQ(parts.named[2].hr, kRegistryTreeLeftHr);

  // Hex digits in either case; the command's failures write them as HResultText does.
  const ElevatedRun run = RunWithReport(kInstall, 0,
                                        {L"not done: old-builds: 0x8007000e",
                                         L"elevated machine-install end hr=0x0000000a not-done=1"});
  EXPECT_TRUE(run.parts.ended);
  EXPECT_EQ(NotDoneFailures(run, L"machine-install"),
            std::vector<std::wstring>{L"machine-install old-builds: 0x8007000E"});
}

// The reports of the build before parts were reported: ended, no count, nothing named, so the
// exit code alone tells how the step ended, as it did.
TEST(ElevatedReport, AReportOfAnOlderBuild) {
  ElevatedRun run =
      RunWithReport(kUninstall, static_cast<DWORD>(E_ACCESSDENIED),
                    {L"elevated machine-uninstall start (token: elevated yes, elevation type 2 "
                     L"(full), integrity 0x3000, Administrators enabled)",
                     L"class for all users removed", L"failed class: 0x80070005",
                     L"elevated machine-uninstall end hr=0x80070005"});
  EXPECT_TRUE(run.parts.ended);
  EXPECT_FALSE(run.parts.counted);
  EXPECT_TRUE(run.parts.named.empty());
  EXPECT_TRUE(NeedsStepFailure(run));
  EXPECT_TRUE(NotDoneFailures(run, L"machine").empty());

  run = RunWithReport(kInstall, 1,
                      {L"class for all users -> app\\2.0.0.0-1a2b3c4d",
                       L"elevated machine-install end hr=0x00000000 restart-pending"});
  EXPECT_EQ(run.outcome, ElevatedOutcome::kRestartPending);
  EXPECT_TRUE(run.parts.ended);
  EXPECT_FALSE(run.parts.counted);
  EXPECT_FALSE(ReportIncomplete(run));
  EXPECT_FALSE(PartsNotDone(run));
}

// Only well-formed lines name a part, and only the step's own end line, as the last line, ends
// its report.
TEST(ElevatedReport, OnlyWellFormedPartsAreNamed) {
  const auto named = [](const std::wstring& line) {
    return ReadReportParts(kInstall, std::vector<std::wstring>{line}).named.size();
  };
  EXPECT_EQ(named(L"not done: task-folder: 0x80070005"), 1u);
  EXPECT_EQ(named(L"not done: hive left: 0x800402F0"), 1u);
  EXPECT_EQ(named(L"not done: " + std::wstring(64, L'a') + L": 0x80070005"), 1u);
  EXPECT_EQ(named(L"not done: " + std::wstring(65, L'a') + L": 0x80070005"), 0u);
  for (const wchar_t* line : {
           L"not done: Task-folder: 0x80070005",   // an uppercase letter
           L"not done: task\\folder: 0x80070005",  // a path's character
           L"not done: task.folder: 0x80070005",
           L"not done:  task-folder: 0x80070005",  // a space first
           L"not done: task-folder : 0x80070005",  // a space last
           L"not done: : 0x80070005",              // no part
           L"not done: a: b: 0x80070005",          // split at the last ": "
           L"not done: task-folder 0x80070005",    // no ": "
           L"not done: task-folder: 0x800700050",  // 9 digits
           L"not done: task-folder: 0x8007005",    // 7 digits
           L"not done: task-folder: 80070005",     // no 0x
           L"not done: task-folder: 0X80070005",
           L"not done: task-folder: 0x8007000g",
           L"not done: task-folder: 0x00000000",  // a success
           L"not done: task-folder: 0x80070005 ",
           L"not done:task-folder: 0x80070005",  // the prefix without its space
           L"Not done: task-folder: 0x80070005",
           L"failed task-folder: 0x80070005",
       }) {
    EXPECT_EQ(named(line), 0u) << WideToUtf8Lossy(line);
  }

  const std::wstring end = L"elevated machine-install end hr=0x00000000";
  const auto read = [](const std::vector<std::wstring>& lines) {
    return ReadReportParts(kInstall, lines);
  };
  EXPECT_FALSE(read({}).ended);
  // A line after the end line: the report did not end there, and what came is still read.
  ReportParts parts = read({L"not done: old-builds: 0x80070057", end + L" not-done=1", L"more"});
  EXPECT_FALSE(parts.ended);
  EXPECT_FALSE(parts.counted);
  EXPECT_EQ(parts.named.size(), 1u);
  // An end line after the last one is not the step's own either.
  parts = read({end + L" not-done=0", L"not done: old-builds: 0x80070057"});
  EXPECT_FALSE(parts.ended);
  EXPECT_EQ(parts.named.size(), 1u);
  // Another step's end line.
  EXPECT_FALSE(read({L"elevated machine-uninstall end hr=0x00000000 not-done=0"}).ended);
  // The HRESULT exactly, then words, each after a space.
  EXPECT_FALSE(read({L"elevated machine-install end hr=0x0000000"}).ended);
  EXPECT_FALSE(read({L"elevated machine-install end hr=0x000000000"}).ended);
  EXPECT_FALSE(read({end + L"restart-pending"}).ended);
  EXPECT_FALSE(read({L"elevated machine-install end hr=0X00000000"}).ended);
  EXPECT_TRUE(read({L"elevated machine-install end hr=0x8007000e not-done=0"}).ended);
  // Words this build does not know are skipped; a count it cannot read is none.
  parts = read({end + L" future=1 not-done=2"});
  EXPECT_TRUE(parts.ended);
  EXPECT_EQ(parts.counted, 2u);
  for (const wchar_t* word : {L" not-done=x", L" not-done=", L" not-done=1234567890",
                              L" not-done=-1", L" not-done=+1", L" Not-done=1"}) {
    parts = read({end + word});
    EXPECT_TRUE(parts.ended) << WideToUtf8Lossy(word);
    EXPECT_FALSE(parts.counted) << WideToUtf8Lossy(word);
  }
  // A count past what a report can hold names no more parts than that.
  parts = read({end + L" not-done=999999999"});
  EXPECT_EQ(parts.counted, kMaxReportLines);
}

// What the command adds to its failures for a step's parts, and whether it still needs the step's
// own entry.
TEST(ElevatedReport, FailuresForTheCommand) {
  ElevatedRun run = RunWithReport(
      kInstall, 0,
      {NotDoneLine(L"task-folder: 0x80070005"), NotDoneLine(L"old-builds: 0x80070057"),
       ElevatedEndLine(kInstall, S_OK, false, 2)});
  EXPECT_EQ(NotDoneFailures(run, L"machine-install"),
            (std::vector<std::wstring>{L"machine-install task-folder: 0x80070005",
                                       L"machine-install old-builds: 0x80070057"}));
  EXPECT_EQ(CountPartsNotDone(run), 2u);
  EXPECT_TRUE(PartsNotDone(run));
  EXPECT_FALSE(NeedsStepFailure(run));

  // Counted, but not all named: the rest in one entry.
  run = RunWithReport(
      kInstall, 0,
      {NotDoneLine(L"task-folder: 0x80070005"), NotDoneLine(L"A Later Part: 0x80070005"),
       ElevatedEndLine(kInstall, S_OK, false, 3)});
  EXPECT_EQ(CountPartsNotDone(run), 3u);
  EXPECT_EQ(
      NotDoneFailures(run, L"machine-install"),
      (std::vector<std::wstring>{L"machine-install task-folder: 0x80070005",
                                 L"machine-install unnamed parts: 2 (see the step's report)"}));

  run = RunWithReport(kInstall, 0, {ElevatedEndLine(kInstall, S_OK, false, 0)});
  EXPECT_FALSE(PartsNotDone(run));
  EXPECT_TRUE(NotDoneFailures(run, L"machine-install").empty());
  EXPECT_FALSE(NeedsStepFailure(run));

  // A failed step that ended with its first part's HRESULT: the parts stand for it.
  const std::vector<std::wstring> parts = {NotDoneLine(L"class: 0x80070005"),
                                           NotDoneLine(L"tasks: 0x80070057")};
  std::vector<std::wstring> lines = parts;
  lines.push_back(ElevatedEndLine(kUninstall, E_ACCESSDENIED, false, 2));
  run = RunWithReport(kUninstall, static_cast<DWORD>(E_ACCESSDENIED), lines);
  EXPECT_FALSE(NeedsStepFailure(run));
  EXPECT_EQ(
      NotDoneFailures(run, L"machine"),
      (std::vector<std::wstring>{L"machine class: 0x80070005", L"machine tasks: 0x80070057"}));
  // Another HRESULT than its first part's, a later part's included: the step's own entry stays.
  for (const HRESULT other : {E_FAIL, E_INVALIDARG}) {
    run = RunWithReport(kUninstall, static_cast<DWORD>(other), parts);
    EXPECT_TRUE(NeedsStepFailure(run)) << std::hex << static_cast<unsigned long>(other);
  }
  // No parts.
  run = RunWithReport(kUninstall, static_cast<DWORD>(E_ACCESSDENIED),
                      {ElevatedEndLine(kUninstall, E_ACCESSDENIED, false, 0)});
  EXPECT_TRUE(NeedsStepFailure(run));
  EXPECT_TRUE(NotDoneFailures(run, L"machine").empty());

  // A step that never ran has no parts, whatever came back: a launch that failed keeps the
  // step's entry; a declined prompt and a step without administrator rights have exit codes of
  // their own.
  run = NotLaunched(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), parts);
  EXPECT_TRUE(NeedsStepFailure(run));
  EXPECT_TRUE(NotDoneFailures(run, L"machine").empty());
  EXPECT_EQ(CountPartsNotDone(run), 0u);
  run = NotLaunched(HRESULT_FROM_WIN32(ERROR_CANCELLED), parts);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kCancelled);
  EXPECT_FALSE(NeedsStepFailure(run));
  EXPECT_TRUE(NotDoneFailures(run, L"machine").empty());
  run = RunWithReport(kUninstall, static_cast<DWORD>(kNoAdministratorRightsHr), parts);
  EXPECT_TRUE(LackedAdministratorRights(run));
  EXPECT_FALSE(NeedsStepFailure(run));
  EXPECT_TRUE(NotDoneFailures(run, L"machine").empty());
  EXPECT_FALSE(PartsNotDone(run));
}

// A failed step always keeps at least one failure, whatever its report says, forged lines
// included: the report can add failures, never take one away.
TEST(ElevatedReport, AFailedStepAlwaysKeepsAFailure) {
  const std::vector<std::vector<std::wstring>> reports = {
      {},
      {L"elevated machine-uninstall end hr=0x80070005"},
      {L"elevated machine-uninstall end hr=0x80070005 not-done=0"},
      {NotDoneLine(L"class: 0x80070005")},
      {NotDoneLine(L"class: 0x80070005"), ElevatedEndLine(kUninstall, E_ACCESSDENIED, false, 1)},
      {NotDoneLine(L"class: 0x80004005"), ElevatedEndLine(kUninstall, E_FAIL, false, 0)},
      {NotDoneLine(L"Not A Part: 0x80070005"),
       ElevatedEndLine(kUninstall, E_ACCESSDENIED, false, 1)},
      {NotDoneLine(L"hive left: 0x800402F0"), NotDoneLine(L"class: 0x80070005")},
      {NotDoneLine(L"class: 0x00000000"),
       L"elevated machine-uninstall end hr=0x00000000 not-done=0"},
  };
  // 2: neither 0, 1 nor an HRESULT (E_UNEXPECTED).
  const DWORD exits[] = {static_cast<DWORD>(E_ACCESSDENIED), static_cast<DWORD>(E_FAIL),
                         static_cast<DWORD>(kRegistryTreeLeftHr), 2};
  for (const DWORD code : exits) {
    for (size_t i = 0; i < reports.size(); ++i) {
      const ElevatedRun run = RunWithReport(kUninstall, code, reports[i]);
      ASSERT_EQ(run.outcome, ElevatedOutcome::kFailed);
      EXPECT_TRUE(NeedsStepFailure(run) || !NotDoneFailures(run, L"machine").empty())
          << "exit " << std::hex << code << ", report " << std::dec << i;
    }
  }
  for (const std::vector<std::wstring>& report : reports) {
    EXPECT_TRUE(NeedsStepFailure(NotLaunched(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), report)));
  }
}

// A step that is done, but whose report did not come back whole: none came back, or its end line
// is not the last line.
TEST(ElevatedReport, ADoneStepWithoutItsWholeReport) {
  EXPECT_TRUE(ReportIncomplete(RunWithReport(kInstall, 0, {})));
  EXPECT_TRUE(ReportIncomplete(
      RunWithReport(kInstall, 1, {L"elevated machine-install start (token: elevated yes)"})));
  EXPECT_FALSE(
      ReportIncomplete(RunWithReport(kInstall, 0, {ElevatedEndLine(kInstall, S_OK, false, 0)})));
  EXPECT_FALSE(ReportIncomplete(
      RunWithReport(kInstall, 1, {L"elevated machine-install end hr=0x00000000 restart-pending"})));
  EXPECT_FALSE(ReportIncomplete(RunWithReport(kInstall, static_cast<DWORD>(E_ACCESSDENIED), {})));
  EXPECT_FALSE(ReportIncomplete(NotLaunched(HRESULT_FROM_WIN32(ERROR_CANCELLED), {})));
  EXPECT_FALSE(ReportIncomplete(NotLaunched(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), {})));
}

TEST(ElevatedReport, Summary) {
  ElevatedRun run = RunWithReport(kInstall, 0, {ElevatedEndLine(kInstall, S_OK, false, 0)});
  EXPECT_EQ(ElevatedRunSummary(kInstall, run),
            L"machine-install launch=0x00000000 exit=0x00000000");
  run = RunWithReport(
      kInstall, 1,
      {NotDoneLine(L"task-folder: 0x80070005"), NotDoneLine(L"old-builds: 0x80070057"),
       ElevatedEndLine(kInstall, S_OK, true, 2)});
  EXPECT_EQ(ElevatedRunSummary(kInstall, run),
            L"machine-install launch=0x00000000 exit=0x00000001 not-done=2");
  run = RunWithReport(kInstall, 0, {L"elevated machine-install start (token: elevated yes)"});
  EXPECT_EQ(ElevatedRunSummary(kInstall, run),
            L"machine-install launch=0x00000000 exit=0x00000000 report-incomplete");
  // Nothing came back: that alone is said.
  run = RunWithReport(kInstall, 0, {});
  EXPECT_EQ(ElevatedRunSummary(kInstall, run),
            L"machine-install launch=0x00000000 exit=0x00000000 no-report");
  run.reportServer = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  EXPECT_EQ(ElevatedRunSummary(kInstall, run),
            L"machine-install launch=0x00000000 exit=0x00000000 report-channel=0x80070005 (taken "
            L"by another process) no-report");
  run.reportServer = E_OUTOFMEMORY;
  EXPECT_EQ(ElevatedRunSummary(kInstall, run),
            L"machine-install launch=0x00000000 exit=0x00000000 report-channel=0x8007000E "
            L"no-report");
  run = NotLaunched(HRESULT_FROM_WIN32(ERROR_CANCELLED), {});
  EXPECT_EQ(ElevatedRunSummary(kUninstall, run),
            L"machine-uninstall launch=0x800704C7 exit=0x00000000 no-report");
}

// ---- the installer's records ----

TEST(Inno, UninstallKey) {
  EXPECT_EQ(InnoUninstallKey(),
            std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") +
                kInnoAppId + L"_is1");
}

TEST(Inno, UninstallString) {
  std::filesystem::path exe;
  ASSERT_EQ(ParseInnoUninstallString(L"\"C:\\Program Files\\App\\unins000.exe\"", &exe), S_OK);
  EXPECT_EQ(exe, L"C:\\Program Files\\App\\unins000.exe");
  ASSERT_EQ(ParseInnoUninstallString(L"  C:\\Apps\\unins001.EXE ", &exe), S_OK);
  EXPECT_EQ(exe, L"C:\\Apps\\unins001.EXE");

  for (const wchar_t* bad :
       {L"", L"\"\"", L"\"C:\\Program Files\\App\\unins000.exe",
        L"\"C:\\App\\unins000.exe\" /SILENT", L"C:\\Program Files\\unins.exe", L"\"unins000.exe\"",
        L"\"\\\\server\\share\\unins000.exe\"", L"\"C:\\App\\..\\unins000.exe\"",
        L"\"C:\\App\\unins000.bat\"", L"\"C:\\App\\unins000.exe:x.exe\"",
        L"\"C:/App/unins000.exe\"", L"\"C:\\App\\*.exe\""}) {
    exe = L"stale";
    EXPECT_EQ(ParseInnoUninstallString(bad, &exe), E_INVALIDARG) << bad;
    EXPECT_TRUE(exe.empty()) << bad;
  }
}

TEST(Inno, Arguments) {
  EXPECT_EQ(InnoUninstallArguments(true), L"/SILENT");
  EXPECT_EQ(InnoUninstallArguments(false), L"");
}

// ---- holders that may be ended ----

constexpr wchar_t kSurrogate[] = L"C:\\Windows\\System32\\dllhost.exe";

Holder Found() {
  Holder holder;
  holder.pid = 4000;
  holder.start = {0x1234, 0x5678};
  holder.session = 1;
  holder.image = kSurrogate;
  return holder;
}

HolderFacts Now() {
  HolderFacts facts;
  facts.ownPid = 100;
  facts.ownSession = 1;
  facts.session = 1;
  facts.start = {0x1234, 0x5678};
  facts.image = kSurrogate;
  facts.sameUser = true;
  return facts;
}

TEST(Holders, OnlyTheSurrogateOfThisSessionAndUser) {
  const std::wstring_view allowed[] = {kSurrogate};
  EXPECT_EQ(JudgeHolder(Found(), Now(), allowed), HolderRefusal::kNone);

  HolderFacts facts = Now();
  facts.image = L"c:\\windows\\system32\\..\\System32\\DLLHOST.EXE";  // same file, other spelling
  EXPECT_EQ(JudgeHolder(Found(), facts, allowed), HolderRefusal::kNone);

  facts = Now();
  facts.image = L"C:\\Windows\\explorer.exe";
  EXPECT_EQ(JudgeHolder(Found(), facts, allowed), HolderRefusal::kNotAllowed);
  facts.image = L"D:\\Temp\\dllhost.exe";
  EXPECT_EQ(JudgeHolder(Found(), facts, allowed), HolderRefusal::kNotAllowed);
  facts.image.clear();
  EXPECT_EQ(JudgeHolder(Found(), facts, allowed), HolderRefusal::kNotAllowed);
  EXPECT_EQ(JudgeHolder(Found(), Now(), {}), HolderRefusal::kNotAllowed);

  facts = Now();
  facts.sameUser = false;
  EXPECT_EQ(JudgeHolder(Found(), facts, allowed), HolderRefusal::kOtherUser);

  facts = Now();
  facts.session = 2;
  EXPECT_EQ(JudgeHolder(Found(), facts, allowed), HolderRefusal::kOtherSession);
  Holder other = Found();
  other.session = 0;
  EXPECT_EQ(JudgeHolder(other, Now(), allowed), HolderRefusal::kOtherSession);

  facts = Now();
  facts.start.dwLowDateTime += 1;  // the id now belongs to a later process
  EXPECT_EQ(JudgeHolder(Found(), facts, allowed), HolderRefusal::kReplaced);

  facts = Now();
  facts.ownPid = Found().pid;
  EXPECT_EQ(JudgeHolder(Found(), facts, allowed), HolderRefusal::kSelf);
}

TEST(Holders, ImagesCompareAsWholePaths) {
  EXPECT_TRUE(SameImagePath(L"C:\\Windows\\explorer.exe", L"c:\\windows\\EXPLORER.EXE"));
  EXPECT_TRUE(
      SameImagePath(L"C:\\Windows\\System32\\..\\.\\explorer.exe", L"C:\\Windows\\explorer.exe"));
  EXPECT_FALSE(SameImagePath(L"C:\\Windows\\SysWOW64\\explorer.exe", L"C:\\Windows\\explorer.exe"));
  EXPECT_FALSE(SameImagePath(L"C:\\Windows\\explorer", L"C:\\Windows\\explorer.exe"));
  // Not absolute: no drive, or no root.
  EXPECT_FALSE(SameImagePath(L"\\Windows\\explorer.exe", L"\\Windows\\explorer.exe"));
  EXPECT_FALSE(SameImagePath(L"explorer.exe", L"explorer.exe"));
  EXPECT_FALSE(SameImagePath(L"", L""));
}

}  // namespace
}  // namespace pdft
