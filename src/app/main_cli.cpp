// The command-line executable (console subsystem), for terminals and scripts. It shares all of
// its code with the settings executable (pdft_appcore).

#include <windows.h>

#include <cstddef>

#include "app_main.h"
#include "startup.h"

int wmain(int argc, wchar_t** argv) {
  // First, before anything can load a DLL by name (startup.h).
  const HRESULT hardened = pdft::HardenProcess();
  try {
    const std::span<const wchar_t* const> args(argv, static_cast<std::size_t>(argc > 0 ? argc : 0));
    return pdft::AppMain(pdft::Mode::kCli, hardened, args);
  } catch (...) {
    // Nothing may escape: an exit code beats std::terminate.
    return static_cast<int>(pdft::ExitCode::kFailed);
  }
}
