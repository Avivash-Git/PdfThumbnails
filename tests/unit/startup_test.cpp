// Start-up hardening (startup.h) and the COM security descriptor (com_security.h).
//
// HardenProcess changes the whole process for good (the search path mode is permanent), so it
// runs in a child: this test program started again with "--pdft-child harden" (main.cpp), which
// checks the result from the inside and reports through its exit code.

#include <windows.h>

#include "startup.h"

#include <gtest/gtest.h>
#include <objbase.h>
#include <sddl.h>

#include <string>
#include <vector>

#include "com_security.h"
#include "medium_il.h"

namespace pdft {
namespace {

std::wstring Self() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::wstring(path, length);
}

TEST(Startup, HardenProcessInAChild) {
  std::wstring commandLine = test::QuoteArgument(Self()) + L" --pdft-child harden";
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  ASSERT_TRUE(CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &startup, &process))
      << GetLastError();
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD code = 99;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  // 0: hardened; see HardenedChildMain (main.cpp) for the other codes. A 4 is a failure, not a
  // skip: the child itself ran, and Application Control can only refuse a file the bare name
  // reached, which a hardened process never does.
  EXPECT_EQ(code, 0u) << "the harden child exited " << code
                      << " (1 HardenProcess failed, 2 image-load policy missing, 3 the DLL next "
                         "to the program loaded by bare name, 4 the bare name reached a file "
                         "outside System32 and Application Control refused to map it, 5 that "
                         "load failed for another reason, 6 PDFium is not next to the test "
                         "program)";
}

// A self-relative descriptor from SDDL, freed with LocalFree.
struct Descriptor {
  PSECURITY_DESCRIPTOR value = nullptr;
  explicit Descriptor(const wchar_t* sddl) {
    ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &value, nullptr);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  ~Descriptor() { LocalFree(value); }
};

// Whether |token| (an impersonation token) gets |desired| COM rights from |descriptor|. COM
// checks with PRINCIPAL_SELF standing for the process's own user, as AccessCheckByType does here.
bool ComAllows(PSECURITY_DESCRIPTOR descriptor, HANDLE token, PSID self, DWORD desired) {
  GENERIC_MAPPING mapping = {COM_RIGHTS_EXECUTE, COM_RIGHTS_EXECUTE, COM_RIGHTS_EXECUTE,
                             COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL |
                                 COM_RIGHTS_EXECUTE_REMOTE | COM_RIGHTS_ACTIVATE_LOCAL |
                                 COM_RIGHTS_ACTIVATE_REMOTE};
  alignas(PRIVILEGE_SET) BYTE privileges[256] = {};
  DWORD privilegesSize = sizeof(privileges);
  DWORD granted = 0;
  BOOL allowed = FALSE;
  if (!AccessCheckByType(descriptor, self, token, desired, nullptr, 0, &mapping,
                         reinterpret_cast<PPRIVILEGE_SET>(privileges), &privilegesSize, &granted,
                         &allowed)) {
    ADD_FAILURE() << "AccessCheckByType failed: " << GetLastError();
    return false;
  }
  return allowed && granted == desired;
}

TEST(ComSecurity, DescriptorLetsOnlyTheUserAndSystemIn) {
  Descriptor descriptor(kComAccessSddl);
  ASSERT_NE(descriptor.value, nullptr) << GetLastError();
  ASSERT_TRUE(IsValidSecurityDescriptor(descriptor.value));

  HANDLE process = nullptr;
  ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process));
  HANDLE token = nullptr;
  ASSERT_TRUE(DuplicateToken(process, SecurityImpersonation, &token));
  CloseHandle(process);
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  ASSERT_TRUE(GetTokenInformation(token, TokenUser, user, sizeof(user), &size));
  PSID self = reinterpret_cast<TOKEN_USER*>(user)->User.Sid;

  EXPECT_TRUE(ComAllows(descriptor.value, token, self, COM_RIGHTS_EXECUTE));
  EXPECT_TRUE(
      ComAllows(descriptor.value, token, self, COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_LOCAL));
  // Never remotely, and never activation.
  EXPECT_FALSE(
      ComAllows(descriptor.value, token, self, COM_RIGHTS_EXECUTE | COM_RIGHTS_EXECUTE_REMOTE));
  EXPECT_FALSE(ComAllows(descriptor.value, token, self, COM_RIGHTS_ACTIVATE_LOCAL));

  // Another identity (anonymous) is refused even what the user gets.
  ASSERT_TRUE(ImpersonateAnonymousToken(GetCurrentThread())) << GetLastError();
  HANDLE anonymous = nullptr;
  const BOOL opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &anonymous);
  RevertToSelf();
  ASSERT_TRUE(opened) << GetLastError();
  EXPECT_FALSE(ComAllows(descriptor.value, anonymous, self, COM_RIGHTS_EXECUTE));
  CloseHandle(anonymous);
  CloseHandle(token);
}

}  // namespace
}  // namespace pdft
