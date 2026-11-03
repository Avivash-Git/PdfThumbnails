// Named objects the app's processes share: the settings window's single-instance mutex, the
// operation lock that lets one check or change run at a time, the log's mutex, and the mutexes
// an installer looks for to tell that the app is running.
//
// A user may run the app twice at once at different integrity levels: the settings window
// "as administrator" (High) and a scheduled check (Medium). An object an elevated process
// creates gets its token's default DACL, which grants only Administrators and SYSTEM, and a
// Medium process holds Administrators deny-only, so it could not open the object and would take
// it for missing. Every named object therefore gets an explicit descriptor (NamedObjectSddl),
// and ERROR_ACCESS_DENIED on opening one still means "another instance has it" (for the
// operation lock's Global\ name, only when its owner shows that this user made it: see
// OperationLock).
//
// Windows gives the same ERROR_ACCESS_DENIED for every name in a namespace that this process may
// not create objects in at all. A program started with another account's token inside someone
// else's session meets it, because Windows keeps that session's named objects for the session's
// own sign-in; so may a program at low integrity, in a sandbox or with a restricted token. No
// other instance has anything then, and waiting changes nothing, so CreateNamedMutex tells the
// two apart (ClassifyRefusedMutex) and returns kNamedObjectsRefusedHr for the second.
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
// two accounts on one PC never share one. Any account may still make another user's Global\ name
// first, with a descriptor of its own: OperationLock::Acquire therefore always takes a name in
// this session too, and the lock's global name only when this user made it.
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
// (creating a Global mutex needs no privilege, unlike a Global file mapping). Because of that,
// any other account may make the name first, so the lock also takes this session's name of it,
// OperationSessionLockName, which no other account can make (see OperationLock).
std::wstring SettingsInstanceMutexName(std::wstring_view userSid);  // Local\<prefix>.Settings.<SID>
std::wstring OperationLockName(std::wstring_view userSid);  // Global\<prefix>.Operation.<SID>
std::wstring OperationSessionLockName(std::wstring_view userSid);  // Local\<prefix>.Operation.<SID>
std::wstring LogMutexName(std::wstring_view userSid);              // Local\<prefix>.Log.<SID>
std::wstring AppMutexName(bool global);                            // <prefix>AppMutex, Global\...

// For tests: appends |suffix| to each of the names above (empty: the real names again), so a
// test run never takes, or waits for, the objects of a running app or of another test run.
// Call it before anything uses the names; it is not synchronized with their use. The app
// itself never calls it.
void SetObjectNameSuffixForTesting(std::wstring_view suffix) noexcept;

// What CreateNamedMutex, TakeSingleInstance, AppMutexes::Hold and OperationLock::Acquire return
// when Windows does not let this process create named objects in the namespace of the name: not
// "another instance has it", and nothing that waiting would change. The commands that take the
// operation lock say so and exit 5 (exit_code.h). A code of this app's own (FACILITY_ITF), not a
// Windows error, so that no call on the way can return it.
inline constexpr HRESULT kNamedObjectsRefusedHr = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x2E5);

// What a refused creation of a named mutex means. CreateMutexW fails with ERROR_ACCESS_DENIED
// both when the mutex exists with a descriptor that keeps this process from opening it fully and
// when this process may not create named objects in that namespace at all; what comes next tells
// them apart.
enum class RefusedMutex {
  kExists,           // the mutex exists: another instance has it
  kProbe,            // not decided yet: create a probe in the namespace, then ask again
  kRetry,            // it existed and is gone now: create it again
  kNamespaceClosed,  // this process may not create named objects there (kNamedObjectsRefusedHr)
  kFailed,           // the open or the probe failed for another reason (its error)
};

// The meaning of a refused creation (pure), from |openError|, what opening the same name to wait
// on it gave (ERROR_SUCCESS: it opened), and |probe|, what creating a mutex of a new name in the
// same namespace gave (S_OK: it was made), std::nullopt while that was not tried:
//   opened                                    kExists
//   neither ERROR_FILE_NOT_FOUND nor denied   kFailed
//   not there, or denied; no probe yet        kProbe
//   probe E_ACCESSDENIED                      kNamespaceClosed
//   probe failed otherwise                    kFailed
//   probe made, the open denied               kExists (it refuses this process even waiting)
//   probe made, the name not there            kRetry (it went away in between)
RefusedMutex ClassifyRefusedMutex(DWORD openError, std::optional<HRESULT> probe) noexcept;

// Creates the mutex |name| (not owned) with NamedObjectSddl(|userSid|, |everyoneSynchronize|),
// or opens it if it exists; *existed says which. When it exists but this process may not open
// it fully (another instance created it with a descriptor that does not include this process),
// *existed is true and it is opened with |fallbackAccess| instead; with 0, or if that fails too,
// *mutex stays empty. kNamedObjectsRefusedHr, with *existed false, when Windows does not let this
// process create named objects in the namespace of |name| (ClassifyRefusedMutex tells that from
// the case before; the probe is a mutex of a new, unguessable name with the same descriptor,
// closed again at once). HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED), logged, when the creation was
// refused each time though the probe was made and the name was not there: nothing then shows
// that another instance has it.
[[nodiscard]] HRESULT CreateNamedMutex(std::wstring_view name, std::wstring_view userSid,
                                       bool everyoneSynchronize, DWORD fallbackAccess,
                                       wil::unique_handle* mutex, bool* existed) noexcept;

