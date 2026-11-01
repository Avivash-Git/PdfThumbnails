// The pure parts of the elevation boundary (elevation.h): the step names and the one accepted
// command line, the elevated side's entry checks in their order, how an elevated run's exit is
// read, and how the report pipe's bytes become lines. Also the installer's records (inno.h) and
// which holders of a DLL may be ended (processes.h).

#include <windows.h>

#include "elevation.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "inno.h"
#include "processes.h"
#include "project_config.h"

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
  facts.verb = verb;
  facts.runsFromMachineRoot = true;
  return facts;
}

TEST(ElevatedEntry, AllChecksPass) { EXPECT_EQ(CheckElevatedEntry(Passing()), S_OK); }

TEST(ElevatedEntry, RefusalsInOrder) {
  ElevatedEntryFacts facts = Passing();
  facts.reparsePoints = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  EXPECT_EQ(CheckElevatedEntry(facts), HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  facts.runsFromMachineRoot = false;
  EXPECT_EQ(CheckElevatedEntry(facts), E_ACCESSDENIED);
  facts.verb.reset();
  EXPECT_EQ(CheckElevatedEntry(facts), E_INVALIDARG);
  facts.packaged = true;
  EXPECT_EQ(CheckElevatedEntry(facts), HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED_IN_APPCONTAINER));
  facts.tokenElevated = false;
  EXPECT_EQ(CheckElevatedEntry(facts), HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED));
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
