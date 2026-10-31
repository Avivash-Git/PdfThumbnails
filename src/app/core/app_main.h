// What both executables run after start-up hardening (main_gui.cpp, main_cli.cpp).

#pragma once

#include <windows.h>

#include <span>

#include "commands.h"
#include "exit_code.h"
#include "mode.h"

namespace pdft {

// Sets up COM (InitializeComSecurity) and WIL's failure logging, parses the command line
// |args| (args[0] is the program) and runs it (RunCommand); returns the process exit code.
// |hardened| is what HardenProcess() returned: when it failed, nothing else runs. |openWindow|
// opens the settings window (the settings executable passes it, the command-line one does not).
// May throw std::bad_alloc; the entry points catch everything.
int AppMain(Mode mode, HRESULT hardened, std::span<const wchar_t* const> args,
            OpenWindowFn openWindow = nullptr);

}  // namespace pdft
