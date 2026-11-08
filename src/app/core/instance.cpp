#include "instance.h"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <wil/result_macros.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "project_config.h"

namespace pdft {
namespace {

// How often CreateNamedMutex creates a name again that was gone after a refused creation, and
// OpenLockMutex one that was gone before its owner could be read.
constexpr int kCreateAttempts = 3;

// The owners of what an elevated process of the user and the system make ("BA" and "SY").
constexpr std::wstring_view kAdministratorsSid = L"S-1-5-32-544";
constexpr std::wstring_view kSystemSid = L"S-1-5-18";

// Appended to every object name (SetObjectNameSuffixForTesting); empty outside tests. Created
// on first use, not before main.
std::wstring& NameSuffix() {
  static std::wstring suffix;
  return suffix;
}

// The namespace |name| is in, as its prefix up to and with its last backslash ("Global\",
// "Local\"); empty for a name without one, which is in this session's own namespace.
std::wstring_view NamespaceOf(std::wstring_view name) noexcept {
  const size_t slash = name.rfind(L'\\');
  return slash == std::wstring_view::npos ? std::wstring_view() : name.substr(0, slash + 1);
}

// Creates a mutex of a new name nobody can have guessed in the namespace |prefix| (NamespaceOf),
// with the same |attributes| as the mutex whose creation was refused, so that it asks Windows the
// same question but for the name, and closes it again: S_OK when it was made, E_ACCESSDENIED
// when this process may not create such a mutex there.
HRESULT ProbeNamespace(std::wstring_view prefix, SECURITY_ATTRIBUTES* attributes) noexcept try {
  // A name that exists all the same proves nothing about creating: another one then.
  for (int attempt = 0; attempt < kCreateAttempts; ++attempt) {
    GUID guid = {};
    RETURN_IF_FAILED(CoCreateGuid(&guid));
    wchar_t text[40] = {};
    RETURN_HR_IF(E_UNEXPECTED, StringFromGUID2(guid, text, ARRAYSIZE(text)) == 0);
    const std::wstring name = std::wstring(prefix) + kObjectPrefix + L".Probe." + text;
    const wil::unique_handle probe(CreateMutexExW(attributes, name.c_str(), 0, SYNCHRONIZE));
    const DWORD error = GetLastError();
    if (!probe) return HRESULT_FROM_WIN32(error);
    if (error != ERROR_ALREADY_EXISTS) return S_OK;
  }
  RETURN_WIN32(ERROR_ALREADY_EXISTS);
}
CATCH_RETURN();

// Whether the object's integrity label is below Medium. An object without a label counts as
// Medium, as Windows treats it.
HRESULT LabelIsBelowMedium(HANDLE object, bool* below) noexcept {
  *below = false;
  PACL sacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD error = GetSecurityInfo(object, SE_KERNEL_OBJECT, LABEL_SECURITY_INFORMATION, nullptr,
                                      nullptr, nullptr, &sacl, &descriptor);
  RETURN_IF_WIN32_ERROR(error);
  wil::unique_hlocal freeDescriptor(descriptor);
  DWORD rid = SECURITY_MANDATORY_MEDIUM_RID;
  for (DWORD i = 0; sacl && i < sacl->AceCount; ++i) {
    ACE_HEADER* ace = nullptr;
    if (!GetAce(sacl, i, reinterpret_cast<void**>(&ace)) ||
        ace->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
      continue;
    }
    PSID label = &reinterpret_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace)->SidStart;
    const UCHAR count = *GetSidSubAuthorityCount(label);
    if (count > 0) rid = *GetSidSubAuthority(label, count - 1u);
  }
  *below = rid < SECURITY_MANDATORY_MEDIUM_RID;
  return S_OK;
}

// The owner of |object| (a handle with READ_CONTROL) as a SID string; empty when it has none.
HRESULT GetOwnerSid(HANDLE object, std::wstring* ownerSid) noexcept try {
  ownerSid->clear();
  PSID owner = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD error = GetSecurityInfo(object, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                                      nullptr, nullptr, nullptr, &descriptor);
  RETURN_IF_WIN32_ERROR(error);
  wil::unique_hlocal freeDescriptor(descriptor);
  if (!owner) return S_OK;
  wil::unique_hlocal_string text;
  RETURN_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(owner, text.put()));
  *ownerSid = text.get();
  return S_OK;
}
CATCH_RETURN();