// The settings window's single-instance mutex. *first is false when another settings window
// of this user already runs. Keep |mutex| open for as long as the window is.
// kNamedObjectsRefusedHr: this process cannot tell whether another window runs.
[[nodiscard]] HRESULT TakeSingleInstance(std::wstring_view userSid, wil::unique_handle* mutex,
                                         bool* first) noexcept;

// The fixed-name mutexes that an installer checks before it replaces files (Inno Setup's
// AppMutex): <prefix>AppMutex in this session and Global\<prefix>AppMutex across sessions.
// Held by the settings window and by command-line and scheduled runs. Everyone may wait on them.
class AppMutexes {
 public:
  // Creates or opens both and keeps them open until this object goes away. Returns the first
  // failure (kNamedObjectsRefusedHr when this process may not create named objects in a
  // mutex's namespace); a mutex that could not be opened is simply not held.
  [[nodiscard]] HRESULT Hold(std::wstring_view userSid) noexcept;

 private:
  wil::unique_handle session_;
  wil::unique_handle global_;
};

// Whether a named object owned by |ownerSid| can have been made by a process of the user
// |userSid| (pure): its owner is that user, Administrators (the owner of what an elevated process
// of the user makes) or SYSTEM. A standard account can make none of these the owner of what it
// makes, so another standard account's object never passes.
bool MadeByThisUser(std::wstring_view ownerSid, std::wstring_view userSid) noexcept;

// The operation lock: one check, fix, save, refresh, install, uninstall or migration of a user
// at a time, across processes and sessions. Owned by the thread that acquired it, which must
// also be the one that destroys it (a mutex is released by its owner).
//
// It is two mutexes, which every process takes in the same order, so no two of them wait for each
// other the wrong way round. First this session's (OperationSessionLockName): no other account's
// sign-in can make names in this session, so the user's processes in one session always keep
// each other out. Then the global one (OperationLockName), which also keeps out the user's
// processes in their other sessions. Any account may make that name first, with any descriptor,
// and so keep the user's checks and changes out, so Acquire holds a global mutex that already
// exists only when this user made it (MadeByThisUser, from its owner). When another account made
// it, or made it with a descriptor that keeps this user from reading its owner and waiting on it,
// or the name is no mutex at all (an object of another type, or a link that leads anywhere), or
// it cannot be made or opened for any other reason, Acquire holds this session's lock alone
// (sessionOnly()). In session 0 both names are one mutex, which Acquire then holds twice.
class OperationLock {
 public:
  // What Acquire returns when another process held the lock for the whole wait, and when the
  // lock cannot be had at all (see Acquire).
  static constexpr HRESULT kBusy = __HRESULT_FROM_WIN32(ERROR_BUSY);
  // What Acquire returns when the lock's name belongs to an object that a process of this user
  // below Medium integrity created (before any of the app's processes did): the label keeps such
  // processes from opening the app's own lock, but whoever creates a name first owns it. Waiting
  // on that object would let the other process decide when checks run, so Acquire never does.
  static constexpr HRESULT kTakenByLowerIntegrity = __HRESULT_FROM_WIN32(ERROR_INVALID_LABEL);

  // Waits up to |waitMs| in all for the lock. S_OK: *lock holds it. kBusy: another process still
  // has it; also, at once, when another account made the global name and this session's name
  // cannot be had (in session 0, whose names are the global ones, or when this process may not
  // make names in its session). kTakenByLowerIntegrity: see above. kNamedObjectsRefusedHr:
  // Windows lets this process create neither name (nothing was waited for; no other process holds
  // anything). A holder that ended without releasing it counts as released (abandoned() is then
  // true).
  [[nodiscard]] static HRESULT Acquire(DWORD waitMs, std::optional<OperationLock>* lock) noexcept;

  OperationLock(OperationLock&&) noexcept = default;
  OperationLock& operator=(OperationLock&&) = delete;
  OperationLock(const OperationLock&) = delete;
  OperationLock& operator=(const OperationLock&) = delete;
  ~OperationLock();

  // The previous holder ended (or crashed) while holding the lock: what it was doing may be
  // half done.
  bool abandoned() const noexcept { return abandoned_; }

  // This is this session's lock alone (OperationSessionLockName): the global name is not this
  // user's, or this process may not make it. The user's processes in other sessions are not kept
  // out while it is held, which is worth a line in the log.
  bool sessionOnly() const noexcept { return sessionOnly_; }

 private:
  OperationLock(wil::unique_handle session, wil::unique_handle global, bool abandoned,
                bool sessionOnly) noexcept;

  wil::unique_handle session_;  // empty when this process may not make names in its session
  wil::unique_handle global_;   // empty when sessionOnly()
  bool abandoned_ = false;
  bool sessionOnly_ = false;
};

}  // namespace pdft
