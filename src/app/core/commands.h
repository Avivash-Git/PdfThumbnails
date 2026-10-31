// Running a parsed command line: the checks every command goes through, then the command.

#pragma once

#include <windows.h>

#include <string>

#include "cli_args.h"
#include "context.h"
#include "mode.h"
#include "output.h"

namespace pdft {

// Opens the settings window and returns the process exit code when it closes. The settings
// executable passes one in (main_gui.cpp); the command-line executable has no window.
using OpenWindowFn = int (*)() noexcept;

// Runs |args| (parsed for |mode|) and returns the process exit code: an ExitCode, or for
// --elevated the HRESULT of the elevated step. In order:
//   --version, --help: print, exit 0 (no log, no context).
//   --elevated <step>: refused for now with E_NOTIMPL (the elevated steps come with installing).
//   No command in the command-line executable: starts the settings executable when this
//   console is its own (started by a double-click), else prints the help.
//   Everything else: reads the process context. A command that writes runs the registry canary
//   first and stops with exit 8 when the registry is (or may be) virtualized; on a PC where no
//   packaged app explains that, the refusal is logged. Then the log (ShouldLog), the installer
//   mutexes, and the command itself.
// May throw std::bad_alloc; AppMain's callers catch everything.
int RunCommand(const CliArgs& args, Mode mode, Output& out, OpenWindowFn openWindow);

// "PdfThumbnails 2.0.0.0 (x64)": product, ProductVersion string, architecture.
std::wstring VersionText();

// Why a command that writes cannot run in |context| (pure): names the packaged ancestor when
// there is one, and whether the registry is virtualized or only possibly so. Without a packaged
// ancestor and with a canary that could not decide, it says that the check itself failed (with
// its HRESULT) instead: there is no container to leave then.
std::wstring CannotRunHereMessage(const ProcessContext& context);

// Whether a command run in |context| writes the log (pure): never in an elevated process. The
// log folder lies in the user's profile, which the user's unelevated processes control: they
// could replace it with a link, and an elevated process that creates, appends to or renames
// files there would then do so wherever the link points. The unelevated side logs the outcome
// of elevated steps.
bool ShouldLog(const ProcessContext& context) noexcept;

// A command's first log line (pure): how it was started and what the registry check that ran
// found, nothing about the user: "start cli il=0x2000 elevated=no packaged=yes virtualized=yes"
// after the canary, "... views=agree" after the read-only check, and " probe-hr=0x..." when
// that check could not decide because something failed.
std::wstring StartMessage(Mode mode, const ProcessContext& context);

// The command-line executable started without a command owns its console alone exactly when it
// was double-clicked (pure): then it starts the settings executable instead of printing help
// into a window that closes at once. A console without a window (a hidden or detached start)
// does not count.
bool ShouldStartSettingsWindow(DWORD consoleProcessCount, bool hasConsoleWindow) noexcept;

}  // namespace pdft
