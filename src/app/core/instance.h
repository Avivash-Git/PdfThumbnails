// Named objects the app's processes share: the settings window's single-instance mutex, the
// operation lock that lets one check or change run at a time, the log's mutex, and the mutexes
// an installer looks for to tell that the app is running.
//
// A user may run the app twice at once at different integrity levels: the settings window
// "as administrator" (High) and a scheduled check (Medium). An object an elevated process
// creates gets its token's default DACL, which grants only Administrators and SYSTEM, and a
// Medium process holds Administrators deny-only, so it could not open the object and would take
// it for missing. Every named object therefore gets an explicit descriptor (NamedObjectSddl),
// and ERROR_ACCESS_DENIED on opening one still means "another instance has it".
//
// Not here yet, because they need the settings window: bringing the running window to the
// front when a second instance starts, and telling it that a check changed the state.

#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <optional>
#include <string>
#include <string_view>

namespace pdft {

// This process's user as a SID string ("S-1-5-21-...-1001"); named objects carry it so that
// two accounts on one PC never share or block each other's objects.
[[nodiscard]] HRESULT GetCurrentUserSid(std::wstring* sid) noexcept;

// The descriptor of the app's named objects (pure): full access for |userSid|, SYSTEM and
// Administrators, and a Medium integrity label with no-write-up, no-read-up and no-execute-up,
// so a Medium and a High process of the user can both open it and a process of the same user
// below Medium (a sandboxed browser part, say) cannot open it at all. No-execute-up matters
// most: waiting on a mutex needs SYNCHRONIZE, which a mutex counts as execute access, so with
// no-write-up alone a Low process could wait on the operation lock, own it and keep every check
// out. With |everyoneSynchronize|, everyone may also wait on it (an installer started by another
// account checks for it).
std::wstring NamedObjectSddl(std::wstring_view userSid, bool everyoneSynchronize);

// The names (pure). The operation lock lives in the Global namespace: the same user signed in
// at the console and in a remote session shares one HKCU, so both sessions must see one lock
// (creating a Global mutex needs no privilege, unlike a Global file mapping).
std::wstring SettingsInstanceMutexName(std::wstring_view userSid);  // Local\<prefix>.Settings.<SID>
std::wstring OperationLockName(std::wstring_view userSid);  // Global\<prefix>.Operation.<SID>
std::wstring LogMutexName(std::wstring_view userSid);       // Local\<prefix>.Log.<SID>
std::wstring AppMutexName(bool global);                     // <prefix>AppMutex, Global\...

// For tests: appends |suffix| to each of the names above (empty: the real names again), so a
// test run never takes, or waits for, the objects of a running app or of another test run.
// Call it before anything uses the names; it is not synchronized with their use. The app
// itself never calls it.
void SetObjectNameSuffixForTesting(std::wstring_view suffix) noexcept;

// Creates the mutex |name| (not owned) with NamedObjectSddl(|userSid|, |everyoneSynchronize|),
// or opens it if it exists; *existed says which. When it exists but this process may not open
// it fully (another instance created it with a descriptor that does not include this process),
// *existed is true and it is opened with |fallbackAccess| instead; with 0, or if that fails too,
// *mutex stays empty.
[[nodiscard]] HRESULT CreateNamedMutex(std::wstring_view name, std::wstring_view userSid,
                                       bool everyoneSynchronize, DWORD fallbackAccess,
                                       wil::unique_handle* mutex, bool* existed) noexcept;

// The settings window's single-instance mutex. *first is false when another settings window
// of this user already runs. Keep |mutex| open for as long as the window is.
[[nodiscard]] HRESULT TakeSingleInstance(std::wstring_view userSid, wil::unique_handle* mutex,
                                         bool* first) noexcept;

// The fixed-name mutexes that an installer checks before it replaces files (Inno Setup's
// AppMutex): <prefix>AppMutex in this session and Global\<prefix>AppMutex across sessions.
// Held by the settings window and by command-line and scheduled runs. Everyone may wait on them.
class AppMutexes {
 public:
  // Creates or opens both and keeps them open until this object goes away. Returns the first
  // failure; a mutex that could not be opened is simply not held.
  [[nodiscard]] HRESULT Hold(std::wstring_view userSid) noexcept;

 private:
  wil::unique_handle session_;
  wil::unique_handle global_;
};

// The operation lock: one check, fix, save, refresh, install, uninstall or migration of a user
// at a time, across processes and sessions. Owned by the thread that acquired it, which must
// also be the one that destroys it (a mutex is released by its owner).
class OperationLock {
 public:
  // What Acquire returns when another process held the lock for the whole wait.
  static constexpr HRESULT kBusy = __HRESULT_FROM_WIN32(ERROR_BUSY);
  // What Acquire returns when the lock's name belongs to an object that a process below Medium
  // integrity created (before any of the app's processes did): the label keeps such processes
  // from opening the app's own lock, but whoever creates a name first owns it. Waiting on that
  // object would let the other process decide when checks run, so Acquire never does.
  static constexpr HRESULT kTakenByLowerIntegrity = __HRESULT_FROM_WIN32(ERROR_INVALID_LABEL);

  // Waits up to |waitMs| for the lock. S_OK: *lock holds it. kBusy: another process still has
  // it. kTakenByLowerIntegrity: see above. A holder that ended without releasing it counts as
  // released (abandoned() is then true).
  [[nodiscard]] static HRESULT Acquire(DWORD waitMs, std::optional<OperationLock>* lock) noexcept;

  OperationLock(OperationLock&&) noexcept = default;
  OperationLock& operator=(OperationLock&&) = delete;
  OperationLock(const OperationLock&) = delete;
  OperationLock& operator=(const OperationLock&) = delete;
  ~OperationLock();

  // The previous holder ended (or crashed) while holding the lock: what it was doing may be
  // half done.
  bool abandoned() const noexcept { return abandoned_; }

 private:
  OperationLock(wil::unique_handle mutex, bool abandoned) noexcept;

  wil::unique_handle mutex_;
  bool abandoned_ = false;
};

}  // namespace pdft
