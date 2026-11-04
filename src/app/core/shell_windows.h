// File Explorer's folder windows, seen from another process through the shell's ShellWindows
// object. Every call runs on a thread of its own (a single-threaded apartment) and waits at most
// |timeoutMs|, so a hung File Explorer cannot hang the caller: after a timeout the thread finishes
// on its own and owns everything it uses. Nothing here runs while no shell window exists
// (HRESULT_FROM_WIN32(ERROR_NOT_FOUND)): COM would otherwise start a ShellWindows of its own
// (shell32 in a rundll32 process), not File Explorer's. ShellWindows runs as the session's
// signed-in user whatever this process's token, so the windows listed and opened are always that
// user's: a command that acts for this process's user refuses first when the shell is another
// user's (ProcessContext::differentFromShellUser). Folder names stay in memory: never logged,
// never printed.

#pragma once

#include <windows.h>

#include <shobjidl.h>

#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace pdft {

struct FolderWindow {
  std::wstring parsingName;  // SIGDN_DESKTOPABSOLUTEPARSING of the folder shown; empty if none
  ULONG_PTR frame = 0;       // the top-level window (several tabs share one)
};

// S_OK with the windows; S_FALSE when the shell did not answer in time (*windows empty).
[[nodiscard]] HRESULT ListFolderWindows(DWORD timeoutMs,
                                        std::vector<FolderWindow>* windows) noexcept;

// Asks the running shell to open each folder in a window of its own (IShellDispatch2::Open on the
// desktop's folder view, so the window is opened inside explorer.exe, never by a child of this
// process). Windows without a parsing name are counted as failed, and so is a folder the shell
// does not open. *opened and *failed always add up to the windows given: S_OK when every one was
// tried; S_FALSE when the shell did not answer in time, and the windows not opened by then count
// as failed and are no longer tried (the one being opened may still appear); a failure when the
// desktop's folder view cannot be reached (nothing opened).
[[nodiscard]] HRESULT OpenFolderWindows(std::span<const FolderWindow> windows, DWORD timeoutMs,
                                        size_t* opened, size_t* failed) noexcept;

// Calls |action| for every view whose folder is one of |folders| (whole paths, ignoring case, as
// SameImagePath compares them: pass them as Windows shows them, in Canonicalize's form, since a
// short 8.3 name does not match), on the watchdog thread; |desktop| adds the desktop's own view.
// |found| receives how many. S_FALSE when the shell did not answer in time: no further call
// starts, but the one under way may still finish on that thread, so |action|, which is copied
// there, must own what it uses (capture by value, never the caller's locals by reference). A
// failure when ShellWindows, or with |desktop| the desktop's view, cannot be reached, or when
// |action| throws.
using ViewAction = std::function<void(IShellBrowser*, IShellView*)>;
[[nodiscard]] HRESULT ForEachViewShowing(std::span<const std::filesystem::path> folders,
                                         bool desktop, DWORD timeoutMs, const ViewAction& action,
                                         size_t* found) noexcept;

}  // namespace pdft
