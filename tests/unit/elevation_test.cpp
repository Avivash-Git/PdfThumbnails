// The pure parts of the elevation boundary (elevation.h): the step names and the one accepted
// command line, the elevated side's entry checks in their order (administrator rights in every
// combination of the token's facts), how an elevated run's exit is read, how the report pipe's
// bytes become lines, which pipe the elevated side writes to, and a report channel another
// process took. Also the installer's records (inno.h) and which holders of a DLL may be ended
// (processes.h).

#include <windows.h>

#include "elevation.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "inno.h"
#include "processes.h"
#include "project_config.h"
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
  ElevatedEntryFacts facts = Passing();
  facts.reparsePoints = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kReparsePoint);
  EXPECT_EQ(CheckElevatedEntry(facts), HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  facts.runsFromMachineRoot = false;
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kNotFromMachineRoot);
  EXPECT_EQ(CheckElevatedEntry(facts), E_ACCESSDENIED);
  facts.verb.reset();
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kNotOneStep);
  EXPECT_EQ(CheckElevatedEntry(facts), E_INVALIDARG);
  facts.packaged = true;
  EXPECT_EQ(FindElevatedRefusal(facts), ElevatedRefusal::kPackaged);
  EXPECT_EQ(CheckElevatedEntry(facts), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED_IN_APPCONTAINER));
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

        // Everything else wrong too: the token still answers first.
        facts.packaged = true;
        facts.verb.reset();
        facts.runsFromMachineRoot = false;
        facts.reparsePoints = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
        EXPECT_EQ(FindElevatedRefusal(facts),
                  expected == ElevatedRefusal::kNone ? ElevatedRefusal::kPackaged : expected)
            << label;
      }
    }
  }
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

}  // namespace
}  // namespace pdft
