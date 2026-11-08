// The settings executable (Windows subsystem): the window, scheduled checks and the elevated
// steps. It shares all of its code with the command-line executable (pdft_appcore); what only it
// shows on screen lives here, so the command-line executable never imports Common Controls.

#include <windows.h>

#include <commctrl.h>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>

#include "app_main.h"
#include "preview_child.h"
#include "project_config.h"
#include "resource_ids.h"
#include "startup.h"

namespace {

// A string from this executable's string table.
std::wstring LoadResourceString(UINT id) {
  const wchar_t* text = nullptr;
  // A buffer size of 0 returns a pointer into the loaded resource itself.
  const int length = LoadStringW(GetModuleHandleW(nullptr), id, reinterpret_cast<LPWSTR>(&text), 0);
  return length > 0 && text ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
}

// Until the settings window exists: says so and points to the command line.
int ShowPlaceholderWindow(const pdft::WindowStart&) noexcept {
  try {
    const std::wstring title = LoadResourceString(IDS_WINDOW_NOT_READY);
    std::wstring text = LoadResourceString(IDS_WINDOW_USE_CLI);
    constexpr std::wstring_view kCliPlaceholder = L"{cli}";
    if (const size_t at = text.find(kCliPlaceholder); at != std::wstring::npos) {
      text.replace(at, kCliPlaceholder.size(), pdft::kCliExe);
    }
    TaskDialog(nullptr, nullptr, pdft::kProductName, title.c_str(), text.c_str(),
               TDCBF_CLOSE_BUTTON, TD_INFORMATION_ICON, nullptr);
  } catch (...) {
    // Out of memory: nothing to show.
  }
  return static_cast<int>(pdft::ExitCode::kOk);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // First, before anything can load a DLL by name (startup.h).
  const HRESULT hardened = pdft::HardenProcess();
  try {
    // The C runtime splits the command line for Windows-subsystem programs too.
    std::span<const wchar_t* const> args;
    if (__wargv && __argc > 0) args = {__wargv, static_cast<std::size_t>(__argc)};
    return pdft::AppMain(pdft::Mode::kGui, hardened, args,
                         pdft::GuiEntry{&ShowPlaceholderWindow, &pdft::RenderPreviewChild});
  } catch (...) {
    // Nothing may escape: an exit code beats std::terminate.
    return static_cast<int>(pdft::ExitCode::kFailed);
  }
}
