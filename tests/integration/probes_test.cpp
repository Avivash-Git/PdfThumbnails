// What the check reads besides the registration: Smart App Control (sac.h), the Code Integrity
// log, PdfCraft (pdfcraft.h), the check task and 1.x's task (tasks.h), Explorer's settings and the
// Windows build (explorer_settings.h). Read-only against the real system: on a developer's PC
// and on CI (elevated, nothing installed, x64 and ARM64) alike, so the tests assert that the
// answers are consistent, never what is installed. Planted values go to a private registry key
// per test (ScopedRegistrySandbox).

#include <windows.h>

#include <gtest/gtest.h>
#include <objbase.h>

#include <cstring>
#include <optional>
#include <string>

#include "delay_load.h"
#include "explorer_settings.h"
#include "pdfcraft.h"
#include "project_config.h"
#include "registration.h"
#include "registry_sandbox.h"
#include "sac.h"
#include "strings.h"
#include "system_probes.h"
#include "tasks.h"

namespace pdft {
namespace {

void SetDword(HKEY root, const wchar_t* key, const wchar_t* name, DWORD value) {
  ASSERT_EQ(RegSetKeyValueW(root, key, name, REG_DWORD, &value, sizeof(value)), ERROR_SUCCESS);
}

void SetText(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
  ASSERT_EQ(RegSetKeyValueW(root, key.c_str(), name, REG_SZ, value.c_str(),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))),
            ERROR_SUCCESS);
}

// A GUID string without braces, for names nobody else uses.
std::wstring NewId() {
  GUID id = {};
  EXPECT_HRESULT_SUCCEEDED(CoCreateGuid(&id));
  wchar_t text[40] = {};
  EXPECT_GT(StringFromGUID2(id, text, ARRAYSIZE(text)), 0);
  std::wstring result = text;
  return result.substr(1, result.size() - 2);
}

class Sandboxed : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sandbox_.ok()) << HResultText(sandbox_.error());
    roots_ = {sandbox_.user(), sandbox_.machine()};
  }

  test::ScopedRegistrySandbox sandbox_;
  RegistryRoots roots_;
};

// ---- Delay-loaded system DLLs --------------------------------------------------------------

TEST(DelayLoad, BindsWhatThisProgramDelayLoads) {
  // Any spelling of the name: the table says "wevtapi.dll".
  EXPECT_HRESULT_SUCCEEDED(EnsureDelayLoaded("WEVTAPI.DLL"));
  EXPECT_HRESULT_SUCCEEDED(EnsureDelayLoaded("msi.dll"));
  EXPECT_NE(GetModuleHandleW(L"msi.dll"), nullptr);
  // Linked normally and loaded: fine. Neither: the loader's answer.
  EXPECT_HRESULT_SUCCEEDED(EnsureDelayLoaded("kernel32.dll"));
  EXPECT_EQ(EnsureDelayLoaded("pdft-no-such-module.dll"), HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND));
  EXPECT_EQ(EnsureDelayLoaded(nullptr), E_INVALIDARG);
}

// ---- Smart App Control --------------------------------------------------------------------

TEST(SmartAppControl, RealStateMatchesTheRegistry) {
  SacReading reading;
  ASSERT_HRESULT_SUCCEEDED(ReadSacState(RegistryRoots{}, &reading));
  DWORD value = 0;
  DWORD size = sizeof(value);
  const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, kSacPolicyKey, kSacPolicyValue,
                                      RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (status == ERROR_SUCCESS) {
    EXPECT_EQ(reading.value, value);
    switch (value) {
      case 0:
        EXPECT_EQ(reading.state, SacState::kOff);
        break;
      case 1:
        EXPECT_EQ(reading.state, SacState::kOn);
        break;
      case 2:
        EXPECT_EQ(reading.state, SacState::kEvaluation);
        break;
      default:
        EXPECT_EQ(reading.state, SacState::kUnknown);
        break;
    }
  } else if (status == ERROR_FILE_NOT_FOUND) {
    EXPECT_EQ(reading.state, SacState::kAbsent);
  } else {
    EXPECT_EQ(reading.state, SacState::kUnknown);  // there, but not a DWORD
  }
}

