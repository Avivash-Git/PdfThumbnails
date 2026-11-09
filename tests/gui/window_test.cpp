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

// Set where the product was installed first (the system job's runner).
bool Installed() {
  wchar_t value[8] = {};
  return GetEnvironmentVariableW(L"PDFT_GUI_INSTALLED", value, ARRAYSIZE(value)) == 1 &&
         value[0] == L'1';
}

TEST_F(SettingsWindow, ShowsItsTitleAndAFirstReading) {
  wchar_t title[128] = {};
  GetWindowTextW(dialog_, title, ARRAYSIZE(title));
  EXPECT_EQ(std::wstring(title), std::wstring(kProductName) + L" Settings");
  // On the runner where nothing is installed; its registry is real either way.
  if (!Installed()) {
    EXPECT_EQ(ControlText(dialog_, IDC_BANNER), L"Not installed.");
    EXPECT_TRUE(IsWindowVisible(GetDlgItem(dialog_, IDC_BANNER)));
  }
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
  EXPECT_EQ(IsWindowEnabled(GetDlgItem(dialog_, IDC_ZOOM_SLIDER)) != FALSE, Installed());
  EXPECT_EQ(IsWindowEnabled(GetDlgItem(dialog_, IDC_ZOOM_EDIT)) != FALSE, Installed());
  EXPECT_EQ(IsWindowEnabled(GetDlgItem(dialog_, IDC_CHECK_NOW)) != FALSE, Installed());
  EXPECT_TRUE(IsWindowEnabled(GetDlgItem(dialog_, IDC_OPEN_LOG)));
  EXPECT_TRUE(IsWindowEnabled(GetDlgItem(dialog_, IDC_COPY_DIAG)));
  for (const int id : {IDC_REBUILD, IDC_UNINSTALL, IDOK}) {
    EXPECT_FALSE(IsWindowEnabled(GetDlgItem(dialog_, id))) << id;
  }
}

TEST_F(SettingsWindow, ShowsTheSavedZoom) {
  EXPECT_EQ(ControlText(dialog_, IDC_ZOOM_EDIT), L"100");
  EXPECT_EQ(SendDlgItemMessageW(dialog_, IDC_ZOOM_SLIDER, TBM_GETPOS, 0, 0), 100);
}

TEST_F(SettingsWindow, ThePreviewTilesShowPages) {
  // A real thumbnail has many colours; the stand-in drawing has three.
  const HWND tile = GetDlgItem(dialog_, IDC_PREVIEW_DOC);
  EXPECT_TRUE(WaitFor([&] { return ColoursIn(dialog_, tile) > 8; }, 20000))
      << ColoursIn(dialog_, tile) << " colours";
}

class InstalledWindow : public SettingsWindow {
 protected:
  void SetUp() override {
    if (!Installed()) GTEST_SKIP() << "only where the product is installed";
    SettingsWindow::SetUp();
  }
  HWND Slider() const { return GetDlgItem(dialog_, IDC_ZOOM_SLIDER); }
  HWND Box() const { return GetDlgItem(dialog_, IDC_ZOOM_EDIT); }
  void MoveSlider(int zoom) {
    SendMessageW(Slider(), TBM_SETPOS, TRUE, zoom);
    SendMessageW(dialog_, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0),
                 reinterpret_cast<LPARAM>(Slider()));
  }
  void Type(const wchar_t* text) {
    SendMessageW(Box(), WM_SETTEXT, 0, reinterpret_cast<LPARAM>(text));
  }
  LRESULT SliderPosition() const { return SendMessageW(Slider(), TBM_GETPOS, 0, 0); }
};

// Before the test below, which leaves a record of a check behind.
TEST_F(InstalledWindow, SaysNotTestedUntilACheckHasRun) {
  EXPECT_EQ(ControlText(dialog_, IDC_STATUS_LINE1).rfind(L"Not tested yet", 0), 0u)
      << "line 1 does not say so";
}

TEST_F(InstalledWindow, CheckNowRunsAndLeavesARecord) {
  const HWND button = GetDlgItem(dialog_, IDC_CHECK_NOW);
  SendMessageW(dialog_, WM_COMMAND, MAKEWPARAM(IDC_CHECK_NOW, BN_CLICKED),
               reinterpret_cast<LPARAM>(button));
  EXPECT_TRUE(WaitFor(
      [&] {
        return IsWindowEnabled(button) &&
               ControlText(dialog_, IDC_STATUS_LINE2).rfind(L"Last check:", 0) == 0;
      },
      120000))
      << "line 2 does not show a check";
  EXPECT_EQ(ControlText(dialog_, IDC_STATUS_LINE1).find(L"Checking"), std::wstring::npos);
}

TEST_F(InstalledWindow, SaveIsOnlyOnWithSomethingToSave) {
  EXPECT_FALSE(IsWindowEnabled(GetDlgItem(dialog_, IDOK)));
  MoveSlider(170);
  EXPECT_TRUE(IsWindowEnabled(GetDlgItem(dialog_, IDOK)));
  MoveSlider(static_cast<int>(100));
  EXPECT_FALSE(IsWindowEnabled(GetDlgItem(dialog_, IDOK)));
}

