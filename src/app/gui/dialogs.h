// The settings window's questions: task dialogs, which are accessible and follow the display
// scaling by themselves.

#pragma once

#include <windows.h>

#include <span>
#include <string>

namespace pdft {

// Asks before the .pdf slots are taken back from another handler; |slots| says what they hold now.
// True for yes.
bool ConfirmTakeBack(HWND owner, std::span<const std::wstring> slots);

enum class SaveAnswer { kSave, kDontSave, kCancel };

// Close with unsaved changes.
SaveAnswer AskSaveChanges(HWND owner);

// Close during a refresh: true to stop it and close.
bool ConfirmStopRefresh(HWND owner);

// Before a rebuild; |question| is the rebuild's own. True for "Rebuild now". *dontAskAgain tells
// whether its box was ticked.
bool ConfirmRebuild(HWND owner, const std::wstring& question, bool* dontAskAgain);

// Before an uninstall; |question| is the uninstall's own. True to go on. *clearCache tells whether
// the thumbnail cache is to be cleared too (ticked at first).
bool ConfirmUninstall(HWND owner, const std::wstring& question, bool* clearCache);

enum class ResultIcon { kInformation, kWarning, kError };

// What a child process ended with.
void ShowChildResult(HWND owner, ResultIcon icon, const std::wstring& title,
                     const std::wstring& text);

// Says what went wrong.
void ShowProblem(HWND owner, const std::wstring& text);

}  // namespace pdft