// What one of the operation lock's names is to this process.
enum class LockMutex {
  kUsable,          // made now, or made by this user: wait on it
  kAnotherAccount,  // another account's, one that keeps this user out, or no mutex at all
  kBusy,            // this user's, but this process may not wait on it (OperationLock::kBusy)
  kLowerIntegrity,  // this user's, made below Medium (OperationLock::kTakenByLowerIntegrity)
  kRefused,         // this process may not create named objects there (kNamedObjectsRefusedHr)
};

// Creates the operation lock's mutex |name| for the user |userSid|, or opens it if it exists, and
// says what it is to this process (*found); *mutex holds it when that is kUsable. A mutex that
// exists counts as this user's only when its owner says so (MadeByThisUser): anyone may make a
// Global\ name first, but no other standard account can give what it makes such an owner. The
// app's descriptor lets the user read the owner and wait on the mutex, so a mutex whose
// descriptor refuses this user that much was not made for this user by the app.
HRESULT OpenLockMutex(const std::wstring& name, const std::wstring& userSid,
                      wil::unique_handle* mutex, LockMutex* found) noexcept try {
  mutex->reset();
  *found = LockMutex::kAnotherAccount;
  for (int attempt = 0; attempt < kCreateAttempts; ++attempt) {
    wil::unique_handle full;
    bool existed = false;
    const HRESULT made = CreateNamedMutex(name, userSid, false, 0, std::addressof(full), &existed);
    if (made == kNamedObjectsRefusedHr) {
      *found = LockMutex::kRefused;
      return S_OK;
    }
    // The name is no mutex that this user may make or open: an object of another type
    // (ERROR_INVALID_HANDLE), or an object manager link that leads where no mutex can be made
    // (the others). No instance of this user made that either.
    if (made == HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) ||
        made == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
        made == HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME) ||
        made == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
      return S_OK;  // kAnotherAccount
    }
    RETURN_IF_FAILED(made);
    if (!existed) {
      *mutex = std::move(full);
      *found = LockMutex::kUsable;
      return S_OK;
    }
    // It exists, and it was opened fully when its descriptor let this process. When not, a
    // handle that may only read who owns it (and wait on it) tells whose it is.
    wil::unique_handle readOnly;
    if (!full) {
      readOnly.reset(OpenMutexW(READ_CONTROL | SYNCHRONIZE, FALSE, name.c_str()));
      if (!readOnly) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND) continue;    // gone in between: make it again
        if (error == ERROR_ACCESS_DENIED) return S_OK;  // keeps this user out: kAnotherAccount
        RETURN_WIN32(error);
      }
    }
    std::wstring owner;
    RETURN_IF_FAILED(GetOwnerSid(full ? full.get() : readOnly.get(), &owner));
    if (!MadeByThisUser(owner, userSid)) return S_OK;  // kAnotherAccount
    if (!full) {
      // This user's, with a descriptor that keeps this process from holding it: another
      // instance has it, as before the owner was read.
      *found = LockMutex::kBusy;
      return S_OK;
    }
    bool belowMedium = false;
    RETURN_IF_FAILED(LabelIsBelowMedium(full.get(), &belowMedium));
    if (belowMedium) {
      *found = LockMutex::kLowerIntegrity;
      return S_OK;
    }
    *mutex = std::move(full);
    *found = LockMutex::kUsable;
    return S_OK;
  }
  // There each time, and gone again each time before its owner could be read: something keeps
  // making and closing a name that this user may not open fully, which no instance of this user
  // does, so it counts as another account's (kAnotherAccount).
  return S_OK;
}
CATCH_RETURN();

