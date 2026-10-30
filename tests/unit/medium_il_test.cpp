// Medium-integrity support. Everything here also runs at High integrity (CI runs elevated):
// there it checks the real High -> Medium path; at Medium it checks that nothing relaunches.

#include <windows.h>

#include "medium_il.h"

#include <gtest/gtest.h>
#include <shellapi.h>

#include <string>
#include <vector>

namespace pdft::test {
namespace {

std::wstring Self() {
  wchar_t path[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::wstring(path, length);
}

std::wstring OwnFolder() {
  const std::wstring self = Self();
  return self.substr(0, self.find_last_of(L'\\'));
}

std::wstring RunMediumExe() { return OwnFolder() + L"\\pdft-run-medium.exe"; }

// This test program in a child mode (see main.cpp).
std::wstring ChildCommand(const wchar_t* mode) {
  return QuoteArgument(Self()) + L" --pdft-child " + mode;
}

struct Started {
  HRESULT hr = E_FAIL;  // starting the program
  DWORD code = 0;       // its exit code
};

// Runs |commandLine| normally (no token change).
Started RunPlain(const std::wstring& commandLine) {
  Started result;
  std::wstring line = commandLine;
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
                      &process)) {
    result.hr = HRESULT_FROM_WIN32(GetLastError());
    return result;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  GetExitCodeProcess(process.hProcess, &result.code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  result.hr = S_OK;
  return result;
}

struct OwnedHandle {
  HANDLE handle = nullptr;
  ~OwnedHandle() {
    if (handle) CloseHandle(handle);
  }
};

TEST(MediumIl, DecideRelaunch) {
  using enum RelaunchDecision;
  EXPECT_EQ(DecideRelaunch(SECURITY_MANDATORY_LOW_RID, false), kRunHere);
  EXPECT_EQ(DecideRelaunch(SECURITY_MANDATORY_MEDIUM_RID, false), kRunHere);
  EXPECT_EQ(DecideRelaunch(SECURITY_MANDATORY_MEDIUM_RID, true), kRunHere);
  EXPECT_EQ(DecideRelaunch(SECURITY_MANDATORY_MEDIUM_PLUS_RID, false), kRelaunch);
  EXPECT_EQ(DecideRelaunch(SECURITY_MANDATORY_HIGH_RID, false), kRelaunch);
  EXPECT_EQ(DecideRelaunch(SECURITY_MANDATORY_SYSTEM_RID, false), kRelaunch);
  EXPECT_EQ(DecideRelaunch(kUnknownIntegrity, false), kRelaunch);
  // A relaunched process that is still above Medium must not relaunch again.
  EXPECT_EQ(DecideRelaunch(SECURITY_MANDATORY_HIGH_RID, true), kRefuse);
  EXPECT_EQ(DecideRelaunch(kUnknownIntegrity, true), kRefuse);
}

TEST(MediumIl, QuoteArgumentRoundTrips) {
  EXPECT_EQ(QuoteArgument(L"plain"), L"plain");
  EXPECT_EQ(QuoteArgument(L""), L"\"\"");
  EXPECT_EQ(QuoteArgument(L"two words"), L"\"two words\"");
  EXPECT_EQ(QuoteArgument(L"C:\\a b\\"), L"\"C:\\a b\\\\\"");
  EXPECT_EQ(QuoteArgument(L"say \"hi\""), L"\"say \\\"hi\\\"\"");
  EXPECT_EQ(QuoteArgument(L"a\\\\b"), L"a\\\\b");

  const std::vector<std::wstring> args = {
      L"x.exe",     L"plain",
      L"",          L"two words",
      L"C:\\a b\\", L"tail\\\\",
      L"q\"uote",   L"\\\"lead",
      L"tab\there", L"\u03B4\u03BF\u03BA\u03B9\u03BC\u03AE-\u30C6\u30B9\u30C8 file.pdf"};
  std::wstring line;
  for (const auto& arg : args) line += (line.empty() ? L"" : L" ") + QuoteArgument(arg);
  int count = 0;
  LPWSTR* parsed = CommandLineToArgvW(line.c_str(), &count);
  ASSERT_NE(parsed, nullptr);
  ASSERT_EQ(count, static_cast<int>(args.size()));
  for (int i = 0; i < count; ++i) EXPECT_EQ(std::wstring(parsed[i]), args[i]) << i;
  LocalFree(parsed);
}

TEST(MediumIl, CurrentIntegrityIsReadable) {
  const DWORD rid = CurrentIntegrityRid();
  EXPECT_NE(rid, kUnknownIntegrity);
  EXPECT_GE(rid, SECURITY_MANDATORY_MEDIUM_RID) << "tests do not run at Low integrity";
  std::printf("[ info     ] this process runs at integrity 0x%04lX\n", rid);
}

TEST(MediumIl, LabelOnlyTokenIsMedium) {
  OwnedHandle token;
  ASSERT_EQ(CreateMediumTokenForSelf(MediumToken::kLabelOnly, &token.handle), S_OK);
  EXPECT_EQ(TokenIntegrityRid(token.handle), static_cast<DWORD>(SECURITY_MANDATORY_MEDIUM_RID));
  TOKEN_TYPE type = TokenImpersonation;
  DWORD size = 0;
  ASSERT_TRUE(GetTokenInformation(token.handle, TokenType, &type, sizeof(type), &size));
  EXPECT_EQ(type, TokenPrimary);
}

TEST(MediumIl, FilteredTokenIsMediumWithoutAdministrators) {
  OwnedHandle token;
  ASSERT_EQ(CreateMediumTokenForSelf(MediumToken::kFiltered, &token.handle), S_OK);
  EXPECT_EQ(TokenIntegrityRid(token.handle), static_cast<DWORD>(SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_FALSE(TokenHasEnabledAdministrators(token.handle));
}

TEST(MediumIl, NeverRaisesALowToken) {
  OwnedHandle self, low, medium;
  ASSERT_TRUE(OpenProcessToken(
      GetCurrentProcess(),
      TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &self.handle));
  ASSERT_TRUE(DuplicateTokenEx(
      self.handle, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY,
      nullptr, SecurityImpersonation, TokenPrimary, &low.handle));
  BYTE sid[SECURITY_MAX_SID_SIZE];
  DWORD sidSize = sizeof(sid);
  ASSERT_TRUE(CreateWellKnownSid(WinLowLabelSid, nullptr, sid, &sidSize));
  TOKEN_MANDATORY_LABEL label = {{sid, SE_GROUP_INTEGRITY}};
  ASSERT_TRUE(SetTokenInformation(low.handle, TokenIntegrityLevel, &label,
                                  sizeof(label) + GetLengthSid(sid)));
  ASSERT_EQ(TokenIntegrityRid(low.handle), static_cast<DWORD>(SECURITY_MANDATORY_LOW_RID));
  EXPECT_EQ(CreateMediumToken(low.handle, MediumToken::kLabelOnly, &medium.handle), E_INVALIDARG);
  EXPECT_EQ(medium.handle, nullptr);
  EXPECT_EQ(CreateMediumToken(low.handle, MediumToken::kLabelOnly, nullptr), E_POINTER);
}

// At Medium (a developer's prompt) the handler tests run in-process: no relaunch.
TEST(MediumIl, NoRelaunchWhenAlreadyMedium) {
  if (CurrentIntegrityRid() > SECURITY_MANDATORY_MEDIUM_RID) {
    GTEST_SKIP() << "running elevated: the relaunch itself is exercised by pdft_handler_tests";
  }
  int code = 12345;
  EXPECT_FALSE(RelaunchAtMediumIfElevated(&code));
  EXPECT_EQ(code, 12345);
}

TEST(MediumIl, RunsAChildWithTheLabelOnlyToken) {
  OwnedHandle token;
  ASSERT_EQ(CreateMediumTokenForSelf(MediumToken::kLabelOnly, &token.handle), S_OK);
  DWORD code = 0;
  ASSERT_EQ(RunWithToken(token.handle, Self(), ChildCommand(L"exit7"), &code), S_OK);
  EXPECT_EQ(code, 7u);
}

TEST(MediumIl, ChildOfTheFilteredTokenIsAStandardUser) {
  OwnedHandle token;
  ASSERT_EQ(CreateMediumTokenForSelf(MediumToken::kFiltered, &token.handle), S_OK);
  DWORD code = 0;
  ASSERT_EQ(RunWithToken(token.handle, std::wstring(), ChildCommand(L"check-token"), &code), S_OK);
  EXPECT_EQ(code, 0u) << "the child is not at Medium, or has Administrators enabled";
}

TEST(MediumIl, ApplicationControlErrors) {
  EXPECT_TRUE(
      IsApplicationControlBlock(HRESULT_FROM_WIN32(ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION)));
  EXPECT_TRUE(IsApplicationControlBlock(static_cast<HRESULT>(0x80070241)));  // invalid image hash
  EXPECT_TRUE(IsApplicationControlBlock(HRESULT_FROM_WIN32(4580)));
  EXPECT_FALSE(IsApplicationControlBlock(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)));
  EXPECT_FALSE(IsApplicationControlBlock(E_FAIL));
}

TEST(RunMediumTool, RunsTheProgramAtMediumAndReturnsItsExitCode) {
  const std::wstring tool = QuoteArgument(RunMediumExe());
  const Started first = RunPlain(tool + L" --exit 42");
  if (IsApplicationControlBlock(first.hr)) {
    GTEST_SKIP() << "pdft-run-medium.exe is blocked by Application Control (Smart App Control) "
                    "on this PC";
  }
  ASSERT_EQ(first.hr, S_OK);
  EXPECT_EQ(first.code, 42u);
  EXPECT_EQ(RunPlain(tool + L" -- " + tool + L" --exit 42").code, 42u);
  EXPECT_EQ(RunPlain(tool + L" " + tool + L" --check-token").code, 0u);
  EXPECT_EQ(RunPlain(tool + L" " + ChildCommand(L"check-token")).code, 0u);
  EXPECT_EQ(RunPlain(tool + L" --label-only -- " + tool + L" --exit 3").code, 3u);
  EXPECT_EQ(RunPlain(tool + L" -- " + QuoteArgument(OwnFolder() + L"\\no such program.exe")).code,
            125u);
  EXPECT_EQ(RunPlain(tool).code, 2u);
  EXPECT_EQ(RunPlain(tool + L" --exit").code, 2u);
  EXPECT_EQ(RunPlain(tool + L" --exit 1x").code, 2u);
}

}  // namespace
}  // namespace pdft::test
