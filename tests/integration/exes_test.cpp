// Both executables, started the way a script starts them: standard output and error on pipes,
// the exit code checked. They sit next to this test program (build folder and test bundle).
//
// Freshly built, unsigned executables can be refused by Smart App Control or another
// Application Control policy. Such a refusal is a skip, never a pass or a failure, and an
// executable that was refused once is not started again in this run.

#include <windows.h>

#include <gtest/gtest.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <wil/resource.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "child_process.h"
#include "cli_args.h"
#include "commands.h"
#include "context.h"
#include "diagnostics.h"
#include "elevation.h"
#include "evaluate.h"
#include "github_runner.h"
#include "instance.h"
#include "json_check.h"
#include "medium_il.h"
#include "project_config.h"
#include "rebuild.h"
#include "refresh.h"
#include "snapshot.h"
#include "strings.h"
#include "system_probes.h"
#include "temp_dir.h"

namespace pdft {
namespace {

// The first start of a new file under Smart App Control can take about 10 s (a cloud lookup).
constexpr DWORD kTimeoutMs = 120000;

std::set<std::wstring>& Refused() {
  static std::set<std::wstring> refused;
  return refused;
}

class Exes : public ::testing::TestWithParam<const wchar_t*> {
 protected:
  std::wstring Exe() const { return test::SelfFolder() + L"\\" + GetParam(); }
  bool IsSettings() const { return std::wstring(GetParam()) == kSettingsExe; }

