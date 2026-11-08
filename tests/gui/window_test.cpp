// The settings window from outside its process, on a runner where nothing is installed.

#include <windows.h>

#include <commctrl.h>
#include <gtest/gtest.h>
#include <objbase.h>
#include <uiautomation.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "github_runner.h"
#include "gui_driver.h"
#include "instance.h"
#include "project_config.h"
#include "resource_ids.h"

namespace pdft::test {
namespace {

constexpr DWORD kStartMs = 60000;

class SettingsWindow : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(program_.Start(L"")) << GetLastError();
    const std::wstring className = SettingsWindowClassName();
    ASSERT_TRUE(WaitFor(
        [&] {
          dialog_ = TopLevelWindowOf(program_.id(), className.c_str());
          return dialog_ != nullptr && IsWindowVisible(dialog_);
        },
        kStartMs))
        << "the window did not show";
    const std::wstring reading = L"Reading";
    ASSERT_TRUE(WaitFor(
        [&] {
          const std::wstring line = ControlText(dialog_, IDC_STATUS_LINE1);
          return !line.empty() && line.rfind(reading, 0) != 0;
        },
        kStartMs))
        << "the first reading did not finish";
  }

  std::wstring NameOf(int id) const {
    wil::com_ptr_nothrow<IUIAutomation> automation;
    EXPECT_HRESULT_SUCCEEDED(CoCreateInstance(
        __uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(automation.put())));
    if (!automation) return {};
    wil::com_ptr_nothrow<IUIAutomationElement> element;
    EXPECT_HRESULT_SUCCEEDED(automation->ElementFromHandle(GetDlgItem(dialog_, id), element.put()));
    if (!element) return {};
    wil::unique_bstr name;
    EXPECT_HRESULT_SUCCEEDED(element->get_CurrentName(name.put()));
    return name ? std::wstring(name.get()) : std::wstring();
  }

  Program program_;
  HWND dialog_ = nullptr;
};

TEST_F(SettingsWindow, ShowsItsTitleAndAFirstReading) {
  wchar_t title[128] = {};
  GetWindowTextW(dialog_, title, ARRAYSIZE(title));
  EXPECT_EQ(std::wstring(title), std::wstring(kProductName) + L" Settings");
  // Nothing is installed on this runner, and its registry is real.
  EXPECT_EQ(ControlText(dialog_, IDC_BANNER), L"Not installed.");
  EXPECT_TRUE(IsWindowVisible(GetDlgItem(dialog_, IDC_BANNER)));
  EXPECT_FALSE(IsWindowVisible(GetDlgItem(dialog_, IDC_BANNER_LINK)));
  EXPECT_FALSE(ControlText(dialog_, IDC_STATUS_LINE3).empty());
}

TEST_F(SettingsWindow, TheControlsAreInTheirTabOrder) {
  const std::vector<int> expected = {IDC_HEAD_ZOOM,
                                     IDC_ZOOM_LABEL,
                                     IDC_ZOOM_SLIDER,
                                     IDC_ZOOM_EDIT_NAME,
                                     IDC_ZOOM_EDIT,
                                     IDC_ZOOM_SPIN,
                                     IDC_ZOOM_PERCENT,
                                     IDC_ZOOM_HINT,
                                     IDC_PREVIEW_DOC,
                                     IDC_PREVIEW_SLIDE,
                                     IDC_PREVIEW_DOC_CAPTION,
                                     IDC_PREVIEW_SLIDE_CAPTION,
                                     IDC_HEAD_CHECKS,
                                     IDC_CHECK_PDFCRAFT,
                                     IDC_CHECK_SIGNIN,
                                     IDC_HEAD_STATUS,
                                     IDC_STATUS_GLYPH1,
                                     IDC_STATUS_LINE1,
                                     IDC_STATUS_GLYPH2,
                                     IDC_STATUS_LINE2,
                                     IDC_STATUS_GLYPH3,
                                     IDC_STATUS_LINE3,
                                     IDC_STATUS_FIX,
                                     IDC_STATUS_HINT,
                                     IDC_INSTALL_MACHINE,
                                     IDC_INSTALL_USER,
                                     IDC_BANNER,
                                     IDC_BANNER_LINK,
                                     IDC_PROGRESS,
                                     IDC_PROGRESS_CANCEL,
                                     IDC_CHECK_NOW,
                                     IDC_REBUILD,
                                     IDC_OPEN_LOG,
                                     IDC_COPY_DIAG,
                                     IDC_UNINSTALL,
                                     IDOK,
                                     IDCANCEL};
  std::vector<int> found;
  for (HWND child = GetWindow(dialog_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
    found.push_back(GetDlgCtrlID(child));
  }
  EXPECT_EQ(found, expected);
}

TEST_F(SettingsWindow, WhatIsNotAvailableIsOff) {
  EXPECT_TRUE(IsWindowEnabled(GetDlgItem(dialog_, IDCANCEL)));
  for (const int id :
       {IDC_ZOOM_SLIDER, IDC_ZOOM_EDIT, IDC_CHECK_NOW, IDC_REBUILD, IDC_UNINSTALL, IDOK}) {
    EXPECT_FALSE(IsWindowEnabled(GetDlgItem(dialog_, id))) << id;
  }
}

TEST_F(SettingsWindow, ShowsTheSavedZoom) {
  EXPECT_EQ(ControlText(dialog_, IDC_ZOOM_EDIT), L"100");
  EXPECT_EQ(SendDlgItemMessageW(dialog_, IDC_ZOOM_SLIDER, TBM_GETPOS, 0, 0), 100);
}

TEST_F(SettingsWindow, ScreenReadersGetNames) {
  EXPECT_NE(NameOf(IDC_ZOOM_SLIDER).find(L"Zoom"), std::wstring::npos);
  EXPECT_EQ(NameOf(IDC_ZOOM_EDIT), L"Zoom percent");
  EXPECT_EQ(NameOf(IDCANCEL), L"Close");
  EXPECT_NE(NameOf(IDC_CHECK_SIGNIN).find(L"sign"), std::wstring::npos);
}

TEST_F(SettingsWindow, EscapeClosesIt) {
  PostMessageW(dialog_, WM_KEYDOWN, VK_ESCAPE, 0);
  EXPECT_EQ(program_.WaitForExit(30000), 0u);
}

TEST_F(SettingsWindow, TheCloseBoxClosesIt) {
  PostMessageW(dialog_, WM_CLOSE, 0, 0);
  EXPECT_EQ(program_.WaitForExit(30000), 0u);
}

TEST_F(SettingsWindow, ASecondStartLeavesOneWindow) {
  Program second;
  ASSERT_TRUE(second.Start(L""));
  EXPECT_EQ(second.WaitForExit(30000), 0u);
  EXPECT_TRUE(program_.Running());
  EXPECT_TRUE(IsWindow(dialog_));
}

// For a person: what the window looks like on this runner.
TEST_F(SettingsWindow, KeepsAPicture) {
  const std::wstring folder = EnvironmentValue(L"PDFT_GUI_SHOTS");
  if (folder.empty()) GTEST_SKIP() << "PDFT_GUI_SHOTS is not set";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  // The first paint after the window shows.
  Sleep(500);
  EXPECT_TRUE(SaveWindowPicture(dialog_, folder + L"\\not-installed.png"));
}

}  // namespace
}  // namespace pdft::test
