// The process context against the real system (context.h). Writes only the canary's volatile
// probe key under HKCU\Software, and checks that the key it used is gone afterwards in both
// registry views.
//
// Whether this process runs in an app container depends on where the tests run: on CI it does
// not; started from a terminal inside a packaged app it may. The tests therefore check that the
// answers agree with each other, not that they have particular values.

#include <windows.h>

#include "context.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "child_process.h"
#include "instance.h"
#include "medium_il.h"
#include "project_config.h"
#include "strings.h"
#include "wmi_registry.h"

namespace pdft {
namespace {

const wchar_t* Name(Virtualization virtualization) {
  switch (virtualization) {
    case Virtualization::kNo:
      return L"no";
    case Virtualization::kYes:
      return L"yes";
    case Virtualization::kUnknown:
      return L"unknown";
  }
  return L"?";
}

const wchar_t* Name(RegistryViews views) {
  switch (views) {
    case RegistryViews::kNotChecked:
      return L"not checked";
    case RegistryViews::kAgree:
      return L"agree";
    case RegistryViews::kDiffer:
      return L"differ";
    case RegistryViews::kUnknown:
      return L"unknown";
  }
  return L"?";
}

std::optional<std::wstring> OwnAncestor() {
  std::optional<std::wstring> package;
  EXPECT_HRESULT_SUCCEEDED(FindPackagedAncestor(&package));
  return package;
}

// Checks that the probe key |key| (under HKCU) exists in neither registry view. Only this one
// key: another process may run its own canary at the same time.
void ExpectProbeKeyGone(const std::wstring& sid, const std::wstring& key) {
  HKEY opened = nullptr;
  EXPECT_EQ(RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &opened),
            static_cast<LSTATUS>(ERROR_FILE_NOT_FOUND))
      << WideToUtf8Lossy(key);
  if (opened) RegCloseKey(opened);

  const size_t slash = key.find_last_of(L'\\');
  ASSERT_NE(slash, std::wstring::npos);
  const std::wstring parent = key.substr(0, slash);
  const std::wstring leaf = key.substr(slash + 1);
  RegistryThroughWmi wmi;
  ASSERT_HRESULT_SUCCEEDED(wmi.Connect());
  std::vector<std::wstring> names;
  ASSERT_HRESULT_SUCCEEDED(wmi.EnumKeys(RegistryThroughWmi::kUsers, sid + L"\\" + parent, &names));
  for (const std::wstring& name : names) {
    EXPECT_FALSE(IEquals(name, leaf)) << WideToUtf8Lossy(name);
  }
}

TEST(Context, AncestorWalkAgreesWithAGrandchild) {
  const std::optional<std::wstring> own = OwnAncestor();
  const test::CapturedRun run =
      test::RunCaptured(test::SelfPath(), L"--pdft-child ancestor 1", 60000);
  ASSERT_HRESULT_SUCCEEDED(run.hr);
  ASSERT_EQ(run.exitCode, 0u) << run.out << run.err;
  const std::string expected = own ? WideToUtf8Lossy(*own) + "\n" : "none\n";
  EXPECT_EQ(run.out, expected);
  std::printf("packaged ancestor: %s", expected.c_str());
}

TEST(Context, ElevatedMeansAdministratorsEnabledOrAboveMedium) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context));
  HANDLE token = nullptr;
  ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token));
  const bool administrators = test::TokenHasEnabledAdministrators(token);
  CloseHandle(token);
  const DWORD integrity = test::CurrentIntegrityRid();
  EXPECT_EQ(context.integrityRid, integrity);
  EXPECT_EQ(context.elevated, administrators || integrity > SECURITY_MANDATORY_MEDIUM_RID);
  std::printf("integrity 0x%04lX, Administrators %s, elevated %s\n", integrity,
              administrators ? "enabled" : "not enabled", context.elevated ? "yes" : "no");
}

TEST(Context, IdentityAndArchitectures) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context));
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  EXPECT_EQ(context.userSid, sid);
  EXPECT_FALSE(context.userAccount.empty());
  EXPECT_EQ(context.probe, ContextProbe::kNone);
  EXPECT_EQ(context.virtualization, Virtualization::kNo);
  EXPECT_EQ(context.views, RegistryViews::kNotChecked);
  EXPECT_EQ(context.probeError, S_OK);
