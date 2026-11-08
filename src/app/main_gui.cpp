// The settings executable (Windows subsystem): the window, scheduled checks and the elevated
// steps. It shares all of its code with the command-line executable (pdft_appcore); what only it
// shows on screen lives here, so the command-line executable never imports Common Controls.

#include <windows.h>

#include <cstddef>
#include <cstdlib>

#include "app_main.h"
#include "main_window.h"
#include "preview_child.h"
#include "startup.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  // First, before anything can load a DLL by name (startup.h).
  const HRESULT hardened = pdft::HardenProcess();
  try {
    // The C runtime splits the command line for Windows-subsystem programs too.
    std::span<const wchar_t* const> args;
    if (__wargv && __argc > 0) args = {__wargv, static_cast<std::size_t>(__argc)};
    return pdft::AppMain(pdft::Mode::kGui, hardened, args,
                         pdft::GuiEntry{&pdft::OpenMainWindow, &pdft::RenderPreviewChild});
  } catch (...) {
    // Nothing may escape: an exit code beats std::terminate.
    return static_cast<int>(pdft::ExitCode::kFailed);
  }
}
