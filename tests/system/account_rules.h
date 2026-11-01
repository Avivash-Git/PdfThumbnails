// The rules of the local account the system tests make for themselves (test_account.h), apart
// from the code that makes it, so that the unit tests can check them on any PC: when the account
// may be made at all, its name and its password, each from what it is given (no system access).

#pragma once

#include <windows.h>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pdft::systest {

// Besides PDFT_SYSTEM_TESTS=1, the tests need a second opt-in that says this PC is thrown away
// after the run, because they make, use and delete a local administrator account on it: on a
// runner GitHub hosts, GitHub Actions sets GITHUB_ACTIONS=true and
// RUNNER_ENVIRONMENT=github-hosted (GITHUB_ACTIONS alone is also set on a self-hosted runner,
// which may be someone's own PC, and by tools that run a workflow locally); a maintainer's own
// throw-away virtual machine says the same with PDFT_SYSTEM_TESTS_THROWAWAY_VM=1.
inline constexpr wchar_t kGitHubActionsEnv[] = L"GITHUB_ACTIONS";
inline constexpr wchar_t kRunnerEnvironmentEnv[] = L"RUNNER_ENVIRONMENT";
inline constexpr wchar_t kThrowawayVmEnv[] = L"PDFT_SYSTEM_TESTS_THROWAWAY_VM";

// Why the environment alone rules this PC out (pure): PDFT_SYSTEM_TESTS (|systemTests|) is not
// "1", or neither GITHUB_ACTIONS (|githubActions|) is "true" with RUNNER_ENVIRONMENT
// (|runnerEnvironment|) "github-hosted" nor PDFT_SYSTEM_TESTS_THROWAWAY_VM (|throwawayVm|) is
// "1". std::nullopt when both opt-ins are there. May throw std::bad_alloc.
std::optional<std::string> WhyNotAThrowawayPc(std::wstring_view systemTests,
                                              std::wstring_view githubActions,
                                              std::wstring_view runnerEnvironment,
                                              std::wstring_view throwawayVm);

// What the guard of the test account looks at.
struct AccountGuard {
  std::wstring systemTests;        // the value of PDFT_SYSTEM_TESTS
  std::wstring githubActions;      // GITHUB_ACTIONS
  std::wstring runnerEnvironment;  // RUNNER_ENVIRONMENT
  std::wstring throwawayVm;        // PDFT_SYSTEM_TESTS_THROWAWAY_VM
  bool elevated = false;           // this process's token (TokenElevation)
  bool realRegistry = false;  // writes reach the real hive: not virtualized, no packaged parent
  bool productFound = true;   // something of 1.x or 2.x is on this PC
};

// Why the test account must not be made here (pure): WhyNotAThrowawayPc, then not elevated, a
// registry that may be virtualized, 1.x or 2.x installed. std::nullopt only when none of these
// holds. May throw std::bad_alloc.
std::optional<std::string> WhyNoTestAccount(const AccountGuard& guard);

// The account's name (pure): "pdft-sys-" and the bytes of |random| as six lower-case hex digits,
// well within the 20 characters a local account's name may have. May throw std::bad_alloc.
std::wstring TestAccountName(const std::array<BYTE, 3>& random);

// The comment every test account is made with: together with a name TestAccountName could have
// given, it marks an account the system tests made.
inline constexpr wchar_t kTestAccountComment[] =
    L"PdfThumbnails system tests: deleted at the end of the run";

// Whether the account |name| with the comment |comment| is one the system tests made (pure): a
// name TestAccountName could give ("pdft-sys-" and six lower-case hex digits) and exactly
// kTestAccountComment. A run that was ended from outside (a timeout, a cancelled job) cannot
// delete its account; the next run removes such accounts before it makes its own.
bool IsTestAccount(std::wstring_view name, std::wstring_view comment) noexcept;

// How many characters the account's password has.
inline constexpr size_t kPasswordLength = 32;

// A password of kPasswordLength characters from |random| (bytes of a cryptographic generator;
// pure) for the account |accountName|: each byte picks one character of upper-case and
// lower-case letters, digits and symbols, and a byte that would make some characters likelier
// than others is skipped. std::nullopt when |random| runs out first, one of the four kinds is
// missing, or the password holds, ignoring case, the account's name or a part of it of three or
// more characters between its dashes, which Windows' password complexity rule may refuse (the
// caller tries again with new bytes); what was built by then is wiped. May throw
// std::bad_alloc.
std::optional<std::wstring> PasswordFromRandom(std::span<const BYTE> random,
                                               std::wstring_view accountName);

}  // namespace pdft::systest
