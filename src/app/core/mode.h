// Which of the two executables is running: they share all of their code (pdft_appcore) and
// differ in how they talk to the user and in a few commands.

#pragma once

namespace pdft {

enum class Mode {
  kGui,  // the settings executable (Windows subsystem)
  kCli,  // the command-line executable (console subsystem)
};

}  // namespace pdft