  // Runs the executable; false (and the test is skipped) when Application Control refused it.
  bool Run(const std::wstring& arguments, test::CapturedRun* run) {
    if (Refused().count(GetParam())) return false;
    *run = test::RunCaptured(Exe(), arguments, kTimeoutMs);
    if (test::IsApplicationControlBlock(run->hr)) {
      Refused().insert(GetParam());
      return false;
    }
    return true;
  }
};

// Why a writing command must not be started here, or nothing when it may be: only where this
// process's own canary (it runs where the executable does) shows that registry writes are
// swallowed, under a packaged app. There the executable refuses before it changes anything and
// logs nothing. Everywhere else the command would really run (or, where the canary cannot
// decide, the executable's own canary might pass and it would run).
std::optional<std::string> WhyAWritingCommandMayNotRunHere(std::wstring* ancestorName) {
  std::wstring sid;
  if (FAILED(GetCurrentUserSid(&sid))) return "the user's SID could not be read";
  Virtualization virtualization = Virtualization::kUnknown;
  HRESULT failure = S_OK;
  if (FAILED(ProbeRegistryVirtualization(sid, &virtualization, &failure))) {
    return "the registry check failed";
  }
  std::optional<std::wstring> ancestor;
  if (FAILED(FindPackagedAncestor(&ancestor))) return "the process tree could not be read";
  if (virtualization == Virtualization::kNo) {
    return "the registry is not virtualized here, so the command would really run (and change "
           "the real registration); it is checked where the guard refuses it";
  }
  if (virtualization == Virtualization::kUnknown) {
    return "the registry check could not decide here (" + WideToUtf8Lossy(HResultText(failure)) +
           "), so the executable's own check might pass and the command would really run";
  }
  if (!ancestor) {
    return "registry writes are swallowed, but no packaged app was found above this process, "
           "so the executable would log its refusal in the real profile";
  }
  if (ancestorName) *ancestorName = *ancestor;
  return std::nullopt;
}

#define RUN_OR_SKIP(arguments, run)                                                             \
  if (!Run(arguments, &run)) {                                                                  \
    GTEST_SKIP() << "Application Control refused to start " << ::testing::PrintToString(Exe()); \
  }                                                                                             \
  ASSERT_HRESULT_SUCCEEDED(run.hr)

TEST_P(Exes, Version) {
  test::CapturedRun run;
  RUN_OR_SKIP(L"--version", run);
  EXPECT_EQ(run.exitCode, 0u) << run.err;
  EXPECT_EQ(run.out, WideToUtf8Lossy(VersionText()) + "\r\n");
  EXPECT_EQ(run.err, "");
}

TEST_P(Exes, HelpListsEveryCommand) {
  test::CapturedRun run;
  RUN_OR_SKIP(L"--help", run);
  EXPECT_EQ(run.exitCode, 0u) << run.err;
  for (const VerbInfo& verb : Verbs()) {
    if (verb.verb == Verb::kNone || verb.hidden || (verb.devOnly && !kDevInstall)) continue;
    EXPECT_NE(run.out.find(WideToUtf8Lossy(verb.name)), std::string::npos)
        << WideToUtf8Lossy(verb.name);
  }
  EXPECT_EQ(run.out.find("--elevated"), std::string::npos);
  EXPECT_NE(run.out.find("Exit codes:"), std::string::npos);
}

TEST_P(Exes, UnknownOptionIsAUsageError) {
  test::CapturedRun run;
  RUN_OR_SKIP(L"--bogus", run);
  EXPECT_EQ(run.exitCode, 2u);
  EXPECT_NE(run.err.find("--bogus"), std::string::npos) << run.err;
  EXPECT_NE(run.err.find("Run --help"), std::string::npos) << run.err;
  EXPECT_EQ(run.out, "");
}

// --zoom 120 writes, so the executable must stop at the virtualization guard (exit 8) before it
// does anything. No test may start a writing command of the executables where its writes could
// reach the real profile, so this one runs only where this process's own canary (it runs where
// the executable does) shows that writes are swallowed, under a packaged app: there the
// executable refuses and logs nothing. Everywhere else it is skipped. Where the canary cannot
// decide, the executable's own canary might still succeed, so that case is skipped too. Checking
// the side of the guard that lets a command through needs a writing path that is sandboxed, not
// one of the real commands.
TEST_P(Exes, WritingCommandPassesTheVirtualizationGuardFirst) {
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  Virtualization virtualization = Virtualization::kUnknown;
  HRESULT failure = S_OK;
  ASSERT_HRESULT_SUCCEEDED(ProbeRegistryVirtualization(sid, &virtualization, &failure));
  std::optional<std::wstring> ancestor;
  ASSERT_HRESULT_SUCCEEDED(FindPackagedAncestor(&ancestor));
  if (virtualization == Virtualization::kNo) {
    GTEST_SKIP() << "the registry is not virtualized here, so --zoom 120 would really run (it "
                    "writes the app log under %LOCALAPPDATA%\\"
                 << kProductName
                 << "\\Logs today and would set the real zoom and refresh thumbnails once "
                    "implemented); the guard is checked where it refuses";
  }
  if (virtualization == Virtualization::kUnknown) {
    GTEST_SKIP() << "the registry check could not decide here (" << HResultText(failure)
                 << "), so the executable's own check might pass and --zoom 120 would really run";
  }
  if (!ancestor) {
    GTEST_SKIP() << "registry writes are swallowed, but no packaged app was found above this "
                    "process, so the executable would log its refusal in the real profile";
  }

  test::CapturedRun run;
  RUN_OR_SKIP(L"--zoom 120", run);
  std::printf("%ls --zoom 120 -> exit %lu: %s", GetParam(), run.exitCode, run.err.c_str());
  EXPECT_EQ(run.exitCode, 8u);
  EXPECT_EQ(run.err.rfind("Cannot run here: ", 0), 0u) << run.err;
  EXPECT_NE(run.err.find("registry virtualized"), std::string::npos) << run.err;
  EXPECT_NE(run.err.find(WideToUtf8Lossy(*ancestor)), std::string::npos) << run.err;
  EXPECT_EQ(run.out, "");
}

// --status reads the real state, read-only. What it finds depends on the machine (CI has
// nothing installed, this PC has 1.1, a container skips the Windows checks), so the test checks
// the shape of the report and that the exit code matches its last line. The Explorer test stays
// off: no test starts a thumbnail process.
TEST_P(Exes, StatusReportsWithoutTheExplorerTest) {
  test::CapturedRun run;
  RUN_OR_SKIP(L"--status --no-live-test", run);
  // Not worded like gate.planting's "<exe> --status --no-live-test -> exit 0x...": CI looks for
  // that line in the same test log to prove the gate ran --status.
  std::printf("status run (%ls): exit %lu\n%s", GetParam(), run.exitCode, run.out.c_str());
  EXPECT_EQ(run.err, "");
  const std::string header = WideToUtf8Lossy(VersionText()) + ", Windows build ";
  EXPECT_EQ(run.out.rfind(header, 0), 0u) << run.out;
  EXPECT_NE(run.out.find("     Explorer test  skipped: "), std::string::npos) << run.out;
  ASSERT_GE(run.out.size(), 2u);
  EXPECT_EQ(run.out.substr(run.out.size() - 2), "\r\n");
  const size_t last = run.out.rfind("\r\nResult: ");
  ASSERT_NE(last, std::string::npos) << run.out;
  const std::string result = run.out.substr(last + 2, run.out.size() - last - 4);
  switch (run.exitCode) {
    case 0:
      EXPECT_EQ(result, "Result: healthy");
      break;
    case 1:
      EXPECT_NE(result.find("from working"), std::string::npos) << result;
      break;
    case 6:
      EXPECT_EQ(result, "Result: not installed");
      break;
    default:
      ADD_FAILURE() << "exit code " << run.exitCode;
  }
}

TEST_P(Exes, StatusJsonIsSchemaOne) {
  test::CapturedRun run;
  RUN_OR_SKIP(L"--status --json --no-live-test", run);
  EXPECT_EQ(run.err, "");
  EXPECT_TRUE(run.exitCode == 0 || run.exitCode == 1 || run.exitCode == 6) << run.exitCode;
  test::JsonValue root;
  std::string error;
  ASSERT_TRUE(test::ParseJson(run.out, &root, &error)) << error << "\n" << run.out;
  ASSERT_EQ(root.type, test::JsonValue::Type::kObject);
  ASSERT_TRUE(root.Get("schema"));
  EXPECT_EQ(root.Get("schema")->text, "1");
  ASSERT_TRUE(root.Get("exitCode"));
  EXPECT_EQ(root.Get("exitCode")->text, std::to_string(run.exitCode));
  ASSERT_TRUE(root.Get("product"));
  EXPECT_EQ(root.Get("product")->Get("version")->text, WideToUtf8Lossy(kProductVersion));
  ASSERT_TRUE(root.Get("liveTest"));
  EXPECT_FALSE(root.Get("liveTest")->Get("ran")->boolean);
  ASSERT_TRUE(root.Get("findings"));
  EXPECT_EQ(root.Get("findings")->type, test::JsonValue::Type::kArray);
  ASSERT_TRUE(root.Get("healthy"));
  EXPECT_EQ(root.Get("healthy")->boolean, run.exitCode == 0);
}

// The report masks this user's SID, profile folder and names.
TEST_P(Exes, DiagnosticsMaskThisUser) {
  test::CapturedRun run;
  RUN_OR_SKIP(L"--diagnostics", run);
  EXPECT_EQ(run.exitCode, 0u) << run.err;
  EXPECT_EQ(run.err, "");
  EXPECT_NE(run.out.find(WideToUtf8Lossy(kProductName) + " diagnostics"), std::string::npos)
      << run.out;
  EXPECT_NE(run.out.find("Log (last 30 lines)"), std::string::npos);

  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context));
  RedactContext redact;
  ASSERT_HRESULT_SUCCEEDED(GetRedactContext(context, &redact));
  const auto lower = [](std::string text) {
    for (char& c : text) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
  };
  const std::string report = lower(run.out);
  EXPECT_EQ(report.find(lower(WideToUtf8Lossy(context.userSid))), std::string::npos);
  ASSERT_FALSE(redact.profile.empty());
  EXPECT_EQ(report.find(lower(WideToUtf8Lossy(redact.profile))), std::string::npos);
  if (!redact.profileShort.empty()) {
    EXPECT_EQ(report.find(lower(WideToUtf8Lossy(redact.profileShort))), std::string::npos);
  }
  // Names this long are masked wherever they stand (shorter ones only as words of their own).
  for (const std::wstring& name : {redact.user, redact.computer, redact.hostName}) {
    if (name.size() >= kRedactAnywhereChars) {
      EXPECT_EQ(report.find(lower(WideToUtf8Lossy(name))), std::string::npos);
    }
  }
}

