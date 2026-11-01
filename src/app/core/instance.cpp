#include "instance.h"

#include <aclapi.h>
#include <objbase.h>
#include <sddl.h>
#include <wil/result_macros.h>

#include <memory>
#include <string>
#include <utility>

#include "project_config.h"

namespace pdft {
namespace {

// How often CreateNamedMutex creates a name again that was gone after a refused creation.
constexpr int kCreateAttempts = 3;

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

std::wstring LogMutexName(std::wstring_view userSid) {
  return std::wstring(L"Local\\") + kObjectPrefix + L".Log." + std::wstring(userSid) + NameSuffix();
}

std::wstring AppMutexName(bool global) {
  return std::wstring(global ? L"Global\\" : L"") + kObjectPrefix + L"AppMutex" + NameSuffix();
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

OperationLock::OperationLock(wil::unique_handle mutex, bool abandoned) noexcept
    : mutex_(std::move(mutex)), abandoned_(abandoned) {}

OperationLock::~OperationLock() {
  if (mutex_) ReleaseMutex(mutex_.get());
}

HRESULT OperationLock::Acquire(DWORD waitMs, std::optional<OperationLock>* lock) noexcept try {
  lock->reset();
  std::wstring sid;
  RETURN_IF_FAILED(GetCurrentUserSid(&sid));
  wil::unique_handle mutex;
  bool existed = false;
  const HRESULT made =
      CreateNamedMutex(OperationLockName(sid), sid, false, 0, std::addressof(mutex), &existed);
  // Not busy: there is no holder to wait for, and waiting would not let this process make the
  // lock. The callers say so (not a WIL failure).
  if (made == kNamedObjectsRefusedHr) return made;
  RETURN_IF_FAILED(made);
  // It exists, but this process may not wait on it: whoever made it keeps everyone else out.
  if (!mutex) return kBusy;
  if (existed) {
    bool belowMedium = false;
    RETURN_IF_FAILED(LabelIsBelowMedium(mutex.get(), &belowMedium));
    if (belowMedium) return kTakenByLowerIntegrity;
  }
  bool abandoned = false;
  switch (WaitForSingleObject(mutex.get(), waitMs)) {
    case WAIT_OBJECT_0:
      break;
    case WAIT_ABANDONED:
      // The holder ended without releasing it; this thread owns it now.
      abandoned = true;
      break;
    case WAIT_TIMEOUT:
      return kBusy;
    default:
      RETURN_LAST_ERROR();
  }
  lock->emplace(OperationLock(std::move(mutex), abandoned));
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
