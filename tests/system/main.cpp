// pdft_system_tests: installs, uses and uninstalls the app for real (system_test.cpp), as a local
// account of its own (test_account.h). Before anything else it makes sure it may: the
// environment asks for it (PDFT_SYSTEM_TESTS=1) and says the PC is thrown away afterwards
// (GITHUB_ACTIONS=true with RUNNER_ENVIRONMENT=github-hosted on a runner GitHub hosts, or
// PDFT_SYSTEM_TESTS_THROWAWAY_VM=1), the process is elevated, its registry writes reach the real
// hive, and nothing of 1.x or 2.x is on this PC, which the tests would change or remove.
// Otherwise it changes nothing and exits 77 (CTest's "skipped"), or 3 where the product is
// installed. Only then does it make the accounts (after removing any an earlier run that was
// ended from outside left): the test account, and then the standard account; it removes them
// again however the tests end. They stop before the first test when the test account's programs
// could not make named objects in this session (TestAccount::namedObjects). Without a usable
// standard account the tests still run, and those that need it fail.

#include <windows.h>

#include <gtest/gtest.h>
#include <objbase.h>
#include <wil/resource.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "account_rules.h"
#include "child_process.h"
#include "com_security.h"
#include "elevation.h"
#include "file_ops.h"
#include "strings.h"
#include "system_env.h"
#include "test_account.h"

