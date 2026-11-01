// Running a parsed command line: the checks every command goes through, then the command.

#pragma once

#include <windows.h>

#include <string>
#include <string_view>
#include <vector>

#include "cli_args.h"
#include "context.h"
#include "install.h"
#include "mode.h"
#include "output.h"
#include "settings.h"
#include "uninstall.h"

namespace pdft {

// Opens the settings window and returns the process exit code when it closes. The settings
// executable passes one in (main_gui.cpp); the command-line executable has no window.
using OpenWindowFn = int (*)() noexcept;

// Runs |args| (parsed for |mode|) and returns the process exit code: an ExitCode, or for
// --elevated the HRESULT of the elevated step. In order:
//   --version, --help: print, exit 0 (no log, no context).
//   --elevated <step>: the elevated half of a change (ElevatedMain, elevation.h).
//   No command in the command-line executable: starts the settings executable when this
//   console is its own (started by a double-click), else prints the help.
//   --fix --take-back, and --uninstall, without --yes and without a console to ask on: exit 2
//   before anything is read (TakeBackNeedsYes, UninstallNeedsYes).
//   Everything else: reads the process context. A command that writes runs the registry canary
//   first and stops with exit 8 when the registry is (or may be) virtualized; on a PC where no
//   packaged app explains that, the refusal is logged. Then the log (ShouldLog), the installer
//   mutexes, and the command itself:
//     --status       read-only report (text or --json); exit 0, 1 or 6
//     --check        the check with the safe repairs (--no-repair: none); exit 0, 1, 4, 6, 7, 8
//     --fix          the same, and with --take-back the slots another handler holds (asks
//                    first); a damaged class for all users is repaired by the administrator
//                    step machine-repair-class (a prompt; declined: 3, failed: 5)
//     --checks       registers the user's check task with the triggers asked for, or deletes it
//                    (none); exit 0, 4, 5, 6 (not installed, or no settings executable for the
//                    task to run), 8 (also when elevated)
//     --diagnostics  the masked report on standard output, the clipboard (--copy) or a file
//                    (--out)
//     --setup-user, --setup-machine, --migrate-v1: the installer's steps and the 1.x clean-up
//                    (setup.h); --migrate-v1 asks for administrator rights for its part for all
//                    users
//     --install      (developer builds) installs from this executable's folder (install.h)
//     --uninstall    removes the product for all users (uninstall.h; asks first, or --yes)
//     --uninstall-cleanup  the uninstaller's step; always exit 0
//     --zoom         prints the zoom and where it comes from, or saves a new one (refused
//                    elevated: 8); the thumbnails already made are not refreshed by this build
//   Commands not built yet say so and exit 5.
// May throw std::bad_alloc; AppMain's callers catch everything.
int RunCommand(const CliArgs& args, Mode mode, Output& out, OpenWindowFn openWindow);

// "PdfThumbnails 2.0.0.0 (x64)": product, ProductVersion string, architecture.
std::wstring VersionText();

// Why a command that writes cannot run in |context| (pure): names the packaged ancestor when
// there is one, and whether the registry is virtualized or only possibly so. Without a packaged
// ancestor and with a canary that could not decide, it says that the check itself failed (with
// its HRESULT) instead: there is no container to leave then.
std::wstring CannotRunHereMessage(const ProcessContext& context);

// Why --check and --fix refuse an elevated process (pure): the repairs change only the user's
// own settings and files, which an administrator process does not write. Starts like
// CannotRunHereMessage ("Cannot run here: ").
std::wstring ElevatedRefusalMessage();

// Why --checks refuses an elevated process (pure): the check task belongs to the signed-in user
// and runs with their own rights. Starts like CannotRunHereMessage ("Cannot run here: ").
std::wstring ChecksElevatedRefusalMessage();

// Why --setup-user and --migrate-v1 (|command|) refuse an elevated process (pure): they change the
// signed-in user's own settings. Starts like CannotRunHereMessage ("Cannot run here: "). May throw
// std::bad_alloc.
std::wstring SetupElevatedRefusalMessage(std::wstring_view command);

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

// Taking slots back from another handler must be confirmed (pure): --fix --take-back needs
// --yes unless standard input is a console with a window to ask on. Scripts and tasks get a
// usage error (exit 2) before anything is read, whatever the slots hold.
bool TakeBackNeedsYes(const CliArgs& args, bool canAskOnConsole) noexcept;

// Why --install refused (pure), for every InstallRefusal but kWrongContext (CannotRunHereMessage)
// and kNone. Names the build's folder, and the object other accounts could change. May throw
// std::bad_alloc.
std::wstring InstallRefusalMessage(const InstallResult& result);

// Why --uninstall refuses an elevated process (pure). Starts like CannotRunHereMessage.
std::wstring UninstallElevatedRefusalMessage();

// What --uninstall prints after it ran (pure; not for a refusal, a busy lock or the installer's
// uninstaller): the thumbnail rebuild it cannot do, the part for all users, files left for the
// next sign-in, and the result. May throw std::bad_alloc.
std::vector<std::wstring> UninstallReport(const UninstallResult& result);

// "Zoom: 130 % (saved setting)", "(1.x's settings.ini)" or "(default)" (pure). May throw
// std::bad_alloc.
std::wstring ZoomText(const ZoomDecision& decision);

// What --zoom <n> prints once it saved the zoom (pure). Names no command this build lacks. May
// throw std::bad_alloc.
std::wstring ZoomSavedText(int zoom);

// --uninstall removes the product for all users, so it must be confirmed (pure): it needs --yes
// unless standard input is a console with a window to ask on.
bool UninstallNeedsYes(const CliArgs& args, bool canAskOnConsole) noexcept;

// The answer to a [y/N] question means yes (pure): "y" or "yes", ignoring case and surrounding
// white space. Anything else, nothing included, is no.
bool IsYesAnswer(std::wstring_view answer) noexcept;

}  // namespace pdft
