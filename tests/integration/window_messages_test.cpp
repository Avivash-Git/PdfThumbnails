// How the app's processes reach an open settings window: against a hidden window of this test's
// own class name (the names carry the run's suffix), so a real settings window is never touched;
// and that a window of that class in a process of the same user below Medium integrity is not
// taken for it.

#include <windows.h>

#include "window_messages.h"

#include <gtest/gtest.h>
#include <wil/resource.h>

#include <string>

#include "child_process.h"
#include "instance.h"
#include "medium_il.h"

namespace pdft {
namespace {

// A hidden top-level window with the settings window's class name.
class FakeSettingsWindow {
 public:
  FakeSettingsWindow() : className_(SettingsWindowClassName()) {
    WNDCLASSEXW windowClass = {sizeof(windowClass)};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className_.c_str();
    registered_ = RegisterClassExW(&windowClass) != 0;
    window_ = CreateWindowExW(0, className_.c_str(), L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                              windowClass.hInstance, nullptr);
  }
  ~FakeSettingsWindow() {
    if (window_) DestroyWindow(window_);
    if (registered_) UnregisterClassW(className_.c_str(), GetModuleHandleW(nullptr));
  }
  HWND handle() const { return window_; }

  // Whether |message| arrives for this window within 5 s.
  bool Received(UINT message) const {
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < 5000) {
      MSG msg;
      while (PeekMessageW(&msg, window_, 0, 0, PM_REMOVE)) {
        if (msg.message == message) return true;
        DispatchMessageW(&msg);
      }
      Sleep(10);
    }
    return false;
  }

 private:
  std::wstring className_;
  bool registered_ = false;
  HWND window_ = nullptr;
};

TEST(WindowMessages, NoWindowIsNoFailure) {
  EXPECT_EQ(ActivateExistingWindow(), S_FALSE);
  NotifyStateChanged();  // nothing to tell, nothing to crash
}

TEST(WindowMessages, ASecondStartAsksTheWindowToComeForward) {
  FakeSettingsWindow window;
  ASSERT_NE(window.handle(), nullptr) << GetLastError();
  EXPECT_EQ(ActivateExistingWindow(), S_OK);
  EXPECT_TRUE(window.Received(kActivateWindowMessage));
}

TEST(WindowMessages, ACommandTellsTheWindowTheStateChanged) {
  FakeSettingsWindow window;
  ASSERT_NE(window.handle(), nullptr) << GetLastError();
  const UINT message = StateChangedMessage();
  ASSERT_NE(message, 0u);
  NotifyStateChanged();
  EXPECT_TRUE(window.Received(message));
}

// A process of the same user below this one's integrity level (the test program again, at Low,
// "--pdft-child window-squat") registers a window of the settings window's class: it is not taken
// for the settings window, so it is neither brought to the front nor told anything.
TEST(WindowMessages, AWindowBelowThisIntegrityLevelIsNotTheSettingsWindow) {
  if (test::CurrentIntegrityRid() < SECURITY_MANDATORY_MEDIUM_RID) {
    GTEST_SKIP() << "needs a test process at Medium integrity or above";
  }
  wil::unique_handle low;
  ASSERT_HRESULT_SUCCEEDED(test::CreateLowTokenForSelf(low.put()));
  test::PipedChild child;
  const HRESULT started = child.Start(
      test::SelfPath(), L"--pdft-child window-squat " + test::NamedObjectSuffix(), low.get());
  if (test::IsApplicationControlBlock(started)) {
    GTEST_SKIP() << "Application Control blocked the test program at Low integrity";
  }
  ASSERT_HRESULT_SUCCEEDED(started);
  EXPECT_EQ(child.ReadLine(), "il=0x1000");
  ASSERT_EQ(child.ReadLine(), "ready");
  EXPECT_EQ(ActivateExistingWindow(), S_FALSE);
  NotifyStateChanged();
  child.CloseInput();
  EXPECT_EQ(child.ReadLine(), "activate=0 changed=0");
  EXPECT_EQ(child.Wait(30000), 0u);
}

}  // namespace
}  // namespace pdft