namespace {

using std::filesystem::path;
using namespace pdft::systest;

std::string Text(std::wstring_view text) { return pdft::WideToUtf8Lossy(text); }

// The account's own programs run with the token of its logon: pdft-run-medium --check-token,
// copied where the account can read it, prints the token it runs with, and its line must hold
// |expected| ("elevation type 3" for the test account's limited token). False, with a line that
// says why (|otherwise| when the token is not the one expected), otherwise.
bool CheckTheAccountsToken(const TestAccount& account, const char* expected,
                           const char* otherwise) {
  const path helper = path(pdft::test::SelfFolder()) / L"pdft-run-medium.exe";
  const path tools = account.tempDir() / L"pdft-system-tools";
  const path copy = tools / helper.filename();
  std::error_code error;
  std::filesystem::create_directories(tools, error);
  if (error || !CopyFileW(helper.c_str(), copy.c_str(), FALSE)) {
    std::printf("[ system ] check-token as the %s: cannot copy %s there\n", account.noun(),
                Text(helper.native()).c_str());
    return false;
  }
  const Outcome run = account.Run(copy, L"--check-token", 2 * 60 * 1000);
  (void)pdft::RemoveTreeNoFollow(tools);
  std::string first = run.out.substr(0, run.out.find('\n'));
  if (!first.empty() && first.back() == '\r') first.pop_back();
  if (FAILED(run.hr)) {
    std::printf("[ system ] check-token as the %s: not run: %s\n", account.noun(),
                Text(pdft::HResultText(run.hr)).c_str());
    return false;
  }
  std::printf("[ system ] check-token as the %s: %s (exit 0x%08lX)\n", account.noun(),
              first.c_str(), run.exitCode);
  if (first.empty()) {
    std::printf(
        "[ system ] the %s's program printed nothing: it did not start properly (no access to "
        "the window station or desktop gives 0xC0000142)\n",
        account.noun());
    return false;
  }
  if (first.find(expected) == std::string::npos) {
    std::printf("[ system ] %s\n", otherwise);
    return false;
  }
  return true;
}

// Makes the standard account and checks that it is what its tests need: a standard user's token
// (TestAccount::Create checks it), with which its programs start and may use this session's named
// objects. nullptr, with a line that says why, otherwise (what was made is removed again).
std::unique_ptr<TestAccount> MakeTheStandardAccount() {
  std::string error;
  std::unique_ptr<TestAccount> standard = TestAccount::Create(TestAccount::Kind::kStandard, &error);
  if (!standard) {
    std::printf("[ system ] the standard account could not be made: %s\n", error.c_str());
    return nullptr;
  }
  std::printf(
      "[ system ] the standard account: %s (%s), not a member of Administrators, with the token "
      "of its interactive logon\n",
      Text(standard->name()).c_str(), Text(standard->sid()).c_str());
  std::printf("[ system ] standard account's %s\n",
              Text(pdft::TokenFactsText(standard->tokenFacts())).c_str());
  std::fflush(stdout);
  if (const HRESULT named = standard->namedObjects(); FAILED(named)) {
    std::printf(
        "[ system ] the standard account is not used: it may not use this session's named "
        "objects or make \"Global\\\" names, and that could not be granted to it (%s)\n",
        Text(pdft::HResultText(named)).c_str());
    return nullptr;
  }
  if (!CheckTheAccountsToken(*standard, "elevation type 1, elevated no",
                             "the standard account's programs do not run with a standard "
                             "user's token (elevation type 1, not elevated)")) {
    std::printf("[ system ] the standard account is not used\n");
    return nullptr;
  }
  return standard;
}

// Makes the test account, checks that it is what the tests need, makes the standard account,
// runs the tests as them, and removes them again however they end (TestAccount's destructor).
int RunAsTheTestAccount() {
  std::string error;
  const std::unique_ptr<TestAccount> account =
      TestAccount::Create(TestAccount::Kind::kAdministrator, &error);
  if (!account) {
    std::printf("[ system ] the test account could not be made: %s\n", error.c_str());
    return 1;
  }
  SetCurrentTestAccount(account.get());
  const auto forget = wil::scope_exit([] { SetCurrentTestAccount(nullptr); });
  std::printf(
      "[ system ] the user: the test account %s (%s), a member of Administrators with the limited "
      "token of its interactive logon; the administrator: this process, elevated\n",
      Text(account->name()).c_str(), Text(account->sid()).c_str());
  std::printf("[ system ] test account's %s\n",
              Text(pdft::TokenFactsText(account->tokenFacts())).c_str());
  std::printf("[ system ] its linked %s\n",
              Text(pdft::TokenFactsText(account->linkedFacts())).c_str());
  std::fflush(stdout);
  if (const HRESULT named = account->namedObjects(); FAILED(named)) {
    std::printf(
        "[ system ] the tests stop: the test account may not use this session's named objects "
        "or make \"Global\\\" names, and that could not be granted to it (%s); run as it, every "
        "command of the app would fail to make its operation lock (the lines above show the "
        "directory's permissions and which step was refused)\n",
        Text(pdft::HResultText(named)).c_str());
    return 1;
  }
  if (!CheckTheAccountsToken(*account, "elevation type 3",
                             "the test account's programs do not run with its limited token "
                             "(elevation type 3), so UAC would not elevate them")) {
    return 1;
  }
  // Made after the test account, so it is removed before it.
  const std::unique_ptr<TestAccount> standard = MakeTheStandardAccount();
  SetCurrentStandardAccount(standard.get());
  const auto forgetStandard = wil::scope_exit([] { SetCurrentStandardAccount(nullptr); });
  std::fflush(stdout);
  return RUN_ALL_TESTS();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  // Checked first, before COM or anything else that touches the system.
  if (const std::optional<std::string> why = WhyNotAThrowawayPc(
          EnvironmentValue(kEnableEnv), EnvironmentValue(kGitHubActionsEnv),
          EnvironmentValue(kRunnerEnvironmentEnv), EnvironmentValue(kThrowawayVmEnv))) {
    std::printf("pdft_system_tests: not run: %s\n", why->c_str());
    return kSkipCode;
  }
  const HRESULT com = pdft::InitializeComSecurity();
  if (FAILED(com)) {
    std::printf("InitializeComSecurity failed: 0x%08lX\n", static_cast<unsigned long>(com));
    return 1;
  }
  int result = kSkipCode;
  if (const std::optional<std::string> why = WhyNotHere()) {
    std::printf("pdft_system_tests: not run: %s\n", why->c_str());
  } else if (const std::vector<std::string> found = Leftovers(); !found.empty()) {
    std::printf(
        "pdft_system_tests: refused: this PC has 1.x or 2.x on it, which these tests "
        "would change or remove:\n");
    for (const std::string& line : found) std::printf("  %s\n", line.c_str());
    result = kRefusedCode;
  } else if (const std::optional<std::string> account = WhyNoTestAccountHere()) {
    // Every condition of the account's guard at once (TestAccount::Create asks again).
    std::printf("pdft_system_tests: not run: %s\n", account->c_str());
  } else {
    ::testing::InitGoogleTest(&argc, argv);
    result = RunAsTheTestAccount();
  }
  CoUninitialize();
  return result;
}
