// The pure rules of the local account the system tests make (tests/system/account_rules.h): when
// it may be made (every combination of the guard's facts), its name, and its password from random
// bytes. Nothing here makes, changes or looks up an account.

#include <windows.h>

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
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
