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

// A SID in a buffer of its own.
using Sid = std::vector<BYTE>;

Sid WellKnown(WELL_KNOWN_SID_TYPE type) {
  Sid sid(SECURITY_MAX_SID_SIZE);
  DWORD size = SECURITY_MAX_SID_SIZE;
  if (!CreateWellKnownSid(type, nullptr, sid.data(), &size)) sid.clear();
  return sid;
}

// Token information that is or starts with a SID: TokenUser or TokenOwner.
Sid TokenSid(HANDLE token, TOKEN_INFORMATION_CLASS what) {
  alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  if (!GetTokenInformation(token, what, buffer, sizeof(buffer), &size)) return Sid();
  PSID sid = what == TokenUser ? reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid
                               : reinterpret_cast<TOKEN_OWNER*>(buffer)->Owner;
  const BYTE* bytes = static_cast<const BYTE*>(sid);
  return Sid(bytes, bytes + GetLengthSid(sid));
}

// The rights |token|'s default DACL allows |sid| (0 when it has no access-allowed ACE for it).
ACCESS_MASK DefaultDaclAllows(HANDLE token, const Sid& sid) {
  DWORD size = 0;
  GetTokenInformation(token, TokenDefaultDacl, nullptr, 0, &size);
  std::vector<BYTE> buffer(size);
  if (!size || !GetTokenInformation(token, TokenDefaultDacl, buffer.data(), size, &size)) return 0;
  const PACL dacl = reinterpret_cast<TOKEN_DEFAULT_DACL*>(buffer.data())->DefaultDacl;
  ACCESS_MASK mask = 0;
  for (DWORD i = 0; dacl && i < dacl->AceCount; ++i) {
    void* ace = nullptr;
    if (GetAce(dacl, i, &ace) &&
        static_cast<ACE_HEADER*>(ace)->AceType == ACCESS_ALLOWED_ACE_TYPE &&
        EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, const_cast<BYTE*>(sid.data()))) {
      mask |= static_cast<ACCESS_ALLOWED_ACE*>(ace)->Mask;
    }
  }
  return mask;
}

// Gives |token| another default DACL until destroyed.
class ScopedDefaultDacl {
 public:
  ScopedDefaultDacl(HANDLE token, PACL dacl) : token_(token) {
    DWORD size = 0;
    GetTokenInformation(token, TokenDefaultDacl, nullptr, 0, &size);
    saved_.resize(size);
    TOKEN_DEFAULT_DACL value = {dacl};
    set_ = size && GetTokenInformation(token, TokenDefaultDacl, saved_.data(), size, &size) &&
           SetTokenInformation(token, TokenDefaultDacl, &value, sizeof(value));
  }
  ScopedDefaultDacl(const ScopedDefaultDacl&) = delete;
  ScopedDefaultDacl& operator=(const ScopedDefaultDacl&) = delete;
  ~ScopedDefaultDacl() {
    if (set_) {
      SetTokenInformation(token_, TokenDefaultDacl, saved_.data(), sizeof(TOKEN_DEFAULT_DACL));
    }
  }
  bool ok() const { return set_; }

 private:
  HANDLE token_;
  std::vector<BYTE> saved_;  // TOKEN_DEFAULT_DACL followed by the ACL it points to
  bool set_ = false;
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
  EXPECT_EQ(DefaultDaclAllows(token.handle, TokenSid(token.handle, TokenUser)),
            static_cast<ACCESS_MASK>(GENERIC_ALL));
}