TEST_F(Sandboxed, SmartAppControlStates) {
  SacReading reading;
  ASSERT_HRESULT_SUCCEEDED(ReadSacState(roots_, &reading));
  EXPECT_EQ(reading.state, SacState::kAbsent);
  const std::pair<DWORD, SacState> cases[] = {
      {0, SacState::kOff}, {1, SacState::kOn}, {2, SacState::kEvaluation}, {7, SacState::kUnknown}};
  for (const auto& [value, state] : cases) {
    SetDword(sandbox_.machine(), kSacPolicyKey, kSacPolicyValue, value);
    ASSERT_HRESULT_SUCCEEDED(ReadSacState(roots_, &reading));
    EXPECT_EQ(reading.state, state) << value;
    EXPECT_EQ(reading.value, value);
  }
  // Read from the machine root only.
  SetDword(sandbox_.user(), kSacPolicyKey, kSacPolicyValue, 1);
  SetDword(sandbox_.machine(), kSacPolicyKey, kSacPolicyValue, 0);
  ASSERT_HRESULT_SUCCEEDED(ReadSacState(roots_, &reading));
  EXPECT_EQ(reading.state, SacState::kOff);
}

TEST(SmartAppControl, BlockLogForAMadeUpBuild) {
  // A build folder that cannot exist: nothing in the log names it.
  const std::wstring leaf = L"9.9.9.9-" + NewId();
  const BlockCount blocks = CountCiBlocks(leaf, kSacBlockDays);
  if (!blocks.readable) {
    // Only a Windows without the channel may say so; anything else is a reading bug.
    EXPECT_EQ(blocks.error, HRESULT_FROM_WIN32(ERROR_EVT_CHANNEL_NOT_FOUND))
        << HResultText(blocks.error);
    return;
  }
  EXPECT_EQ(blocks.count, 0);
  EXPECT_EQ(blocks.error, S_OK);
  // Through the interface the check uses, the same.
  const BlockCount again = RealSystemProbes().CountCiBlocks(leaf, kSacBlockDays);
  EXPECT_TRUE(again.readable);
  EXPECT_EQ(again.count, 0);
}

// ---- PdfCraft -----------------------------------------------------------------------------

TEST(PdfCraft, DetectionIsConsistent) {
  PdfCraftInfo info;
  ASSERT_HRESULT_SUCCEEDED(DetectPdfCraft(&info));
  if (info.installed) {
    EXPECT_NE(info.source, PdfCraftSource::kNone);
    EXPECT_TRUE(info.productCode.empty() || IsGuidString(info.productCode)) << info.productCode;
    EXPECT_LE(info.version.size(), kMaxPdfCraftVersionChars);
    if (info.source != PdfCraftSource::kUninstallKey) EXPECT_FALSE(info.version.empty());
  } else {
    EXPECT_EQ(info.source, PdfCraftSource::kNone);
    EXPECT_TRUE(info.version.empty());
  }
  // What the Uninstall keys know, the full detection knows too.
  PdfCraftInfo keys;
  ASSERT_HRESULT_SUCCEEDED(DetectPdfCraftInUninstallKeys(RegistryRoots{}, &keys));
  if (keys.installed) EXPECT_TRUE(info.installed);
  // And Windows Installer agrees with itself.
  PdfCraftInfo installer;
  ASSERT_HRESULT_SUCCEEDED(DetectPdfCraftWithInstaller(&installer));
  if (installer.installed) {
    EXPECT_EQ(info.productCode, installer.productCode);
    EXPECT_EQ(info.version, installer.version);
  }
  PdfCraftInfo throughProbes;
  ASSERT_HRESULT_SUCCEEDED(RealSystemProbes().DetectPdfCraft(&throughProbes));
  EXPECT_EQ(throughProbes.installed, info.installed);
}