// Elevated (CI), --diagnostics does not read the log, which lies in a folder the user's
// unelevated programs control, and says so where its lines would be; --out is refused (exit 8)
// before anything is read, and no file is made. Both runs only read.
TEST_P(Exes, ElevatedDiagnosticsReadNoLogAndWriteNoFile) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context));
  if (!context.elevated) GTEST_SKIP() << "not elevated here (CI runs elevated)";

  test::CapturedRun run;
  RUN_OR_SKIP(L"--diagnostics", run);
  EXPECT_EQ(run.exitCode, 0u) << run.err;
  EXPECT_EQ(run.err, "");
  RedactContext redact;
  ASSERT_HRESULT_SUCCEEDED(GetRedactContext(context, &redact));
  // The line as the report masks it (an account named like a word of it would be masked too).
  const std::string notRead =
      WideToUtf8Lossy(Redact(L"  " + std::wstring(kLogNotReadElevated), redact));
  EXPECT_NE(run.out.find("Log (last 30 lines)\r\n" + notRead + "\r\n"), std::string::npos)
      << run.out;

  test::TempDir dir(L"diagnostics");
  ASSERT_TRUE(dir.ok());
  const std::wstring file = dir.File(L"report.txt");
  RUN_OR_SKIP(L"--diagnostics --out " + test::QuoteArgument(file), run);
  std::printf("%ls --diagnostics --out (elevated) -> exit %lu: %s", GetParam(), run.exitCode,
              run.err.c_str());
  EXPECT_EQ(run.exitCode, 8u);
  EXPECT_EQ(run.err.rfind(WideToUtf8Lossy(DiagnosticsOutElevatedRefusalMessage()), 0), 0u)
      << run.err;
  EXPECT_EQ(run.out, "");
  EXPECT_EQ(GetFileAttributesW(file.c_str()), INVALID_FILE_ATTRIBUTES);
}

// Not elevated, --diagnostics --out replaces a file that is already there with the report, as
// UTF-8 with a byte order mark, and says where it went. Apart from that file it only reads.
TEST_P(Exes, DiagnosticsOutReplacesTheFile) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context));
  if (context.elevated) GTEST_SKIP() << "elevated here (--out is refused there)";

  test::TempDir dir(L"diagnostics");
  ASSERT_TRUE(dir.ok());
  const std::wstring file = dir.File(L"report.txt");
  std::ofstream(file, std::ios::binary) << "an older report";
  test::CapturedRun run;
  RUN_OR_SKIP(L"--diagnostics --out " + test::QuoteArgument(file), run);
  EXPECT_EQ(run.exitCode, 0u) << run.err;
  EXPECT_EQ(run.err, "");
  EXPECT_EQ(run.out.rfind("Diagnostics written to ", 0), 0u) << run.out;
  std::ifstream stream(file, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
  EXPECT_EQ(text.rfind("\xEF\xBB\xBF" + WideToUtf8Lossy(kProductName) + " diagnostics", 0), 0u)
      << text.substr(0, 200);
  EXPECT_EQ(text.find("an older report"), std::string::npos);
}