// Waits up to |waitMs| for |mutex|: S_OK when this thread owns it now (*abandoned set when its
// holder ended without releasing it), OperationLock::kBusy when another holder kept it all along.
HRESULT WaitForLock(HANDLE mutex, DWORD waitMs, bool* abandoned) noexcept {
  const DWORD waited = WaitForSingleObject(mutex, waitMs);
  if (waited == WAIT_OBJECT_0) return S_OK;
  if (waited == WAIT_ABANDONED) {
    // The holder ended without releasing it; this thread owns it now.
    *abandoned = true;
    return S_OK;
  }
  if (waited == WAIT_TIMEOUT) return OperationLock::kBusy;
  RETURN_LAST_ERROR();
}

// What is left of |waitMs| (INFINITE stays INFINITE) of a wait that began at |start|.
DWORD RemainingWait(DWORD waitMs, ULONGLONG start) noexcept {
  if (waitMs == INFINITE) return INFINITE;
  const ULONGLONG spent = GetTickCount64() - start;
  return spent >= waitMs ? 0 : static_cast<DWORD>(waitMs - spent);
}

}  // namespace

void SetObjectNameSuffixForTesting(std::wstring_view suffix) noexcept {
  try {
    NameSuffix() = suffix;
  } catch (...) {
    NameSuffix().clear();
  }
}

HRESULT GetCurrentUserSid(std::wstring* sid) noexcept try {
  sid->clear();
  wil::unique_handle token;
  RETURN_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()));
  alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  RETURN_IF_WIN32_BOOL_FALSE(
      GetTokenInformation(token.get(), TokenUser, buffer, sizeof(buffer), &size));
  wil::unique_hlocal_string text;
  RETURN_IF_WIN32_BOOL_FALSE(
      ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, text.put()));
  *sid = text.get();
  return S_OK;
}
CATCH_RETURN();

std::wstring NamedObjectSddl(std::wstring_view userSid, bool everyoneSynchronize) {
  std::wstring sddl = L"D:(A;;GA;;;";
  sddl += userSid;
  sddl += L")(A;;GA;;;SY)(A;;GA;;;BA)";
  if (everyoneSynchronize) sddl += L"(A;;0x00100000;;;WD)";  // SYNCHRONIZE
  sddl += L"S:(ML;;NWNRNX;;;ME)";
  return sddl;
}

std::wstring SettingsInstanceMutexName(std::wstring_view userSid) {
  return std::wstring(L"Local\\") + kObjectPrefix + L".Settings." + std::wstring(userSid) +
         NameSuffix();
}

std::wstring OperationLockName(std::wstring_view userSid) {
  return std::wstring(L"Global\\") + kObjectPrefix + L".Operation." + std::wstring(userSid) +
         NameSuffix();
}

std::wstring OperationSessionLockName(std::wstring_view userSid) {
  return std::wstring(L"Local\\") + kObjectPrefix + L".Operation." + std::wstring(userSid) +
         NameSuffix();
}

std::wstring LogMutexName(std::wstring_view userSid) {
  return std::wstring(L"Local\\") + kObjectPrefix + L".Log." + std::wstring(userSid) + NameSuffix();
}

std::wstring AppMutexName(bool global) {
  return std::wstring(global ? L"Global\\" : L"") + kObjectPrefix + L"AppMutex" + NameSuffix();
}

std::wstring SettingsWindowClassName() {
  return std::wstring(kObjectPrefix) + L".SettingsWindow" + NameSuffix();
}

std::wstring StateChangedMessageName() {
  return std::wstring(kObjectPrefix) + L".StateChanged" + NameSuffix();
}

RefusedMutex ClassifyRefusedMutex(DWORD openError, std::optional<HRESULT> probe) noexcept {
  if (openError == ERROR_SUCCESS) return RefusedMutex::kExists;
  if (openError != ERROR_FILE_NOT_FOUND && openError != ERROR_ACCESS_DENIED) {
    return RefusedMutex::kFailed;
  }
  // Not there, or not even to be waited on: whether this process may create anything in that
  // namespace decides.
  if (!probe) return RefusedMutex::kProbe;
  if (*probe == E_ACCESSDENIED) return RefusedMutex::kNamespaceClosed;
  if (FAILED(*probe)) return RefusedMutex::kFailed;
  return openError == ERROR_ACCESS_DENIED ? RefusedMutex::kExists : RefusedMutex::kRetry;
}

