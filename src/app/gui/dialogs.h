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

}  // namespace pdft