TEST_F(Sandboxed, PdfCraftInUninstallKeys) {
  PdfCraftInfo info;
  ASSERT_HRESULT_SUCCEEDED(DetectPdfCraftInUninstallKeys(roots_, &info));
  EXPECT_FALSE(info.installed);

  const std::wstring uninstall = kUninstallKey;
  const std::wstring userCode = L"{" + NewId() + L"}";
  SetText(sandbox_.user(), uninstall + L"\\" + userCode, L"DisplayName", L"PdfCraft");
  SetText(sandbox_.user(), uninstall + L"\\" + userCode, L"DisplayVersion", L"0.13.0");
  SetText(sandbox_.machine(), uninstall + L"\\PdfCraft_is1", L"DisplayName", L"pdfcraft");
  SetText(sandbox_.machine(), uninstall + L"\\PdfCraft_is1", L"DisplayVersion", L"0.15.1\r\n");
  SetText(sandbox_.machine(), uninstall + L"\\Other", L"DisplayName", L"PdfCraft Helper");
  SetText(sandbox_.machine(), uninstall + L"\\Other", L"DisplayVersion", L"9.0");
  SetDword(sandbox_.machine(), (uninstall + L"\\Odd").c_str(), L"DisplayName", 1);

  ASSERT_HRESULT_SUCCEEDED(DetectPdfCraftInUninstallKeys(roots_, &info));
  ASSERT_TRUE(info.installed);
  EXPECT_EQ(info.source, PdfCraftSource::kUninstallKey);
  EXPECT_EQ(info.version, L"0.15.1");  // the newest, its line break dropped
  EXPECT_EQ(info.productCode, L"");    // its key name is not a product code

  ASSERT_EQ(RegDeleteTreeW(sandbox_.machine(), (uninstall + L"\\PdfCraft_is1").c_str()),
            ERROR_SUCCESS);
  ASSERT_HRESULT_SUCCEEDED(DetectPdfCraftInUninstallKeys(roots_, &info));
  ASSERT_TRUE(info.installed);
  EXPECT_EQ(info.version, L"0.13.0");
  EXPECT_EQ(info.productCode, userCode);
}

// ---- Tasks --------------------------------------------------------------------------------

TEST(CheckTask, AMadeUpUserHasNone) {
  const std::wstring sid = L"S-1-12-1-1-2-3-" + std::to_wstring(GetTickCount64() % 1000000 + 1000);
  TaskLocation location;
  ASSERT_HRESULT_SUCCEEDED(DefaultTaskLocation(sid, &location));
  TaskState state;
  state.exists = true;
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location, &state));
  EXPECT_FALSE(state.exists);
  EXPECT_FALSE(state.atFallback);
  EXPECT_TRUE(state.path.empty());

  // A folder that does not exist, with a fallback name that does not either.
  const std::wstring id = NewId();
  location.folder = std::wstring(L"\\") + kProductName + L"-Test-" + id;
  location.fallbackName = std::wstring(kProductName) + L"-Test-" + id + L" check " + sid;
  ASSERT_HRESULT_SUCCEEDED(RealSystemProbes().ReadTask(location, &state));
  EXPECT_FALSE(state.exists);
}

TEST(CheckTask, LegacyTaskReadsConsistently) {
  bool exists = true;
  ASSERT_HRESULT_SUCCEEDED(LegacyTaskExists(&exists));
  bool throughProbes = !exists;
  ASSERT_HRESULT_SUCCEEDED(RealSystemProbes().LegacyTaskExists(&throughProbes));
  EXPECT_EQ(throughProbes, exists);

  TaskLocation location;
  ASSERT_TRUE(SplitTaskPath(kLegacyTask, &location.folder, &location.name));
  TaskState state;
  const HRESULT hr = ReadTask(location, &state);
  if (!exists) {
    // Absent, or another account's (then reading it is refused).
    EXPECT_TRUE(SUCCEEDED(hr) ? !state.exists : hr == E_ACCESSDENIED) << HResultText(hr);
    return;
  }
  // 1.x registered it with both triggers and one program to run (read-only here).
  ASSERT_HRESULT_SUCCEEDED(hr);
  EXPECT_TRUE(state.exists);
  EXPECT_TRUE(IEquals(state.path, kLegacyTask)) << state.path;
  EXPECT_TRUE(state.atSignIn);
  EXPECT_TRUE(state.afterPdfCraft);
  EXPECT_FALSE(state.actionPath.empty());
  EXPECT_FALSE(state.atFallback);
}