HRESULT CreateNamedMutex(std::wstring_view name, std::wstring_view userSid,
                         bool everyoneSynchronize, DWORD fallbackAccess, wil::unique_handle* mutex,
                         bool* existed) noexcept try {
  mutex->reset();
  *existed = false;
  wil::unique_hlocal_security_descriptor descriptor;
  RETURN_IF_WIN32_BOOL_FALSE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      NamedObjectSddl(userSid, everyoneSynchronize).c_str(), SDDL_REVISION_1, descriptor.put(),
      nullptr));
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor.get(), FALSE};
  const std::wstring nameText(name);
  for (int attempt = 0; attempt < kCreateAttempts; ++attempt) {
    HANDLE handle = CreateMutexW(&attributes, FALSE, nameText.c_str());
    const DWORD error = GetLastError();
    if (handle) {
      mutex->reset(handle);
      *existed = error == ERROR_ALREADY_EXISTS;
      return S_OK;
    }
    RETURN_HR_IF(HRESULT_FROM_WIN32(error), error != ERROR_ACCESS_DENIED);
    // Refused: the mutex exists, made by a process whose descriptor does not let this one open
    // it with full access (another instance has it), or this process may not create named
    // objects in this namespace at all. Opening it, and where that says nothing a probe, tells.
    wil::unique_handle opened(
        OpenMutexW(fallbackAccess ? fallbackAccess : SYNCHRONIZE, FALSE, nameText.c_str()));
    const DWORD openError = opened ? ERROR_SUCCESS : GetLastError();
    std::optional<HRESULT> probe;
    RefusedMutex meaning = ClassifyRefusedMutex(openError, probe);
    if (meaning == RefusedMutex::kProbe) {
      probe = ProbeNamespace(NamespaceOf(name), &attributes);
      meaning = ClassifyRefusedMutex(openError, probe);
    }
    switch (meaning) {
      case RefusedMutex::kExists:
        *existed = true;
        if (fallbackAccess) *mutex = std::move(opened);
        return S_OK;
      case RefusedMutex::kNamespaceClosed:
        // An answer, not a failure of this function: the callers say so, WIL does not log it.
        return kNamedObjectsRefusedHr;
      case RefusedMutex::kFailed:
        RETURN_HR(probe && FAILED(*probe) ? *probe : HRESULT_FROM_WIN32(openError));
      case RefusedMutex::kRetry:
      case RefusedMutex::kProbe:
        break;
    }
  }
  // Refused each time, though a probe could be made, and gone again at each open after it. Some
  // other process may keep making and closing the name, or the name leads somewhere the probe's
  // does not (an object manager link): either way nothing shows that another instance has the
  // mutex, so this is a failure (logged), not "busy".
  RETURN_WIN32(ERROR_ACCESS_DENIED);
}
CATCH_RETURN();

HRESULT TakeSingleInstance(std::wstring_view userSid, wil::unique_handle* mutex,
                           bool* first) noexcept try {
  *first = false;
  bool existed = false;
  const HRESULT made = CreateNamedMutex(SettingsInstanceMutexName(userSid), userSid, false,
                                        SYNCHRONIZE, mutex, &existed);
  if (made == kNamedObjectsRefusedHr) return made;  // the caller says so (not a WIL failure)
  RETURN_IF_FAILED(made);
  *first = !existed;
  return S_OK;
}
CATCH_RETURN();

HRESULT AppMutexes::Hold(std::wstring_view userSid) noexcept try {
  // Opened with SYNCHRONIZE when another account created them: the handle keeps the mutex in
  // existence for as long as this process runs, which is all an installer checks.
  bool existed = false;
  const HRESULT session = CreateNamedMutex(AppMutexName(false), userSid, true, SYNCHRONIZE,
                                           std::addressof(session_), &existed);
  const HRESULT global = CreateNamedMutex(AppMutexName(true), userSid, true, SYNCHRONIZE,
                                          std::addressof(global_), &existed);
  return FAILED(session) ? session : global;
}
CATCH_RETURN();