// --check and --fix write, so they run only where the guard refuses them (exit 8) before they
// read or change anything; see WhyAWritingCommandMayNotRunHere.
TEST_P(Exes, CheckAndFixAreRefusedWhereWritesAreSwallowed) {
  std::wstring ancestor;
  if (const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(&ancestor)) {
    GTEST_SKIP() << *why;
  }
  for (const wchar_t* arguments : {L"--check --no-live-test", L"--check --quiet", L"--fix"}) {
    test::CapturedRun run;
    RUN_OR_SKIP(arguments, run);
    std::printf("%ls %ls -> exit %lu: %s", GetParam(), arguments, run.exitCode, run.err.c_str());
    EXPECT_EQ(run.exitCode, 8u) << WideToUtf8Lossy(arguments);
    EXPECT_EQ(run.err.rfind("Cannot run here: ", 0), 0u) << run.err;
    EXPECT_NE(run.err.find(WideToUtf8Lossy(ancestor)), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

// --checks writes the user's check task, so it runs only where a guard refuses it before it reads
// or changes anything: the virtualization guard here (see WhyAWritingCommandMayNotRunHere).
TEST_P(Exes, ChecksAreRefusedWhereWritesAreSwallowed) {
  std::wstring ancestor;
  if (const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(&ancestor)) {
    GTEST_SKIP() << *why;
  }
  for (const wchar_t* value : {L"default", L"pdfcraft,signin", L"pdfcraft", L"signin", L"none"}) {
    const std::wstring arguments = std::wstring(L"--checks ") + value;
    test::CapturedRun run;
    RUN_OR_SKIP(arguments, run);
    std::printf("%ls %ls -> exit %lu: %s", GetParam(), arguments.c_str(), run.exitCode,
                run.err.c_str());
    EXPECT_EQ(run.exitCode, 8u) << WideToUtf8Lossy(arguments);
    EXPECT_EQ(run.err.rfind("Cannot run here: ", 0), 0u) << run.err;
    EXPECT_NE(run.err.find(WideToUtf8Lossy(ancestor)), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

// The installer's per-user step and the 1.x clean-up write the user's settings, so they run only
// where the virtualization guard refuses them before they read or change anything (and before
// --migrate-v1 could ask for administrator rights).
TEST_P(Exes, SetupUserAndMigrationAreRefusedWhereWritesAreSwallowed) {
  std::wstring ancestor;
  if (const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(&ancestor)) {
    GTEST_SKIP() << *why;
  }
  for (const wchar_t* arguments :
       {L"--setup-user", L"--setup-user --checks signin --zoom 120", L"--migrate-v1",
        L"--migrate-v1 --user-only", L"--migrate-v1 --machine-only"}) {
    test::CapturedRun run;
    RUN_OR_SKIP(arguments, run);
    std::printf("%ls %ls -> exit %lu: %s", GetParam(), arguments, run.exitCode, run.err.c_str());
    EXPECT_EQ(run.exitCode, 8u) << WideToUtf8Lossy(arguments);
    EXPECT_EQ(run.err.rfind("Cannot run here: ", 0), 0u) << run.err;
    EXPECT_NE(run.err.find(WideToUtf8Lossy(ancestor)), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

// Elevated where the registry is the real one (CI), they stop at their elevation guard (exit 8)
// before anything is read: everything they change belongs to the signed-in user.
TEST_P(Exes, SetupUserAndMigrationAreRefusedElevated) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &context));
  if (!context.elevated) GTEST_SKIP() << "not elevated here (CI runs elevated)";
  if (context.virtualization != Virtualization::kNo) {
    GTEST_SKIP() << "the registry may be virtualized here, so the virtualization guard answers "
                    "first (checked by SetupUserAndMigrationAreRefusedWhereWritesAreSwallowed)";
  }
  for (const wchar_t* arguments : {L"--setup-user", L"--migrate-v1"}) {
    test::CapturedRun run;
    RUN_OR_SKIP(arguments, run);
    std::printf("%ls %ls (elevated) -> exit %lu: %s", GetParam(), arguments, run.exitCode,
                run.err.c_str());
    EXPECT_EQ(run.exitCode, 8u) << WideToUtf8Lossy(arguments);
    EXPECT_NE(run.err.find("running as administrator"), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

// --setup-machine changes the PC for all users, so it is only started where it stops at once:
// unelevated, it needs an administrator (exit 7) and touches nothing.
TEST_P(Exes, SetupMachineNeedsAnAdministrator) {
  bool elevated = false;
  ASSERT_HRESULT_SUCCEEDED(GetTokenElevation(&elevated));
  if (elevated) GTEST_SKIP() << "elevated: it would really set up this PC";
  test::CapturedRun run;
  RUN_OR_SKIP(L"--setup-machine", run);
  std::printf("%ls --setup-machine -> exit %lu: %s", GetParam(), run.exitCode, run.err.c_str());
  EXPECT_EQ(run.exitCode, 7u);
  EXPECT_NE(run.err.find("administrator"), std::string::npos) << run.err;
  EXPECT_EQ(run.out, "");
}

// A value that is not one of the five is a usage error before anything is read, anywhere.
TEST_P(Exes, ChecksValueIsChecked) {
  for (const wchar_t* arguments : {L"--checks bogus", L"--checks", L"--checks=signin,pdfcraft"}) {
    test::CapturedRun run;
    RUN_OR_SKIP(arguments, run);
    EXPECT_EQ(run.exitCode, 2u) << WideToUtf8Lossy(arguments);
    EXPECT_NE(run.err.find("--checks"), std::string::npos) << run.err;
    EXPECT_NE(run.err.find("Run --help"), std::string::npos) << run.err;
    EXPECT_EQ(run.err.find("Cannot run here"), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

// The elevated half of a change refuses to run from anywhere but the install folder: this build
// folder is not under Program Files, so the step never starts. Without administrator rights it
// stops even earlier, at the token check; the child runs with this process's token, so what the
// entry check makes of that token here says which refusal comes first. Only the settings
// executable knows --elevated at all. The step is one that never installs anything;
// machine-install is never started by a test.
TEST_P(Exes, ElevatedStepRefusedOutsideTheInstallFolder) {
  test::CapturedRun run;
  RUN_OR_SKIP(L"--elevated machine-repair-class", run);
  if (!IsSettings()) {
    EXPECT_EQ(run.exitCode, 2u) << run.err;
    return;
  }
  ElevatedEntryFacts facts;
  ASSERT_HRESULT_SUCCEEDED(ReadEntryTokenFacts(nullptr, &facts));
  ReadPackageAbove(&facts.machineWrites);
  facts.verb = ElevatedVerb::kMachineRepairClass;
  facts.runsFromMachineRoot = false;
  const ElevatedRefusal refusal = FindElevatedRefusal(facts);
  std::printf("%ls --elevated machine-repair-class (refusal expected: %ls) -> exit 0x%08lX\n",
              GetParam(),
              refusal == ElevatedRefusal::kNone ? L"none" : ElevatedRefusalText(refusal).data(),
              run.exitCode);
  EXPECT_NE(refusal, ElevatedRefusal::kNone);
  // The refusal these facts give comes before the package check, which is the last one, and the
  // exit code below shows that the child refused with it too. That a step refused this early
  // makes no test writes is DecideElevatedEntry's order (unit-tested): this test cannot see them.
  EXPECT_NE(refusal, ElevatedRefusal::kPackaged);
  EXPECT_EQ(run.exitCode, static_cast<DWORD>(CheckElevatedEntry(facts)));
  EXPECT_EQ(run.out, "");
  EXPECT_EQ(run.err, "");
}

// Elevated where the registry is the real one (CI), --checks stops at its elevation guard (exit
// 8) before it reads anything; the task itself refuses an elevated caller too, and nothing is
// installed there either (exit 6), so a wrong order still changes nothing.
TEST_P(Exes, ChecksAreRefusedElevated) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &context));
  if (!context.elevated) GTEST_SKIP() << "not elevated here (CI runs elevated)";
  if (context.virtualization != Virtualization::kNo) {
    GTEST_SKIP() << "the registry may be virtualized here, so the virtualization guard answers "
                    "first";
  }
  test::CapturedRun run;
  RUN_OR_SKIP(L"--checks default", run);
  std::printf("%ls --checks default (elevated) -> exit %lu: %s", GetParam(), run.exitCode,
              run.err.c_str());
  EXPECT_EQ(run.exitCode, 8u);
  EXPECT_EQ(run.err.rfind(WideToUtf8Lossy(ChecksElevatedRefusalMessage()), 0), 0u) << run.err;
  EXPECT_EQ(run.out, "");
}

// Where nothing is installed and the registry is the real one (CI), --checks answers "not
// installed" (exit 6) without registering anything. It runs at Medium integrity, with a
// standard user's token where the test runs elevated. Skipped wherever something is installed
// (this PC has 1.1), because there it could really change the task.
TEST_P(Exes, ChecksNeedAnInstall) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &context));
  if (context.virtualization != Virtualization::kNo) {
    GTEST_SKIP() << "the registry may be virtualized here, so the virtualization guard answers "
                    "first (checked by ChecksAreRefusedWhereWritesAreSwallowed)";
  }
  GatherOptions options;
  options.liveTest = false;
  options.context = context;
  Snapshot snapshot;
  ASSERT_HRESULT_SUCCEEDED(GatherSnapshot(RegistryRoots{}, options, RealSystemProbes(), &snapshot));
  if (!HasFinding(Evaluate(snapshot), FindingId::kNotInstalled)) {
    GTEST_SKIP() << "something is installed here, so --checks would really change the check task";
  }
  if (Refused().count(GetParam())) GTEST_SKIP() << "Application Control refused it before";

  std::string output;
  DWORD exitCode = 0;
  if (context.elevated) {
    wil::unique_handle token;
    ASSERT_HRESULT_SUCCEEDED(
        test::CreateMediumTokenForSelf(test::MediumToken::kFiltered, token.put()));
    test::PipedChild child;
    const HRESULT started = child.Start(Exe(), L"--checks default", token.get());
    if (test::IsApplicationControlBlock(started)) {
      Refused().insert(GetParam());
      GTEST_SKIP() << "Application Control refused to start " << ::testing::PrintToString(Exe());
    }
    ASSERT_HRESULT_SUCCEEDED(started);
    child.CloseInput();
    for (std::string line = child.ReadLine(); !line.empty(); line = child.ReadLine()) {
      output += line + "\n";
    }
    exitCode = child.Wait(kTimeoutMs);
  } else {
    test::CapturedRun run;
    RUN_OR_SKIP(L"--checks default", run);
    output = run.out + run.err;
    exitCode = run.exitCode;
  }
  std::printf("%ls --checks default (not installed) -> exit %lu: %s", GetParam(), exitCode,
              output.c_str());
  if (exitCode == 8 && output.find("registry check") != std::string::npos) {
    GTEST_SKIP() << "the executable's own registry check could not decide: " << output;
  }
  EXPECT_EQ(exitCode, 6u) << output;
  EXPECT_NE(output.find(" is not installed"), std::string::npos) << output;
}

// Taking slots back needs --yes when there is no console to ask on, and that is decided before
// the process context, the guard or anything else is read: exit 2 even where the guard would
// refuse with 8. The command-line executable started without a window still gets a console of
// its own (hidden, nobody to answer), which must count as none. Started only where the guard would
// refuse anyway, so that a wrong order could never take slots back for real.
TEST_P(Exes, FixTakeBackWithoutYesAndWithoutAConsoleIsAUsageError) {
  if (const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(nullptr)) {
    GTEST_SKIP() << *why;
  }
  test::CapturedRun run;
  RUN_OR_SKIP(L"--fix --take-back", run);
  std::printf("%ls --fix --take-back -> exit %lu: %s", GetParam(), run.exitCode, run.err.c_str());
  EXPECT_EQ(run.exitCode, 2u);
  EXPECT_NE(run.err.find("--yes"), std::string::npos) << run.err;
  EXPECT_EQ(run.err.find("Cannot run here"), std::string::npos) << run.err;
  EXPECT_EQ(run.out, "");
}

// Why the elevated tests of a rebuild (--refresh --full, --uninstall --rebuild-thumbnails) must
// not start the executable here, or nothing when they may. Elevated, only the elevation refusals
// under test would keep a run from ending File Explorer and deleting the thumbnail cache, so they
// run only where both are thrown away after the job: elevated with the real registry on a runner
// GitHub hosts.
std::optional<std::string> WhyAnElevatedRefreshMayNotRunHere(const ProcessContext& context) {
  if (!context.elevated) return "not elevated here (CI runs elevated)";
  if (context.virtualization != Virtualization::kNo) {
    return "elevated, but the registry may be virtualized here, so it is not certain which "
           "refusal answers first";
  }
  if (!test::OnGitHubHostedRunner()) {
    return "elevated, but not on a runner GitHub hosts (GITHUB_ACTIONS=true with "
           "RUNNER_ENVIRONMENT=github-hosted): only the refusal under test would keep this PC's "
           "File Explorer and thumbnail cache from a real rebuild";
  }
  return std::nullopt;
}

// The commands that install, uninstall or set the zoom, in the forms a test may start.
std::vector<std::wstring> InstallUninstallZoomCommands() {
  std::vector<std::wstring> commands = {L"--uninstall --yes",
                                        L"--uninstall --yes --rebuild-thumbnails", L"--zoom 130",
                                        L"--zoom 130 --no-refresh"};
  if (kDevInstall) {
    commands.emplace_back(L"--install");
    commands.emplace_back(L"--install --user --checks signin --zoom 120");
  }
  return commands;
}

// --install, --uninstall and --zoom <n> write (and the first two would ask for administrator
// rights), so they run only where the virtualization guard refuses them before anything is read.
TEST_P(Exes, InstallUninstallAndZoomAreRefusedWhereWritesAreSwallowed) {
  std::wstring ancestor;
  if (const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(&ancestor)) {
    GTEST_SKIP() << *why;
  }
  for (const std::wstring& arguments : InstallUninstallZoomCommands()) {
    test::CapturedRun run;
    RUN_OR_SKIP(arguments, run);
    std::printf("%ls %ls -> exit %lu: %s", GetParam(), arguments.c_str(), run.exitCode,
                run.err.c_str());
    EXPECT_EQ(run.exitCode, 8u) << WideToUtf8Lossy(arguments);
    EXPECT_EQ(run.err.rfind("Cannot run here: ", 0), 0u) << run.err;
    EXPECT_NE(run.err.find(WideToUtf8Lossy(ancestor)), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

// Elevated where the registry is the real one (CI), they stop at their elevation guard (exit 8)
// before anything else is read: each has a part that belongs to the signed-in user, and the
// install and the uninstall ask for administrator rights themselves. The uninstall's rebuild is
// started only where the elevated --refresh tests start one (WhyAnElevatedRefreshMayNotRunHere).
TEST_P(Exes, InstallUninstallAndZoomAreRefusedElevated) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &context));
  if (!context.elevated) GTEST_SKIP() << "not elevated here (CI runs elevated)";
  if (context.virtualization != Virtualization::kNo) {
    GTEST_SKIP() << "the registry may be virtualized here, so the virtualization guard answers "
                    "first";
  }
  const std::optional<std::string> whyNoRebuild = WhyAnElevatedRefreshMayNotRunHere(context);
  for (const std::wstring& arguments : InstallUninstallZoomCommands()) {
    if (whyNoRebuild && arguments.find(L"--rebuild-thumbnails") != std::wstring::npos) {
      std::printf("%ls %ls (elevated) not started: %s\n", GetParam(), arguments.c_str(),
                  whyNoRebuild->c_str());
      continue;
    }
    test::CapturedRun run;
    RUN_OR_SKIP(arguments, run);
    std::printf("%ls %ls (elevated) -> exit %lu: %s", GetParam(), arguments.c_str(), run.exitCode,
                run.err.c_str());
    EXPECT_EQ(run.exitCode, 8u) << WideToUtf8Lossy(arguments);
    EXPECT_NE(run.err.find("running as administrator"), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

// --uninstall removes the product for all users: without --yes and without a console to ask on
// it is a usage error before anything is read. Started only where the guard would refuse anyway.
TEST_P(Exes, UninstallWithoutYesAndWithoutAConsoleIsAUsageError) {
  if (const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(nullptr)) {
    GTEST_SKIP() << *why;
  }
  test::CapturedRun run;
  RUN_OR_SKIP(L"--uninstall", run);
  std::printf("%ls --uninstall -> exit %lu: %s", GetParam(), run.exitCode, run.err.c_str());
  EXPECT_EQ(run.exitCode, 2u);
  EXPECT_NE(run.err.find("--yes"), std::string::npos) << run.err;
  EXPECT_EQ(run.err.find("Cannot run here"), std::string::npos) << run.err;
  EXPECT_EQ(run.out, "");
}

// Why --refresh must not be started here, or nothing when it may be. A real rebuild would end
// File Explorer and delete the thumbnail cache, so it is started only where it must be refused
// before it reads anything: where the virtualization guard refuses every writing command
// (WhyAWritingCommandMayNotRunHere), and elevated with the real registry on a runner GitHub hosts
// (WhyAnElevatedRefreshMayNotRunHere), where the rebuild refuses an administrator process. That
// holds for its usage errors too, so a parser that let one through could still change nothing.
std::optional<std::string> WhyARefreshMayNotRunHere() {
  const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(nullptr);
  if (!why) return std::nullopt;
  ProcessContext context;
  if (FAILED(GetProcessContext(ContextProbe::kWrite, &context))) {
    return "the process context could not be read";
  }
  const std::optional<std::string> whyElevated = WhyAnElevatedRefreshMayNotRunHere(context);
  if (!whyElevated) return std::nullopt;
  return *why + "; " + *whyElevated;
}

// --refresh --full writes, so where the virtualization guard refuses writing commands it stops
// there (exit 8) before it reads anything or asks.
TEST_P(Exes, RefreshFullIsRefusedWhereWritesAreSwallowed) {
  std::wstring ancestor;
  if (const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(&ancestor)) {
    GTEST_SKIP() << *why;
  }
  test::CapturedRun run;
  RUN_OR_SKIP(L"--refresh --full --yes", run);
  std::printf("%ls --refresh --full --yes -> exit %lu: %s", GetParam(), run.exitCode,
              run.err.c_str());
  EXPECT_EQ(run.exitCode, 8u);
  EXPECT_EQ(run.err.rfind("Cannot run here: ", 0), 0u) << run.err;
  EXPECT_NE(run.err.find(WideToUtf8Lossy(ancestor)), std::string::npos) << run.err;
  EXPECT_EQ(run.out, "");
}

// Elevated with the real registry on a runner GitHub hosts (CI), --refresh --full stops at the
// rebuild's elevation refusal (exit 8) before it reads anything or asks: File Explorer must not
// run as administrator, and an elevated process may not end the user's.
TEST_P(Exes, RefreshFullIsRefusedElevated) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &context));
  if (const std::optional<std::string> why = WhyAnElevatedRefreshMayNotRunHere(context)) {
    GTEST_SKIP() << *why;
  }
  test::CapturedRun run;
  RUN_OR_SKIP(L"--refresh --full --yes", run);
  std::printf("%ls --refresh --full --yes (elevated) -> exit %lu: %s", GetParam(), run.exitCode,
              run.err.c_str());
  EXPECT_EQ(run.exitCode, 8u);
  EXPECT_EQ(run.err.rfind(
                WideToUtf8Lossy(RefreshFullRefusalMessage(RebuildRefusal::kElevated, context)), 0),
            0u)
      << run.err;
  EXPECT_EQ(run.out, "");
}

// The --folder a refusal test names: one that does not exist, in a folder of the test's own, so
// even a command that went past its refusal would find nothing to refresh (exit 2).
std::wstring MissingFolder(const test::TempDir& dir) { return dir.File(L"no-such-folder"); }

// --refresh --folder writes (the thumbnail cache), so where the virtualization guard refuses
// writing commands it stops there (exit 8) before it reads anything.
TEST_P(Exes, RefreshFolderIsRefusedWhereWritesAreSwallowed) {
  std::wstring ancestor;
  if (const std::optional<std::string> why = WhyAWritingCommandMayNotRunHere(&ancestor)) {
    GTEST_SKIP() << *why;
  }
  test::TempDir dir(L"refresh-folder");
  ASSERT_TRUE(dir.ok());
  test::CapturedRun run;
  RUN_OR_SKIP(L"--refresh --folder \"" + MissingFolder(dir) + L"\"", run);
  std::printf("%ls --refresh --folder -> exit %lu: %s", GetParam(), run.exitCode, run.err.c_str());
  EXPECT_EQ(run.exitCode, 8u);
  EXPECT_EQ(run.err.rfind("Cannot run here: ", 0), 0u) << run.err;
  EXPECT_NE(run.err.find(WideToUtf8Lossy(ancestor)), std::string::npos) << run.err;
  EXPECT_EQ(run.out, "");
}

// Elevated with the real registry on a runner GitHub hosts (CI), --refresh --folder stops at its
// elevation refusal (exit 8) before it reads anything else or looks at the folder: the thumbnail
// cache is the signed-in user's own.
TEST_P(Exes, RefreshFolderIsRefusedElevated) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &context));
  if (const std::optional<std::string> why = WhyAnElevatedRefreshMayNotRunHere(context)) {
    GTEST_SKIP() << *why;
  }
  test::TempDir dir(L"refresh-folder");
  ASSERT_TRUE(dir.ok());
  test::CapturedRun run;
  RUN_OR_SKIP(L"--refresh --folder \"" + MissingFolder(dir) + L"\"", run);
  std::printf("%ls --refresh --folder (elevated) -> exit %lu: %s", GetParam(), run.exitCode,
              run.err.c_str());
  EXPECT_EQ(run.exitCode, 8u);
  EXPECT_EQ(
      run.err.rfind(
          WideToUtf8Lossy(RefreshFolderRefusalMessage(RefreshRefusal::kElevated, context)), 0),
      0u)
      << run.err;
  EXPECT_EQ(run.out, "");
}

// Elevated, --refresh without a folder stops at the same refusal, before it looks anywhere.
TEST_P(Exes, RefreshDefaultPlacesAreRefusedElevated) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &context));
  if (const std::optional<std::string> why = WhyAnElevatedRefreshMayNotRunHere(context)) {
    GTEST_SKIP() << *why;
  }
  test::CapturedRun run;
  RUN_OR_SKIP(L"--refresh", run);
  std::printf("%ls --refresh (elevated) -> exit %lu: %s", GetParam(), run.exitCode,
              run.err.c_str());
  EXPECT_EQ(run.exitCode, 8u);
  EXPECT_EQ(run.out, "");
}

