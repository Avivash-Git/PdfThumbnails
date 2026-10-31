#include "instance.h"

#include <sddl.h>
#include <wil/result_macros.h>

#include <memory>
#include <string>

#include "project_config.h"

namespace pdft {
namespace {

// Appended to every object name (SetObjectNameSuffixForTesting); empty outside tests. Created
// on first use, not before main.
std::wstring& NameSuffix() {
  static std::wstring suffix;
  return suffix;
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
  sddl += L"S:(ML;;NW;;;ME)";
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
  HANDLE handle = CreateMutexW(&attributes, FALSE, nameText.c_str());
  const DWORD error = GetLastError();
  if (handle) {
    mutex->reset(handle);
    *existed = error == ERROR_ALREADY_EXISTS;
    return S_OK;
  }
  // Access denied: the mutex exists, created by a process whose descriptor does not let this
  // one open it with full access. That is still "another instance has it".
  RETURN_HR_IF(HRESULT_FROM_WIN32(error), error != ERROR_ACCESS_DENIED);
  *existed = true;
  if (fallbackAccess) mutex->reset(OpenMutexW(fallbackAccess, FALSE, nameText.c_str()));
  return S_OK;
}
CATCH_RETURN();

HRESULT TakeSingleInstance(std::wstring_view userSid, wil::unique_handle* mutex,
                           bool* first) noexcept try {
  *first = false;
  bool existed = false;
  RETURN_IF_FAILED(CreateNamedMutex(SettingsInstanceMutexName(userSid), userSid, false, SYNCHRONIZE,
                                    mutex, &existed));
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
  RETURN_IF_FAILED(
      CreateNamedMutex(OperationLockName(sid), sid, false, 0, std::addressof(mutex), &existed));
  // It exists, but this process may not wait on it: whoever made it keeps everyone else out.
  if (!mutex) return kBusy;
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