TEST(MediumIl, FilteredTokenIsMediumWithoutAdministrators) {
  OwnedHandle token;
  ASSERT_EQ(CreateMediumTokenForSelf(MediumToken::kFiltered, &token.handle), S_OK);
  EXPECT_EQ(TokenIntegrityRid(token.handle), static_cast<DWORD>(SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_FALSE(TokenHasEnabledAdministrators(token.handle));
  EXPECT_EQ(DefaultDaclAllows(token.handle, TokenSid(token.handle, TokenUser)),
            static_cast<ACCESS_MASK>(GENERIC_ALL));
}

// An elevated token's default DACL grants only Administrators and SYSTEM. A token made from it
// keeps that default unless CreateMediumToken replaces it, and a filtered child (Administrators
// deny-only) then cannot open its own token (error 5). Our own token gets that default DACL while
// the Medium tokens are made, so this runs the elevated case at any integrity level. (Not a copy
// of our token: starting a process with a token filtered from a copy needs a privilege.)
TEST(MediumIl, TokensGrantTheirUserWhenTheSourceGrantsOnlyAdministrators) {
  const Sid administrators = WellKnown(WinBuiltinAdministratorsSid);
  const Sid system = WellKnown(WinLocalSystemSid);
  ASSERT_FALSE(administrators.empty() || system.empty());
  alignas(DWORD) BYTE acl[256];
  const PACL elevated = reinterpret_cast<PACL>(acl);  // D:(A;;GA;;;BA)(A;;GA;;;SY)
  ASSERT_TRUE(InitializeAcl(elevated, sizeof(acl), ACL_REVISION));
  ASSERT_TRUE(AddAccessAllowedAce(elevated, ACL_REVISION, GENERIC_ALL,
                                  const_cast<BYTE*>(administrators.data())));
  ASSERT_TRUE(
      AddAccessAllowedAce(elevated, ACL_REVISION, GENERIC_ALL, const_cast<BYTE*>(system.data())));

  OwnedHandle self, labelOnly, filtered;
  ASSERT_TRUE(OpenProcessToken(
      GetCurrentProcess(),
      TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY, &self.handle));
  const Sid user = TokenSid(self.handle, TokenUser);
  ASSERT_FALSE(user.empty());
  {
    const ScopedDefaultDacl asElevated(self.handle, elevated);
    ASSERT_TRUE(asElevated.ok());
    ASSERT_EQ(DefaultDaclAllows(self.handle, user), 0u);
    ASSERT_EQ(CreateMediumToken(self.handle, MediumToken::kLabelOnly, &labelOnly.handle), S_OK);
    ASSERT_EQ(CreateMediumToken(self.handle, MediumToken::kFiltered, &filtered.handle), S_OK);
  }
  for (const HANDLE token : {labelOnly.handle, filtered.handle}) {
    SCOPED_TRACE(token == filtered.handle ? "filtered" : "label only");
    EXPECT_EQ(DefaultDaclAllows(token, user), static_cast<ACCESS_MASK>(GENERIC_ALL));
    EXPECT_EQ(DefaultDaclAllows(token, system), static_cast<ACCESS_MASK>(GENERIC_ALL));
    EXPECT_EQ(DefaultDaclAllows(token, administrators), 0u);
    EXPECT_EQ(TokenSid(token, TokenOwner), user);
  }

  // The filtered child opens its own token (check-token exits 3 when it cannot).
  DWORD code = 0;
  ASSERT_EQ(RunWithToken(filtered.handle, Self(), ChildCommand(L"check-token"), &code), S_OK);
  EXPECT_EQ(code, 0u) << "the child cannot open its own token, or is not a Medium standard user";
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
  // The tool starting a second program: Application Control can block that start too (the
  // tool then exits with 126).
  const struct {
    std::wstring arguments;
    DWORD code;
  } nested[] = {
      {L" -- " + tool + L" --exit 42", 42},
      {L" " + tool + L" --check-token", 0},
      {L" " + ChildCommand(L"check-token"), 0},
      {L" --label-only -- " + tool + L" --exit 3", 3},
  };
  for (const auto& call : nested) {
    const Started run = RunPlain(tool + call.arguments);
    if (IsApplicationControlBlock(run.hr) || run.code == 126) {
      GTEST_SKIP() << "Application Control (Smart App Control) blocked pdft-run-medium.exe or a "
                      "program it started on this PC";
    }
    ASSERT_EQ(run.hr, S_OK) << "pdft-run-medium" << call.arguments;
    EXPECT_EQ(run.code, call.code) << "pdft-run-medium" << call.arguments;
  }
  EXPECT_EQ(RunPlain(tool + L" -- " + QuoteArgument(OwnFolder() + L"\\no such program.exe")).code,
            125u);
  EXPECT_EQ(RunPlain(tool).code, 2u);
  EXPECT_EQ(RunPlain(tool + L" --exit").code, 2u);
  EXPECT_EQ(RunPlain(tool + L" --exit 1x").code, 2u);
}

}  // namespace
}  // namespace pdft::test
