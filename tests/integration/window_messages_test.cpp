// How the app's processes reach an open settings window: against a hidden window of this test's
// own class name (the names carry the run's suffix), so a real settings window is never touched.

#include <windows.h>

#include "window_messages.h"

#include <gtest/gtest.h>

#include <string>

#include "instance.h"

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

}  // namespace
}  // namespace pdft
