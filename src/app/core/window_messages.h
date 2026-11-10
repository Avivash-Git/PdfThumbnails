// How the app's processes reach the settings window that is already open: a second start brings
// it to the front, and a command that changed something tells it to read the state again.
//
// Only a window of this process's own user counts. Windows of one class name can belong to
// several accounts on one desktop ("Run as different user"), and each account has its own settings
// window (the single-instance mutex carries the user's SID). Any process may register a window of
// that class name, so a process of the same user below this one's integrity level (a sandboxed
// part of a browser, say) does not count either: it would be handed the foreground and the
// messages meant for the real window.

#pragma once

#include <windows.h>

namespace pdft {

// Posted to the window by a second start: restore yourself and come to the front.
inline constexpr UINT kActivateWindowMessage = WM_APP + 1;

// Whether a window of the settings window's class may be taken for this user's settings window
// (pure): its process runs as this process's user (|sameUser|) at this process's integrity level
// (|ownIntegrityRid|, SECURITY_MANDATORY_*_RID) or above.
bool IsOwnWindowProcess(bool sameUser, DWORD theirIntegrityRid, DWORD ownIntegrityRid) noexcept;

// The registered message that says the state changed (StateChangedMessageName, instance.h); 0
// when Windows cannot register it.
UINT StateChangedMessage() noexcept;

// Lets the open settings window of this user come to the front and asks it to. S_OK when one was
// found and asked, S_FALSE when none was found (it may be just closing or still starting).
[[nodiscard]] HRESULT ActivateExistingWindow() noexcept;

// Tells the open settings window of this user, if there is one, that a check or change ended.
// Best effort: it never fails the command that calls it.
void NotifyStateChanged() noexcept;

}  // namespace pdft