// --refresh --full restarts File Explorer: without --yes and without a console to ask on it is a
// usage error before anything is read, and so are --folder with --full and --yes without --full.
// Started only where the command would be refused anyway (WhyARefreshMayNotRunHere).
TEST_P(Exes, RefreshUsageErrors) {
  if (const std::optional<std::string> why = WhyARefreshMayNotRunHere()) {
    GTEST_SKIP() << *why;
  }
  struct Case {
    const wchar_t* arguments;
    const char* says;
  };
  for (const Case& c : {Case{L"--refresh --full", "needs --yes when there is no console"},
                        Case{L"--refresh --folder x --full", "--folder and --full cannot be"},
                        Case{L"--refresh --yes", "--yes goes with --full"}}) {
    test::CapturedRun run;
    RUN_OR_SKIP(c.arguments, run);
    std::printf("%ls %ls -> exit %lu: %s", GetParam(), c.arguments, run.exitCode, run.err.c_str());
    EXPECT_EQ(run.exitCode, 2u) << WideToUtf8Lossy(c.arguments);
    EXPECT_NE(run.err.find(c.says), std::string::npos) << run.err;
    EXPECT_NE(run.err.find("Run --help"), std::string::npos) << run.err;
    EXPECT_EQ(run.err.find("Cannot run here"), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

// Options that do not fit are usage errors before anything is read, anywhere.
TEST_P(Exes, InstallUninstallAndZoomUsageErrors) {
  for (const wchar_t* arguments : {L"--install --machine --user", L"--uninstall --force",
                                   L"--zoom 99", L"--install --zoom 250", L"--zoom --no-refresh"}) {
    test::CapturedRun run;
    RUN_OR_SKIP(arguments, run);
    EXPECT_EQ(run.exitCode, 2u) << WideToUtf8Lossy(arguments);
    EXPECT_NE(run.err.find("Run --help"), std::string::npos) << run.err;
    EXPECT_EQ(run.err.find("Cannot run here"), std::string::npos) << run.err;
    EXPECT_EQ(run.out, "");
  }
}

INSTANTIATE_TEST_SUITE_P(Executables, Exes, ::testing::Values(kCliExe, kSettingsExe),
                         [](const ::testing::TestParamInfo<const wchar_t*>& info) {
                           return std::string(info.param == kCliExe ? "Cli" : "Settings");
                         });

}  // namespace
}  // namespace pdft