#if defined(_M_ARM64)
  EXPECT_EQ(context.processArch, Arch::kArm64);
#elif defined(_M_X64)
  EXPECT_EQ(context.processArch, Arch::kX64);
#endif
  EXPECT_NE(context.osArch, Arch::kOther);
  // Without a desktop shell there is nobody to differ from. With one, a CI runner's shell may
  // belong to another account, so the answer is only reported.
  if (!GetShellWindow()) EXPECT_FALSE(context.differentFromShellUser);
  std::printf("account differs from the shell's: %s\n",
              context.differentFromShellUser ? "yes" : "no");
}

TEST(Context, WriteCanaryLeavesNoProbeKeyAndAgreesWithTheAncestor) {
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  Virtualization virtualization = Virtualization::kUnknown;
  HRESULT failure = E_UNEXPECTED;
  std::wstring probeKey;
  ASSERT_HRESULT_SUCCEEDED(ProbeRegistryVirtualization(sid, &virtualization, &failure, &probeKey));

  EXPECT_EQ(probeKey.rfind(std::wstring(kRegKey) + L".probe-", 0), 0u) << WideToUtf8Lossy(probeKey);
  ExpectProbeKeyGone(sid, probeKey);
  // A failure is kept only behind an undecided verdict.
  if (virtualization != Virtualization::kUnknown) EXPECT_EQ(failure, S_OK);
  const std::optional<std::wstring> ancestor = OwnAncestor();
  std::printf("canary: %ls (packaged ancestor: %ls)\n", Name(virtualization),
              ancestor ? ancestor->c_str() : L"none");
  if (!ancestor) {
    // Outside any package nothing redirects the registry.
    EXPECT_EQ(virtualization, Virtualization::kNo) << WideToUtf8Lossy(HResultText(failure));
  } else {
    // Inside one either answer is possible (some packages do not virtualize their children's
    // registry), but WMI must have answered.
    EXPECT_NE(virtualization, Virtualization::kUnknown) << WideToUtf8Lossy(HResultText(failure));
  }
}

TEST(Context, ReadOnlyCheckAgreesWithTheAncestor) {
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  const std::optional<std::wstring> ancestor = OwnAncestor();
  RegistryViews views = RegistryViews::kNotChecked;
  HRESULT failure = E_UNEXPECTED;
  ASSERT_HRESULT_SUCCEEDED(CompareRegistryViews(sid, ancestor.has_value(), &views, &failure));
  std::printf("read-only check: views %ls\n", Name(views));
  EXPECT_NE(views, RegistryViews::kNotChecked);
  if (views != RegistryViews::kUnknown) EXPECT_EQ(failure, S_OK);
  if (!ancestor) EXPECT_EQ(views, RegistryViews::kAgree) << WideToUtf8Lossy(HResultText(failure));
}

TEST(Context, GetProcessContextRunsTheRequestedCheck) {
  ProcessContext write;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &write));
  EXPECT_EQ(write.probe, ContextProbe::kWrite);
  EXPECT_EQ(write.views, RegistryViews::kNotChecked);  // only the read-only check fills it
  ProcessContext readOnly;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kReadOnly, &readOnly));
  EXPECT_EQ(readOnly.probe, ContextProbe::kReadOnly);
  EXPECT_EQ(readOnly.virtualization, Virtualization::kNo);  // only the canary fills it
  EXPECT_NE(readOnly.views, RegistryViews::kNotChecked);
  EXPECT_EQ(write.packagedAncestor, readOnly.packagedAncestor);
  std::printf("context: canary %ls, views %ls\n", Name(write.virtualization), Name(readOnly.views));
  if (!write.packagedAncestor) {
    EXPECT_EQ(write.virtualization, Virtualization::kNo)
        << WideToUtf8Lossy(HResultText(write.probeError));
    EXPECT_EQ(readOnly.views, RegistryViews::kAgree)
        << WideToUtf8Lossy(HResultText(readOnly.probeError));
  }
}

}  // namespace
}  // namespace pdft