TEST_F(InstalledWindow, SaveWritesTheZoom) {
  MoveSlider(130);
  SendMessageW(dialog_, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
               reinterpret_cast<LPARAM>(GetDlgItem(dialog_, IDOK)));
  EXPECT_TRUE(WaitFor(
      [&] {
        DWORD zoom = 0;
        DWORD size = sizeof(zoom);
        return RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"Zoom", RRF_RT_REG_DWORD, nullptr, &zoom,
                            &size) == ERROR_SUCCESS &&
               zoom == 130 && !IsWindowEnabled(GetDlgItem(dialog_, IDOK)) &&
               IsWindowEnabled(GetDlgItem(dialog_, IDC_CHECK_NOW));
      },
      120000))
      << "the zoom was not saved";
  // Back to what the other tests expect.
  MoveSlider(100);
  SendMessageW(dialog_, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
               reinterpret_cast<LPARAM>(GetDlgItem(dialog_, IDOK)));
  EXPECT_TRUE(WaitFor([&] { return IsWindowEnabled(GetDlgItem(dialog_, IDC_CHECK_NOW)) != FALSE; },
                      120000));
}

TEST_F(InstalledWindow, CloseAsksAboutAnUnsavedZoom) {
  MoveSlider(170);
  PostMessageW(dialog_, WM_CLOSE, 0, 0);
  HWND question = nullptr;
  ASSERT_TRUE(WaitFor(
      [&] {
        question = TopLevelWindowOf(program_.id(), L"#32770");
        return question != nullptr && question != dialog_;
      },
      15000))
      << "no question appeared";
  // "Don't save" (the second custom button, IDNO).
  PostMessageW(question, WM_COMMAND, IDNO, 0);
  EXPECT_EQ(program_.WaitForExit(30000), 0u);
}

TEST_F(InstalledWindow, TheSliderMovesTheBox) {
  MoveSlider(150);
  EXPECT_EQ(ControlText(dialog_, IDC_ZOOM_EDIT), L"150");
}

TEST_F(InstalledWindow, TheBoxMovesTheSliderOnlyWithAValidNumber) {
  Type(L"125");
  EXPECT_EQ(SliderPosition(), 125);
  Type(L"12");
  EXPECT_EQ(SliderPosition(), 125);
}

TEST_F(InstalledWindow, LeavingTheBoxSettlesItsText) {
  Type(L"999");
  SendMessageW(dialog_, WM_COMMAND, MAKEWPARAM(IDC_ZOOM_EDIT, EN_KILLFOCUS),
               reinterpret_cast<LPARAM>(Box()));
  EXPECT_EQ(ControlText(dialog_, IDC_ZOOM_EDIT), L"200");
  EXPECT_EQ(SliderPosition(), 200);
}

TEST_F(InstalledWindow, ThePreviewFollowsTheZoom) {
  const HWND tile = GetDlgItem(dialog_, IDC_PREVIEW_DOC);
  ASSERT_TRUE(WaitFor([&] { return ColoursIn(dialog_, tile) > 8; }, 20000));
  Sleep(500);
  const uint64_t before = PictureOf(dialog_, tile);
  MoveSlider(200);
  EXPECT_TRUE(WaitFor([&] { return PictureOf(dialog_, tile) != before; }, 20000));
}

TEST_F(SettingsWindow, CopyDiagnosticsPutsAMaskedReportOnTheClipboard) {
  const DWORD before = GetClipboardSequenceNumber();
  const HWND button = GetDlgItem(dialog_, IDC_COPY_DIAG);
  SendMessageW(dialog_, WM_COMMAND, MAKEWPARAM(IDC_COPY_DIAG, BN_CLICKED),
               reinterpret_cast<LPARAM>(button));
  ASSERT_TRUE(WaitFor([&] { return GetClipboardSequenceNumber() != before; }, 60000))
      << "nothing reached the clipboard";
  std::wstring report;
  ASSERT_TRUE(WaitFor(
      [&] {
        if (!OpenClipboard(nullptr)) return false;
        if (const HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
          if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
            report = text;
            GlobalUnlock(data);
          }
        }
        CloseClipboard();
        return !report.empty();
      },
      5000));
  EXPECT_NE(report.find(kProductName), std::wstring::npos);
  wchar_t user[64] = {};
  const DWORD length = GetEnvironmentVariableW(L"USERNAME", user, ARRAYSIZE(user));
  ASSERT_GT(length, 0u);
  EXPECT_EQ(report.find(user), std::wstring::npos) << "the report names the account";
  EXPECT_TRUE(WaitFor([&] { return IsWindowEnabled(button) != FALSE; }, 10000));
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
  wchar_t value[1024] = {};
  const DWORD length = GetEnvironmentVariableW(L"PDFT_GUI_SHOTS", value, ARRAYSIZE(value));
  if (length == 0 || length >= ARRAYSIZE(value)) GTEST_SKIP() << "PDFT_GUI_SHOTS is not set";
  const std::wstring folder(value, length);
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  // Once the preview has arrived.
  const HWND tile = GetDlgItem(dialog_, IDC_PREVIEW_DOC);
  (void)WaitFor([&] { return ColoursIn(dialog_, tile) > 8; }, 20000);
  Sleep(500);
  EXPECT_TRUE(SaveWindowPicture(
      dialog_, folder + (Installed() ? L"\\installed.png" : L"\\not-installed.png")));
}

}  // namespace
}  // namespace pdft::test
