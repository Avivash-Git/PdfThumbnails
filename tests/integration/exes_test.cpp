// Both executables, started the way a script starts them: standard output and error on pipes,
// the exit code checked. They sit next to this test program (build folder and test bundle).
//
// Freshly built, unsigned executables can be refused by Smart App Control or another
// Application Control policy. Such a refusal is a skip, never a pass or a failure, and an
// executable that was refused once is not started again in this run.

#include <windows.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <optional>
#include <set>
#include <string>

#include "child_process.h"
#include "cli_args.h"
#include "commands.h"
#include "context.h"
#include "instance.h"
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

INSTANTIATE_TEST_SUITE_P(Executables, Exes, ::testing::Values(kCliExe, kSettingsExe),
                         [](const ::testing::TestParamInfo<const wchar_t*>& info) {
                           return std::string(info.param == kCliExe ? "Cli" : "Settings");
                         });

}  // namespace
}  // namespace pdft
