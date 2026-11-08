#include "window_messages.h"

#include <wil/resource.h>

#include <string>
#include <vector>

#include "instance.h"

namespace pdft {
namespace {

// The token user of |token|, in |buffer|; nullptr when it cannot be read.
PSID TokenUserSid(HANDLE token, std::vector<BYTE>* buffer) {
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size == 0) return nullptr;
  buffer->resize(size);
  if (!GetTokenInformation(token, TokenUser, buffer->data(), size, &size)) return nullptr;
  return reinterpret_cast<TOKEN_USER*>(buffer->data())->User.Sid;
}

// Whether process |pid| runs as this process's user.
bool RunsAsThisUser(DWORD pid) {
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (!process) return false;
  wil::unique_handle theirToken;
  wil::unique_handle ownToken;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, theirToken.put()) ||
      !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, ownToken.put())) {
    return false;
  }
  std::vector<BYTE> theirs;
  std::vector<BYTE> own;
  const PSID theirSid = TokenUserSid(theirToken.get(), &theirs);
  const PSID ownSid = TokenUserSid(ownToken.get(), &own);
  return theirSid && ownSid && EqualSid(theirSid, ownSid);
}

// This user's settings window on this desktop, and its process; nullptr when there is none.
HWND FindOwnWindow(DWORD* pid) {
  const std::wstring className = SettingsWindowClassName();
  HWND window = nullptr;
  while ((window = FindWindowExW(nullptr, window, className.c_str(), nullptr)) != nullptr) {
    DWORD owner = 0;
    if (GetWindowThreadProcessId(window, &owner) && RunsAsThisUser(owner)) {
      *pid = owner;
      return window;
    }
  }
  return nullptr;
}

}  // namespace

UINT StateChangedMessage() noexcept try {
  return RegisterWindowMessageW(StateChangedMessageName().c_str());
} catch (...) {
  return 0;
}

HRESULT ActivateExistingWindow() noexcept try {
  DWORD pid = 0;
  const HWND window = FindOwnWindow(&pid);
  if (!window) return S_FALSE;
  // Only the process in the foreground may hand the foreground on, and that is this one.
  (void)AllowSetForegroundWindow(pid);
  return PostMessageW(window, kActivateWindowMessage, 0, 0) ? S_OK
                                                            : HRESULT_FROM_WIN32(GetLastError());
} catch (...) {
  return E_OUTOFMEMORY;
}

void NotifyStateChanged() noexcept try {
  const UINT message = StateChangedMessage();
  if (message == 0) return;
  DWORD pid = 0;
  if (const HWND window = FindOwnWindow(&pid)) (void)PostMessageW(window, message, 0, 0);
} catch (...) {
  // Out of memory: the window finds out at its next read.
}

}  // namespace pdft