// ---- Explorer settings and the Windows build ----------------------------------------------

TEST(ExplorerSettingsReal, ReadsWithoutFailing) {
  ExplorerSettings settings;
  EXPECT_HRESULT_SUCCEEDED(ReadExplorerSettings(RegistryRoots{}, &settings));
}

TEST_F(Sandboxed, ExplorerSettings) {
  ExplorerSettings settings;
  ASSERT_HRESULT_SUCCEEDED(ReadExplorerSettings(roots_, &settings));
  EXPECT_FALSE(settings.iconsOnly);
  EXPECT_FALSE(settings.thumbnailsOffByUserPolicy);
  EXPECT_FALSE(settings.thumbnailsOffByMachinePolicy);

  SetDword(sandbox_.user(), kExplorerAdvancedKey, kIconsOnlyValue, 1);
  SetDword(sandbox_.machine(), kExplorerPoliciesKey, kDisableThumbnailsValue, 1);
  ASSERT_HRESULT_SUCCEEDED(ReadExplorerSettings(roots_, &settings));
  EXPECT_TRUE(settings.iconsOnly);
  EXPECT_FALSE(settings.thumbnailsOffByUserPolicy);
  EXPECT_TRUE(settings.thumbnailsOffByMachinePolicy);

  SetDword(sandbox_.user(), kExplorerAdvancedKey, kIconsOnlyValue, 0);
  SetDword(sandbox_.user(), kExplorerPoliciesKey, kDisableThumbnailsValue, 1);
  SetDword(sandbox_.machine(), kExplorerPoliciesKey, kDisableThumbnailsValue, 0);
  ASSERT_HRESULT_SUCCEEDED(ReadExplorerSettings(roots_, &settings));
  EXPECT_FALSE(settings.iconsOnly);
  EXPECT_TRUE(settings.thumbnailsOffByUserPolicy);
  EXPECT_FALSE(settings.thumbnailsOffByMachinePolicy);
}

TEST(OsBuildReal, NamesThisWindows) {
  std::wstring build;
  ASSERT_HRESULT_SUCCEEDED(ReadOsBuild(RegistryRoots{}, &build));
  ASSERT_FALSE(build.empty());
  EXPECT_TRUE(build[0] >= L'0' && build[0] <= L'9') << build;
  wchar_t current[32] = {};
  DWORD size = sizeof(current);
  ASSERT_EQ(RegGetValueW(HKEY_LOCAL_MACHINE, kWindowsVersionKey, L"CurrentBuild", RRF_RT_REG_SZ,
                         nullptr, current, &size),
            ERROR_SUCCESS);
  EXPECT_TRUE(build == current || build.starts_with(std::wstring(current) + L".")) << build;
}

TEST_F(Sandboxed, OsBuild) {
  std::wstring build = L"x";
  ASSERT_HRESULT_SUCCEEDED(ReadOsBuild(roots_, &build));
  EXPECT_EQ(build, L"");
  SetText(sandbox_.machine(), kWindowsVersionKey, L"CurrentBuild", L"26300");
  SetDword(sandbox_.machine(), kWindowsVersionKey, L"UBR", 9818);
  ASSERT_HRESULT_SUCCEEDED(ReadOsBuild(roots_, &build));
  EXPECT_EQ(build, L"26300.9818");
}

}  // namespace
}  // namespace pdft