bool MadeByThisUser(std::wstring_view ownerSid, std::wstring_view userSid) noexcept {
  return !ownerSid.empty() &&
         (ownerSid == userSid || ownerSid == kAdministratorsSid || ownerSid == kSystemSid);
}

OperationLock::OperationLock(wil::unique_handle session, wil::unique_handle global, bool abandoned,
                             bool sessionOnly) noexcept
    : session_(std::move(session)),
      global_(std::move(global)),
      abandoned_(abandoned),
      sessionOnly_(sessionOnly) {}

OperationLock::~OperationLock() {
  // The other way round from Acquire; in session 0 the two are one mutex, owned twice.
  if (global_) ReleaseMutex(global_.get());
  if (session_) ReleaseMutex(session_.get());
}

HRESULT OperationLock::Acquire(DWORD waitMs, std::optional<OperationLock>* lock) noexcept try {
  lock->reset();
  std::wstring sid;
  RETURN_IF_FAILED(GetCurrentUserSid(&sid));
  const ULONGLONG start = GetTickCount64();
  bool abandoned = false;

  // This session's name first, in every process, so two of them never wait for each other the
  // wrong way round. No other account's sign-in can make it, so the user's processes in this
  // session keep each other out whatever happens to the global name.
  wil::unique_handle session;
  LockMutex found = LockMutex::kRefused;
  RETURN_IF_FAILED(
      OpenLockMutex(OperationSessionLockName(sid), sid, std::addressof(session), &found));
  switch (found) {
    case LockMutex::kUsable:
      if (const HRESULT waited = WaitForLock(session.get(), waitMs, &abandoned); waited != S_OK) {
        if (waited == kBusy) return kBusy;
        RETURN_HR(waited);
      }
      break;
    case LockMutex::kRefused:
      // This process may not make names in its session (one started as another account in
      // someone else's session, say): the global name alone, if it can be had.
      break;
    case LockMutex::kLowerIntegrity:
      return kTakenByLowerIntegrity;
    case LockMutex::kAnotherAccount:
    case LockMutex::kBusy:
      // It exists, but this process may not wait on it: whoever made it keeps everyone else out.
      // In session 0 a Local\ name is the Global\ one (that session's namespace is the
      // machine's), so another account may have made this one too.
      return kBusy;
  }
  // Let go of this session's mutex again when the global name ends the attempt.
  auto releaseSession = wil::scope_exit([&session] {
    if (session) ReleaseMutex(session.get());
  });

  // Then the global name, which keeps out the user's processes in their other sessions too, but
  // only when this user made it. Any account may make that name, or put an object manager link
  // there that leads wherever it likes, so whatever fails there (WIL has logged why) is not this
  // user's lock either.
  wil::unique_handle global;
  LockMutex globalFound = LockMutex::kAnotherAccount;
  if (FAILED(OpenLockMutex(OperationLockName(sid), sid, std::addressof(global), &globalFound))) {
    global.reset();
    globalFound = LockMutex::kAnotherAccount;
  }
  switch (globalFound) {
    case LockMutex::kUsable:
      // In session 0 this is the session's mutex again: this thread owns it already, so the wait
      // returns at once, and each handle releases it once.
      if (const HRESULT waited =
              WaitForLock(global.get(), RemainingWait(waitMs, start), &abandoned);
          waited != S_OK) {
        if (waited == kBusy) return kBusy;
        RETURN_HR(waited);
      }
      break;
    case LockMutex::kLowerIntegrity:
      return kTakenByLowerIntegrity;
    case LockMutex::kBusy:
      return kBusy;
    case LockMutex::kAnotherAccount:
    case LockMutex::kRefused:
      global.reset();
      if (!session) {
        // Neither name can be had. When this process may make neither, there is no holder to
        // wait for, and waiting would not let it make the lock: the callers say so (not a WIL
        // failure). Otherwise the global name keeps it out.
        return globalFound == LockMutex::kRefused ? kNamedObjectsRefusedHr : kBusy;
      }
      break;
  }
  releaseSession.release();
  const bool sessionOnly = !global;
  lock->emplace(OperationLock(std::move(session), std::move(global), abandoned, sessionOnly));
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
