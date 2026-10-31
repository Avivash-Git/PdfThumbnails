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

#include <cstdio>
#include <optional>
#include <set>
#include <string>

#include "child_process.h"
#include "cli_args.h"
#include "commands.h"
#include "context.h"
#include "diagnostics.h"
#include "instance.h"
#include "json_check.h"
#include "medium_il.h"
#include "project_config.h"
#include "strings.h"

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

INSTANTIATE_TEST_SUITE_P(Executables, Exes, ::testing::Values(kCliExe, kSettingsExe),
                         [](const ::testing::TestParamInfo<const wchar_t*>& info) {
                           return std::string(info.param == kCliExe ? "Cli" : "Settings");
                         });

}  // namespace
}  // namespace pdft
