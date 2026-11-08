// The settings window: a modeless dialog with its own message loop. Its rules are in
// window_model.h and its work runs off this thread (window_jobs.h, program_run.h).

#pragma once

#include "commands.h"

namespace pdft {

// Opens the window and returns when it has closed.
WindowExit OpenMainWindow(const WindowStart& start) noexcept;

}  // namespace pdft
