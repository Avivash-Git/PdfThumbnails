// The COM servers the app talks to accept calls under its COM security (InitializeComSecurity:
// packet privacy, impersonation, only the user and SYSTEM may call back). Read-only.

#include <windows.h>

#include <gtest/gtest.h>
#include <oleauto.h>
#include <shlobj.h>  // ShellWindows (exdisp.h), after the OLE headers exdisp.h needs
#include <taskschd.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "wmi_registry.h"

namespace pdft {
namespace {

TEST(ComSecurity, TaskSchedulerConnectsAndOpensTheRootFolder) {
  wil::com_ptr_nothrow<ITaskService> service;
  ASSERT_HRESULT_SUCCEEDED(CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                                            IID_PPV_ARGS(service.put())));
  VARIANT empty = {};
  empty.vt = VT_EMPTY;
  ASSERT_HRESULT_SUCCEEDED(service->Connect(empty, empty, empty, empty));
  wil::unique_bstr root = wil::make_bstr_nothrow(L"\\");
  ASSERT_TRUE(root);
  wil::com_ptr_nothrow<ITaskFolder> folder;
  ASSERT_HRESULT_SUCCEEDED(service->GetFolder(root.get(), folder.put()));
  wil::unique_bstr path;
  ASSERT_HRESULT_SUCCEEDED(folder->get_Path(path.put()));
  EXPECT_STREQ(path.get(), L"\\");
}

TEST(ComSecurity, WmiRegistryProviderReadsAValue) {
  constexpr wchar_t kKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
  RegistryThroughWmi wmi;
  ASSERT_HRESULT_SUCCEEDED(wmi.Connect());
  std::optional<std::wstring> build;
  ASSERT_HRESULT_SUCCEEDED(
      wmi.GetString(RegistryThroughWmi::kLocalMachine, kKey, L"CurrentBuild", &build));
  ASSERT_TRUE(build);
  wchar_t expected[32] = {};
  DWORD size = sizeof(expected);
  ASSERT_EQ(RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"CurrentBuild", RRF_RT_REG_SZ, nullptr,
                         expected, &size),
            ERROR_SUCCESS);
  EXPECT_EQ(*build, expected);

  // A missing value or key is an answer, not a failure; a value of another type is a failure.
  std::optional<std::wstring> missing = L"x";
  ASSERT_HRESULT_SUCCEEDED(wmi.GetString(RegistryThroughWmi::kLocalMachine, kKey,
                                         L"NoSuchValue-PdftIntegrationTest", &missing));
  EXPECT_FALSE(missing);
  missing = L"x";
  ASSERT_HRESULT_SUCCEEDED(wmi.GetString(RegistryThroughWmi::kLocalMachine,
                                         L"SOFTWARE\\NoSuchKey-PdftIntegrationTest", L"",
                                         &missing));
  EXPECT_FALSE(missing);
  std::optional<DWORD> wrongType;
  EXPECT_HRESULT_FAILED(
      wmi.GetDword(RegistryThroughWmi::kLocalMachine, kKey, L"CurrentBuild", &wrongType));

  std::vector<std::wstring> names;
  ASSERT_HRESULT_SUCCEEDED(
      wmi.EnumKeys(RegistryThroughWmi::kLocalMachine, L"SOFTWARE\\Microsoft\\Windows NT", &names));
  EXPECT_NE(std::find(names.begin(), names.end(), L"CurrentVersion"), names.end());
  ASSERT_HRESULT_SUCCEEDED(wmi.EnumKeys(RegistryThroughWmi::kLocalMachine,
                                        L"SOFTWARE\\NoSuchKey-PdftIntegrationTest", &names));
  EXPECT_TRUE(names.empty());
  std::optional<DWORD> number;
  ASSERT_HRESULT_SUCCEEDED(
      wmi.GetDword(RegistryThroughWmi::kLocalMachine, kKey, L"CurrentMajorVersionNumber", &number));
  ASSERT_TRUE(number);
  EXPECT_GE(*number, 10u);
}

TEST(ComSecurity, ShellWindowsCountsTheExplorerWindows) {
  if (!GetShellWindow()) GTEST_SKIP() << "no shell (Explorer) on this desktop";
  wil::com_ptr_nothrow<IShellWindows> windows;
  ASSERT_HRESULT_SUCCEEDED(
      CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_ALL, IID_PPV_ARGS(windows.put())));
  long count = -1;
  ASSERT_HRESULT_SUCCEEDED(windows->get_Count(&count));
  EXPECT_GE(count, 0);
}

}  // namespace
}  // namespace pdft
