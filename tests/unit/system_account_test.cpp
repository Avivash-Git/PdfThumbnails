// The pure rules of the local accounts the system tests make (tests/system/account_rules.h): when
// they may be made (every combination of the guard's facts), their names, their passwords from
// random bytes, and how the standard account's run of the task tests is read from its output and
// judged. Nothing here makes, changes or looks up an account.

#include <windows.h>

#include <gtest/gtest.h>

#include <initializer_list>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "account_rules.h"

namespace pdft::systest {
namespace {

AccountGuard Allowed() {
  AccountGuard guard;
  guard.systemTests = L"1";
  guard.githubActions = L"true";
  guard.runnerEnvironment = L"github-hosted";
  guard.elevated = true;
  guard.realRegistry = true;
  guard.productFound = false;
  return guard;
}

TEST(TestAccountGuard, OnlyWhenEverythingSaysSo) {
  EXPECT_FALSE(WhyNoTestAccount(Allowed()));
  // Each of the seven facts right or wrong, in every combination: only all right allows it.
  for (int mask = 0; mask < 128; ++mask) {
    AccountGuard guard = Allowed();
    if (mask & 1) guard.systemTests = L"0";
    if (mask & 2) guard.githubActions = L"";
    if (mask & 4) guard.runnerEnvironment = L"self-hosted";
    if (mask & 8) guard.throwawayVm = L"1";  // the other opt-in: makes up for a missing first
    if (mask & 16) guard.elevated = false;
    if (mask & 32) guard.realRegistry = false;
    if (mask & 64) guard.productFound = true;
    const bool githubHosted = !(mask & 2) && !(mask & 4);
    const bool optIn = githubHosted || (mask & 8);
    const bool allowed = !(mask & 1) && optIn && !(mask & 16) && !(mask & 32) && !(mask & 64);
    EXPECT_EQ(!WhyNoTestAccount(guard).has_value(), allowed) << "mask " << mask;
  }
}

TEST(TestAccountGuard, ExactValues) {
  for (const wchar_t* value : {L"", L"0", L"true", L"1 ", L" 1", L"yes"}) {
    AccountGuard guard = Allowed();
    guard.systemTests = value;
    EXPECT_TRUE(WhyNoTestAccount(guard)) << "PDFT_SYSTEM_TESTS=" << ::testing::PrintToString(value);
  }
  for (const wchar_t* value : {L"", L"1", L"TRUE", L"True", L"true ", L"yes"}) {
    AccountGuard guard = Allowed();
    guard.githubActions = value;
    EXPECT_TRUE(WhyNoTestAccount(guard)) << "GITHUB_ACTIONS=" << ::testing::PrintToString(value);
    guard.throwawayVm = L"1";
    EXPECT_FALSE(WhyNoTestAccount(guard)) << "with the virtual machine's opt-in";
    for (const wchar_t* vm : {L"", L"true", L"yes", L"01", L"1 "}) {
      guard.throwawayVm = vm;
      EXPECT_TRUE(WhyNoTestAccount(guard))
          << "PDFT_SYSTEM_TESTS_THROWAWAY_VM=" << ::testing::PrintToString(vm);
    }
  }
  // A self-hosted runner, or a tool that runs the workflow on someone's own PC, also sets
  // GITHUB_ACTIONS=true: only a runner GitHub hosts counts.
  for (const wchar_t* value :
       {L"", L"self-hosted", L"GitHub-Hosted", L"github-hosted ", L"github", L"1"}) {
    AccountGuard guard = Allowed();
    guard.runnerEnvironment = value;
    EXPECT_TRUE(WhyNoTestAccount(guard))
        << "RUNNER_ENVIRONMENT=" << ::testing::PrintToString(value);
    guard.throwawayVm = L"1";
    EXPECT_FALSE(WhyNoTestAccount(guard)) << "with the virtual machine's opt-in";
  }
}

TEST(TestAccountGuard, TheEnvironmentAlone) {
  EXPECT_FALSE(WhyNotAThrowawayPc(L"1", L"true", L"github-hosted", L""));
  EXPECT_FALSE(WhyNotAThrowawayPc(L"1", L"", L"", L"1"));
  EXPECT_FALSE(WhyNotAThrowawayPc(L"1", L"true", L"self-hosted", L"1"));
  EXPECT_FALSE(WhyNotAThrowawayPc(L"1", L"true", L"github-hosted", L"1"));
  EXPECT_TRUE(WhyNotAThrowawayPc(L"1", L"true", L"", L""));
  EXPECT_TRUE(WhyNotAThrowawayPc(L"1", L"true", L"self-hosted", L""));
  EXPECT_TRUE(WhyNotAThrowawayPc(L"1", L"", L"github-hosted", L""));
  const std::optional<std::string> none = WhyNotAThrowawayPc(L"", L"true", L"github-hosted", L"1");
  ASSERT_TRUE(none);
  EXPECT_NE(none->find("PDFT_SYSTEM_TESTS"), std::string::npos);
  const std::optional<std::string> neither = WhyNotAThrowawayPc(L"1", L"true", L"self-hosted", L"");
  ASSERT_TRUE(neither);
  EXPECT_NE(neither->find("GITHUB_ACTIONS=true"), std::string::npos);
  EXPECT_NE(neither->find("RUNNER_ENVIRONMENT=github-hosted"), std::string::npos);
  EXPECT_NE(neither->find("PDFT_SYSTEM_TESTS_THROWAWAY_VM=1"), std::string::npos);
}

TEST(TestAccountName, FromThreeBytes) {
  EXPECT_EQ(TestAccountName({0x00, 0xAB, 0xFF}), L"pdft-sys-00abff");
  EXPECT_EQ(TestAccountName({0x12, 0x34, 0x56}), L"pdft-sys-123456");
  const std::wstring name = TestAccountName({0xFF, 0xFF, 0xFF});
  EXPECT_LE(name.size(), 20u);  // a local account's name has at most 20 characters
  EXPECT_EQ(name.find_first_of(L"\"/\\[]:;|=,+*?<>@ "), std::wstring::npos);
}

// Only an account with a name TestAccountName gives and the tests' own comment is one the next
// run removes.
TEST(TestAccountName, EarlierRunsAccounts) {
  EXPECT_TRUE(IsTestAccount(TestAccountName({0x00, 0xAB, 0xFF}), kTestAccountComment));
  EXPECT_TRUE(IsTestAccount(L"pdft-sys-123456", kTestAccountComment));
  for (const wchar_t* name :
       {L"", L"pdft-sys-", L"pdft-sys-12345", L"pdft-sys-1234567", L"pdft-sys-12345G",
        L"pdft-sys-00ABFF", L"PDFT-SYS-00abff", L"pdft-sys-00abf ", L"xpdft-sys-00abf",
        L"Administrator", L"runneradmin"}) {
    EXPECT_FALSE(IsTestAccount(name, kTestAccountComment)) << ::testing::PrintToString(name);
  }
  for (const wchar_t* comment : {L"", L"PdfThumbnails system tests",
                                 L"pdfthumbnails system tests: deleted at the end of "
                                 L"the run"}) {
    EXPECT_FALSE(IsTestAccount(L"pdft-sys-123456", comment)) << ::testing::PrintToString(comment);
  }
  std::wstring longer = kTestAccountComment;
  longer += L" ";
  EXPECT_FALSE(IsTestAccount(L"pdft-sys-123456", longer));
}

std::vector<BYTE> Bytes(std::initializer_list<int> values) {
  std::vector<BYTE> bytes;
  for (const int value : values) bytes.push_back(static_cast<BYTE>(value));
  return bytes;
}

constexpr wchar_t kName[] = L"pdft-sys-00abff";

TEST(TestAccountPassword, OneCharacterPerUsableByte) {
  // "A", "a", "0", "!" (the first of each kind), then "B" for the rest.
  std::vector<BYTE> bytes = Bytes({0, 26, 52, 62});
  bytes.resize(kPasswordLength, 1);
  const std::optional<std::wstring> password = PasswordFromRandom(bytes, kName);
  ASSERT_TRUE(password);
  EXPECT_EQ(*password, L"Aa0!" + std::wstring(kPasswordLength - 4, L'B'));

  // Bytes from 228 up are skipped (228 is 3 times the 76 characters); 227 is the last symbol.
  std::vector<BYTE> skipped = Bytes({228, 255, 0, 26, 240, 52, 62, 227});
  skipped.resize(kPasswordLength + 4, 1);
  const std::optional<std::wstring> second = PasswordFromRandom(skipped, kName);
  ASSERT_TRUE(second);
  EXPECT_EQ(*second, L"Aa0!~" + std::wstring(kPasswordLength - 5, L'B'));
}

TEST(TestAccountPassword, EveryKindOrNone) {
  std::vector<BYTE> letters(kPasswordLength * 2);
  for (size_t i = 0; i < letters.size(); ++i) letters[i] = static_cast<BYTE>(i % 52);
  EXPECT_FALSE(PasswordFromRandom(letters, kName));  // no digit, no symbol
  std::vector<BYTE> same(256, 5);
  EXPECT_FALSE(PasswordFromRandom(same, kName));
  // Too few usable bytes.
  std::vector<BYTE> few = Bytes({0, 26, 52, 62});
  few.resize(kPasswordLength - 1, 1);
  EXPECT_FALSE(PasswordFromRandom(few, kName));
  few.resize(kPasswordLength + 20, 250);
  EXPECT_FALSE(PasswordFromRandom(few, kName));
  EXPECT_FALSE(PasswordFromRandom({}, kName));
}

// Windows' complexity rule refuses a password that holds the account's name, or a part of it of
// three or more characters, ignoring case: such a password is not given.
TEST(TestAccountPassword, NothingOfTheName) {
  // "Aa0!" and then |part| (indexes into the alphabet), "B" for the rest.
  const auto with = [](std::initializer_list<int> part) {
    std::vector<BYTE> bytes = Bytes({0, 26, 52, 62});
    for (const int index : part) bytes.push_back(static_cast<BYTE>(index));
    bytes.resize(kPasswordLength, 1);
    return bytes;
  };
  const std::vector<BYTE> sys = with({18, 24, 18});               // "SYS"
  const std::vector<BYTE> lowerSys = with({44, 50, 44});          // "sys"
  const std::vector<BYTE> pdft = with({41, 3, 5, 45});            // "pDFt"
  const std::vector<BYTE> hex = with({52, 52, 0, 27, 5, 31});     // "00AbFf"
  const std::vector<BYTE> twoOnly = with({18, 24, 1, 15, 3, 1});  // "SYBPDB": no part of 3
  for (const std::vector<BYTE>* bytes : {&sys, &lowerSys, &pdft, &hex}) {
    EXPECT_FALSE(PasswordFromRandom(*bytes, kName));
    // Fine for an account whose name holds none of it.
    EXPECT_TRUE(PasswordFromRandom(*bytes, L"someone-else"));
  }
  EXPECT_TRUE(PasswordFromRandom(twoOnly, kName));
  EXPECT_TRUE(PasswordFromRandom(hex, L"pdft-sys-123456"));
  EXPECT_FALSE(PasswordFromRandom(sys, L"sys"));
  EXPECT_TRUE(PasswordFromRandom(sys, L"sy"));  // too short to count
}

TEST(TestAccountPassword, OnlyItsAlphabet) {
  std::vector<BYTE> all(256);
  for (size_t i = 0; i < all.size(); ++i) all[i] = static_cast<BYTE>((i * 7 + 3) % 256);
  const std::optional<std::wstring> password = PasswordFromRandom(all, kName);
  ASSERT_TRUE(password);
  EXPECT_EQ(password->size(), kPasswordLength);
  for (const wchar_t c : *password) {
    const bool letter = (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
    const bool digit = c >= L'0' && c <= L'9';
    const bool symbol = std::wstring_view(L"!#%*+-.:=?@^_~").find(c) != std::wstring_view::npos;
    EXPECT_TRUE(letter || digit || symbol) << static_cast<int>(c);
  }
}

// GoogleTest's console output as MSVC builds print it, CR LF line ends, the lists at the end.
constexpr char kTaskRunOutput[] =
    "Note: Google Test filter = TaskWriting.*:TaskFolderElevated.*\r\n"
    "[==========] Running 4 tests from 2 test suites.\r\n"
    "[----------] 3 tests from TaskWriting\r\n"
    "[ RUN      ] TaskWriting.First\r\n"
    "[ tasks ] something the test printed\r\n"
    "[       OK ] TaskWriting.First (12 ms)\r\n"
    "[ RUN      ] TaskWriting.Second\r\n"
    "C:\\src\\tasks_test.cpp(10): error: Value of: ok\r\n"
    "[  FAILED  ] TaskWriting.Second (3 ms)\r\n"
    "[ RUN      ] TaskWriting.TaskSchedulerRunsTheTask\r\n"
    "C:\\src\\tasks_test.cpp(985): Skipped\r\n"
    "no interactive desktop here: Task Scheduler runs interactive-token tasks only in the user's "
    "session\r\n"
    "\r\n"
    "[  SKIPPED ] TaskWriting.TaskSchedulerRunsTheTask (0 ms)\r\n"
    "[ RUN      ] TaskFolderElevated.Third\r\n"
    "C:\\src\\tasks_test.cpp(1064): Skipped\r\n"
    "\r\n"
    "[  SKIPPED ] TaskFolderElevated.Third (1 ms)\r\n"
    "[ RUN      ] TaskFolderElevated.Crashed\r\n"
    "[==========] 4 tests from 2 test suites ran. (16 ms total)\r\n"
    "[  PASSED  ] 1 test.\r\n"
    "[  SKIPPED ] 2 tests, listed below:\r\n"
    "[  SKIPPED ] TaskWriting.TaskSchedulerRunsTheTask\r\n"
    "[  SKIPPED ] TaskFolderElevated.Third\r\n"
    "[  FAILED  ] 1 test, listed below:\r\n"
    "[  FAILED  ] TaskWriting.Second\r\n"
    "[  FAILED  ] TaskFolderElevated.Crashed\r\n";

TEST(StandardAccountTaskTests, EachTestWithItsOwnResult) {
  const std::vector<GTestCase> cases = ReadGTestCases(kTaskRunOutput);
  ASSERT_EQ(cases.size(), 5u);
  EXPECT_EQ(cases[0].name, "TaskWriting.First");
  EXPECT_EQ(cases[0].result, GTestCase::Result::kPassed);
  EXPECT_EQ(cases[1].name, "TaskWriting.Second");
  EXPECT_EQ(cases[1].result, GTestCase::Result::kFailed);
  EXPECT_EQ(cases[2].name, "TaskWriting.TaskSchedulerRunsTheTask");
  EXPECT_EQ(cases[2].result, GTestCase::Result::kSkipped);
  EXPECT_EQ(cases[2].skipReason,
            "no interactive desktop here: Task Scheduler runs interactive-token tasks only in the "
            "user's session");
  // A skip without a message.
  EXPECT_EQ(cases[3].result, GTestCase::Result::kSkipped);
  EXPECT_EQ(cases[3].skipReason, "");
  // The list at the end does not finish a test that never ended.
  EXPECT_EQ(cases[4].name, "TaskFolderElevated.Crashed");
  EXPECT_EQ(cases[4].result, GTestCase::Result::kUnfinished);
  EXPECT_TRUE(ReadGTestCases("").empty());
  EXPECT_TRUE(ReadGTestCases("[  SKIPPED ] TaskWriting.First (1 ms)\n").empty());
  // Without line ends at the end, and with LF alone.
  const std::vector<GTestCase> last = ReadGTestCases(
      "[ RUN      ] A.B\n[       OK ] A.B (0 ms)\n[ RUN      ] A.C\n[  FAILED  ] "
      "A.C (0 ms)");
  ASSERT_EQ(last.size(), 2u);
  EXPECT_EQ(last[0].result, GTestCase::Result::kPassed);
  EXPECT_EQ(last[1].result, GTestCase::Result::kFailed);
}

GTestCase Case(std::string name, GTestCase::Result result, std::string reason = {}) {
  GTestCase test;
  test.name = std::move(name);
  test.result = result;
  test.skipReason = std::move(reason);
  return test;
}

// What a standard user's run gives where nothing is wrong.
std::vector<GTestCase> AsExpected() {
  std::vector<GTestCase> cases;
  for (const char* required : kStandardAccountRequiredTests) {
    cases.push_back(Case(required, GTestCase::Result::kPassed));
  }
  cases.push_back(
      Case("TaskWriting.EachCheckboxCombinationWithoutTheFolder", GTestCase::Result::kPassed));
  cases.push_back(
      Case(kSessionTaskTest, GTestCase::Result::kSkipped,
           "Task Scheduler did not start the interactive-token task here (state 3, 0x00041303)"));
  cases.push_back(Case("TaskFolderElevated.EnsureTaskFolderMakesTheProtectedFolder",
                       GTestCase::Result::kSkipped,
                       "needs elevation (the shared folder is made and removed by elevated code "
                       "only); runs on CI"));
  return cases;
}

TEST(StandardAccountTaskTests, WhatAStandardUsersRunMustShow) {
  EXPECT_EQ(StandardAccountTaskTestProblems(AsExpected(), 0), std::vector<std::string>{});
  // The session test may also pass, or skip for each reason that means no session of its own.
  for (const char* reason : kNoSessionSkips) {
    std::vector<GTestCase> cases = AsExpected();
    cases[4].skipReason = std::string(reason) + " (0x800710E0)";
    EXPECT_TRUE(StandardAccountTaskTestProblems(cases, 0).empty()) << reason;
  }
  std::vector<GTestCase> ran = AsExpected();
  ran[4].result = GTestCase::Result::kPassed;
  ran[4].skipReason.clear();
  EXPECT_TRUE(StandardAccountTaskTestProblems(ran, 0).empty());

  const auto problems = [](void (*change)(std::vector<GTestCase>*), DWORD exitCode = 0) {
    std::vector<GTestCase> cases = AsExpected();
    change(&cases);
    return StandardAccountTaskTestProblems(cases, exitCode);
  };
  const auto nothing = [](std::vector<GTestCase>*) {};
  EXPECT_EQ(problems(nothing, 1).size(), 1u);
  // Any other skip of a TaskWriting test, the session test's for another reason among them.
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) {
              (*cases)[4].skipReason =
                  "Application Control blocked pdft-run-medium.exe (0x800711C7)";
            }).size(),
            1u);
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) {
              (*cases)[3].result = GTestCase::Result::kSkipped;
              (*cases)[3].skipReason = "no interactive desktop here";
            }).size(),
            1u);
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) {
              (*cases)[3].result = GTestCase::Result::kFailed;
            }).size(),
            1u);
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) {
              (*cases)[3].result = GTestCase::Result::kUnfinished;
            }).size(),
            1u);
  // The protected folder's tests must skip, for needing elevation.
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) {
              (*cases)[5].result = GTestCase::Result::kPassed;
            }).size(),
            1u);
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) {
              (*cases)[5].skipReason = "Application Control blocked the test program";
            }).size(),
            1u);
  // Both suites, only them, and each test that shows what a standard user may not do.
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) { cases->pop_back(); }).size(), 1u);
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) {
              cases->push_back(Case("Other.Test", GTestCase::Result::kPassed));
            }).size(),
            1u);
  EXPECT_EQ(problems([](std::vector<GTestCase>* cases) { cases->erase(cases->begin()); }).size(),
            1u);
  const std::vector<std::string> none = StandardAccountTaskTestProblems({}, 0);
  EXPECT_EQ(none.size(), 2u + std::size(kStandardAccountRequiredTests));
}

TEST(PendingRenamePath, WithAndWithoutTheMarkNewerWindowsAdds) {
  EXPECT_EQ(PendingRenamePath(L"\\??\\C:\\Program Files\\x"), L"C:\\Program Files\\x");
  EXPECT_EQ(PendingRenamePath(L"*1\\??\\C:\\Program Files\\x"), L"C:\\Program Files\\x");
  EXPECT_EQ(PendingRenamePath(L"*12\\??\\C:\\x"), L"C:\\x");
  EXPECT_EQ(PendingRenamePath(L"*\\??\\C:\\x"), L"C:\\x");
  EXPECT_EQ(PendingRenamePath(L"C:\\x"), L"C:\\x");
  EXPECT_EQ(PendingRenamePath(L""), L"");
}

}  // namespace
}  // namespace pdft::systest
