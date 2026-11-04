#include "test_account.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <lm.h>
#include <sddl.h>
#include <shlobj.h>
#include <winternl.h>
#include <wtsapi32.h>
// Again, after the headers above: WIL adds its wrappers for what they declare.
#include <wil/resource.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "account_rules.h"
#include "context.h"
#include "medium_il.h"
#include "project_config.h"
#include "strings.h"

namespace pdft::systest {
namespace {

using std::filesystem::path;

TestAccount* g_current = nullptr;
TestAccount* g_standard = nullptr;

// The accounts this process made and has not deleted yet: never removed as an earlier run's.
std::vector<std::wstring>& OwnAccounts() {
  static std::vector<std::wstring> names;
  return names;
}

// What a program run as the account may do with this process's window station and desktop:
// enough to load user32 and show a (hidden) console.
constexpr ACCESS_MASK kStationRights = WINSTA_ALL_ACCESS | READ_CONTROL;
constexpr ACCESS_MASK kDesktopRights =
    DESKTOP_READOBJECTS | DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_HOOKCONTROL |
    DESKTOP_JOURNALRECORD | DESKTOP_JOURNALPLAYBACK | DESKTOP_ENUMERATE | DESKTOP_WRITEOBJECTS |
    DESKTOP_SWITCHDESKTOP | READ_CONTROL;

std::string Text(std::wstring_view text) { return WideToUtf8Lossy(text); }

HRESULT LastError() {
  const DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

std::string Failed(const char* what, HRESULT hr) {
  return std::string(what) + " failed: " + Text(HResultText(hr));
}

HRESULT NetResult(NET_API_STATUS status) {
  return status == NERR_Success ? S_OK : HRESULT_FROM_WIN32(status);
}

// |bytes| from the system's cryptographic generator.
HRESULT RandomBytes(std::span<BYTE> bytes) {
  const NTSTATUS status = BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return BCRYPT_SUCCESS(status) ? S_OK : HRESULT_FROM_NT(status);
}

// Overwrites |secret|'s characters before it is emptied, so it does not stay in freed memory.
void Wipe(std::wstring* secret) noexcept {
  SecureZeroMemory(secret->data(), secret->size() * sizeof(wchar_t));
  secret->clear();
}

// Enables the privileges |names| in this process's token, those it holds (best effort), for as
// long as it lives, then puts back how they were. Every program this process starts gets a copy
// of its token: an installer's command run with backup and restore enabled would get past
// permission checks that a real installer's, which has them disabled, does not.
class EnabledPrivileges {
 public:
  explicit EnabledPrivileges(std::initializer_list<const wchar_t*> names) noexcept {
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                          token_.put())) {
      return;
    }
    alignas(TOKEN_PRIVILEGES) std::array<BYTE, kBufferSize> wanted = {};
    auto* enable = reinterpret_cast<TOKEN_PRIVILEGES*>(wanted.data());
    for (const wchar_t* name : names) {
      if (enable->PrivilegeCount == kMaxPrivileges) break;
      LUID_AND_ATTRIBUTES& entry = enable->Privileges[enable->PrivilegeCount];
      if (LookupPrivilegeValueW(nullptr, name, &entry.Luid)) {
        entry.Attributes = SE_PRIVILEGE_ENABLED;
        ++enable->PrivilegeCount;
      }
    }
    DWORD size = 0;
    // |previous_| receives the state of those it changed: what the destructor puts back.
    if (enable->PrivilegeCount == 0 ||
        !AdjustTokenPrivileges(token_.get(), FALSE, enable, static_cast<DWORD>(previous_.size()),
                               reinterpret_cast<TOKEN_PRIVILEGES*>(previous_.data()), &size)) {
      token_.reset();
    }
  }
  ~EnabledPrivileges() {
    auto* previous = reinterpret_cast<TOKEN_PRIVILEGES*>(previous_.data());
    if (token_ && previous->PrivilegeCount > 0) {
      AdjustTokenPrivileges(token_.get(), FALSE, previous, 0, nullptr, nullptr);
    }
  }
  EnabledPrivileges(const EnabledPrivileges&) = delete;
  EnabledPrivileges& operator=(const EnabledPrivileges&) = delete;

 private:
  static constexpr DWORD kMaxPrivileges = 4;
  static constexpr size_t kBufferSize =
      sizeof(TOKEN_PRIVILEGES) + (kMaxPrivileges - 1) * sizeof(LUID_AND_ATTRIBUTES);
  wil::unique_handle token_;
  alignas(TOKEN_PRIVILEGES) std::array<BYTE, kBufferSize> previous_ = {};
};

// Whether |process| runs as |sid|, by its token's user. False when that cannot be read.
bool RunsAs(HANDLE process, PSID sid) noexcept {
  wil::unique_handle token;
  if (!OpenProcessToken(process, TOKEN_QUERY, token.put())) return false;
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  return GetTokenInformation(token.get(), TokenUser, user, sizeof(user), &size) &&
         EqualSid(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, sid);
}

// Ends every process that runs as |sid| (but this one), printing each.
void EndProcessesOf(PSID sid) noexcept {
  // Their objects grant only the account and SYSTEM; an administrator ends them with the debug
  // privilege. Which they are, the session manager says without opening them.
  const EnabledPrivileges debug({SE_DEBUG_NAME});
  WTS_PROCESS_INFOW* processes = nullptr;
  DWORD count = 0;
  if (!WTSEnumerateProcessesW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &processes, &count)) return;
  const auto freeProcesses = wil::scope_exit([processes] { WTSFreeMemory(processes); });
  for (DWORD i = 0; i < count; ++i) {
    const WTS_PROCESS_INFOW& entry = processes[i];
    if (!entry.pUserSid || !EqualSid(entry.pUserSid, sid) ||
        entry.ProcessId == GetCurrentProcessId()) {
      continue;
    }
    // The list is a snapshot: by now the process may have ended and its number gone to another
    // one. So the process this handle holds is ended only if it runs as the account itself.
    const wil::unique_handle process(
        OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE,
                    entry.ProcessId));
    const char* ended = "no";
    if (process && !RunsAs(process.get(), sid)) {
      ended = "no (not confirmed to run as the account)";
    } else if (process && TerminateProcess(process.get(), 1) &&
               WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0) {
      ended = "yes";
    }
    std::printf("[ system ] %ls (process %lu) still ran as a test account: ended %s\n",
                entry.pProcessName ? entry.pProcessName : L"?", entry.ProcessId, ended);
  }
}

// Deletes the profile of the account |sid| (its folder and its ProfileList entry), trying again
// for a while: Windows may hold its files a moment after it was unloaded. "yes", "none" when
// it had no profile, else "no" (and a line about |who| that says why).
const char* DeleteProfileOf(const std::wstring& sid, const char* who) noexcept {
  DWORD lastError = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (attempt > 0) Sleep(1000);
    if (DeleteProfileW(sid.c_str(), nullptr, nullptr)) return "yes";
    lastError = GetLastError();
    if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) return "none";
  }
  std::printf("[ system ] %s: DeleteProfileW failed: %lu\n", who, lastError);
  return "no";
}

bool IsOwnAccount(std::wstring_view name) {
  for (const std::wstring& own : OwnAccounts()) {
    if (IEquals(own, name)) return true;
  }
  return false;
}

// Removes the test accounts that earlier runs made and could not delete (they were ended from
// outside: a timeout, a cancelled job), with their profiles: IsTestAccount's name and comment,
// nothing else, and none that this process made and still has.
void RemoveEarlierTestAccounts() {
  std::vector<std::wstring> names;
  DWORD resume = 0;
  NET_API_STATUS status = NERR_Success;
  do {
    LPBYTE buffer = nullptr;
    DWORD read = 0;
    DWORD total = 0;
    status = NetUserEnum(nullptr, 1, FILTER_NORMAL_ACCOUNT, &buffer, MAX_PREFERRED_LENGTH, &read,
                         &total, &resume);
    if (buffer) {
      const auto* users = reinterpret_cast<const USER_INFO_1*>(buffer);
      for (DWORD i = 0; i < read; ++i) {
        if (users[i].usri1_name &&
            IsTestAccount(users[i].usri1_name,
                          users[i].usri1_comment ? users[i].usri1_comment : L"") &&
            !IsOwnAccount(users[i].usri1_name)) {
          names.emplace_back(users[i].usri1_name);
        }
      }
      NetApiBufferFree(buffer);
    }
  } while (status == ERROR_MORE_DATA);
  if (status != NERR_Success) {
    std::printf("[ system ] listing earlier test accounts failed: 0x%08lX\n",
                static_cast<unsigned long>(NetResult(status)));
  }
  for (const std::wstring& name : names) {
    std::wstring sid;
    LPBYTE buffer = nullptr;
    if (NetUserGetInfo(nullptr, name.c_str(), 23, &buffer) == NERR_Success && buffer) {
      const PSID accountSid = reinterpret_cast<const USER_INFO_23*>(buffer)->usri23_user_sid;
      wil::unique_hlocal_string text;
      if (accountSid && IsValidSid(accountSid)) {
        EndProcessesOf(accountSid);
        if (ConvertSidToStringSidW(accountSid, text.put())) sid = text.get();
      }
    }
    if (buffer) NetApiBufferFree(buffer);
    const NET_API_STATUS deleted = NetUserDel(nullptr, name.c_str());
    const char* profile =
        sid.empty() ? "unknown (no SID)" : DeleteProfileOf(sid, "an earlier run's test account");
    std::printf(
        "[ system ] an earlier run's test account %ls (%ls) removed: account deleted %s, "
        "profile deleted %s\n",
        name.c_str(), sid.empty() ? L"no SID" : sid.c_str(),
        deleted == NERR_Success ? "yes" : Text(HResultText(NetResult(deleted))).c_str(), profile);
  }
  std::fflush(stdout);
}

// The local Administrators group's name in this Windows' language, from its well-known SID.
HRESULT AdministratorsGroupName(std::wstring* name) {
  BYTE sid[SECURITY_MAX_SID_SIZE];
  DWORD sidSize = sizeof(sid);
  if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &sidSize)) return LastError();
  wchar_t group[256] = {};
  DWORD groupSize = ARRAYSIZE(group);
  wchar_t domain[256] = {};
  DWORD domainSize = ARRAYSIZE(domain);
  SID_NAME_USE use = SidTypeUnknown;
  if (!LookupAccountSidW(nullptr, sid, group, &groupSize, domain, &domainSize, &use)) {
    return LastError();
  }
  *name = group;
  return S_OK;
}

// The known folder |id| of the user of |token|, whose profile is loaded.
HRESULT KnownFolderOf(REFKNOWNFOLDERID id, HANDLE token, path* out) {
  wil::unique_cotaskmem_string folder;
  const HRESULT hr = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, token, folder.put());
  if (FAILED(hr)) return hr;
  *out = folder.get();
  return S_OK;
}

// The name of the window station or desktop |object|.
std::wstring UserObjectName(HANDLE object) {
  wchar_t name[256] = {};
  DWORD needed = 0;
  if (!GetUserObjectInformationW(object, UOI_NAME, name, sizeof(name) - sizeof(wchar_t), &needed)) {
    return {};
  }
  return name;
}

// An object's DACL as read for changing it, with the control bits that go with it.
struct ObjectDacl {
  wil::unique_hlocal_security_descriptor descriptor;
  PACL acl = nullptr;  // in |descriptor|; nullptr: no DACL (everyone may use the object)
  SECURITY_DESCRIPTOR_CONTROL control = 0;
};

// Reads the DACL of |object|, of the kind |type| (a window station or desktop: SE_WINDOW_OBJECT;
// an object directory: SE_KERNEL_OBJECT), opened with READ_CONTROL.
HRESULT ReadDacl(HANDLE object, SE_OBJECT_TYPE type, ObjectDacl* dacl) {
  const DWORD error = GetSecurityInfo(object, type, DACL_SECURITY_INFORMATION, nullptr, nullptr,
                                      &dacl->acl, nullptr, dacl->descriptor.put());
  if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
  DWORD revision = 0;
  if (!GetSecurityDescriptorControl(dacl->descriptor.get(), &dacl->control, &revision)) {
    return LastError();
  }
  return S_OK;
}

// Makes |acl| the DACL of |object| (of the kind |type|, opened with WRITE_DAC), with the DACL's
// control bits (protected, auto-inherited) as |control| has them; nothing else of its
// permissions changes.
HRESULT WriteDacl(HANDLE object, SE_OBJECT_TYPE type, PACL acl,
                  SECURITY_DESCRIPTOR_CONTROL control) {
  SECURITY_DESCRIPTOR descriptor = {};
  if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)) {
    return LastError();
  }
  constexpr SECURITY_DESCRIPTOR_CONTROL kKept = SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED;
  if (!SetSecurityDescriptorControl(&descriptor, kKept, control & kKept)) return LastError();
  SECURITY_INFORMATION which = DACL_SECURITY_INFORMATION;
  const BOOL written = type == SE_WINDOW_OBJECT
                           ? SetUserObjectSecurity(object, &which, &descriptor)
                           : SetKernelObjectSecurity(object, which, &descriptor);
  return written ? S_OK : LastError();
}

// Lets |sid| (an account or a logon session) have |rights| on |object|, of the kind |type|,
// opened with READ_CONTROL and WRITE_DAC: adds one allowing entry without inheritance after the
// object's own entries (before any inherited ones) and changes nothing else, neither the order
// of the other entries nor the DACL's control bits. S_FALSE: the object has no DACL, so everyone
// may use it already, and nothing was added. When it fails, nothing was changed. RevokeOnObject
// takes the entry away again.
HRESULT GrantOnObject(HANDLE object, SE_OBJECT_TYPE type, PSID sid, ACCESS_MASK rights) {
  ObjectDacl dacl;
  if (const HRESULT hr = ReadDacl(object, type, &dacl); FAILED(hr)) return hr;
  if (!dacl.acl) return S_FALSE;
  ACL_SIZE_INFORMATION size = {};
  if (!GetAclInformation(dacl.acl, &size, sizeof(size), AclSizeInformation)) return LastError();
  const DWORD bytes = size.AclBytesInUse + sizeof(ACCESS_ALLOWED_ACE) + GetLengthSid(sid);
  std::vector<DWORD> buffer((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
  auto* updated = reinterpret_cast<PACL>(buffer.data());
  const DWORD revision = dacl.acl->AclRevision;
  if (!InitializeAcl(updated, static_cast<DWORD>(buffer.size() * sizeof(DWORD)), revision)) {
    return LastError();
  }
  bool added = false;
  for (DWORD i = 0; i < dacl.acl->AceCount; ++i) {
    ACE_HEADER* ace = nullptr;
    if (!GetAce(dacl.acl, i, reinterpret_cast<void**>(&ace))) return LastError();
    if (!added && (ace->AceFlags & INHERITED_ACE)) {
      if (!AddAccessAllowedAceEx(updated, revision, 0, rights, sid)) return LastError();
      added = true;
    }
    if (!AddAce(updated, revision, MAXDWORD, ace, ace->AceSize)) return LastError();
  }
  if (!added && !AddAccessAllowedAceEx(updated, revision, 0, rights, sid)) return LastError();
  return WriteDacl(object, type, updated, dacl.control);
}

// Takes away the entry GrantOnObject added to |object| for |sid| with |rights| (the first
// allowing entry without flags that says exactly that), and nothing else: entries added or
// changed since stay as they are, and so do the DACL's control bits. S_FALSE: no such entry
// was there.
HRESULT RevokeOnObject(HANDLE object, SE_OBJECT_TYPE type, PSID sid, ACCESS_MASK rights) noexcept {
  try {
    ObjectDacl dacl;
    if (const HRESULT hr = ReadDacl(object, type, &dacl); FAILED(hr)) return hr;
    for (DWORD i = 0; dacl.acl && i < dacl.acl->AceCount; ++i) {
      ACE_HEADER* ace = nullptr;
      if (!GetAce(dacl.acl, i, reinterpret_cast<void**>(&ace))) return LastError();
      const auto* allowed = reinterpret_cast<const ACCESS_ALLOWED_ACE*>(ace);
      if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE || ace->AceFlags != 0 ||
          allowed->Mask != rights || !EqualSid(const_cast<DWORD*>(&allowed->SidStart), sid)) {
        continue;
      }
      // A copy without that entry.
      std::vector<DWORD> buffer((dacl.acl->AclSize + sizeof(DWORD) - 1) / sizeof(DWORD));
      std::memcpy(buffer.data(), dacl.acl, dacl.acl->AclSize);
      auto* updated = reinterpret_cast<PACL>(buffer.data());
      if (!DeleteAce(updated, i)) return LastError();
      return WriteDacl(object, type, updated, dacl.control);
    }
    return S_FALSE;
  } catch (...) {
    return E_OUTOFMEMORY;
  }
}

// ---- This session's named objects ----

// Object directory rights (ntddk.h's DIRECTORY_*, which the SDK's user-mode headers lack).
constexpr ACCESS_MASK kDirectoryQuery = 0x0001;
constexpr ACCESS_MASK kDirectoryTraverse = 0x0002;
constexpr ACCESS_MASK kDirectoryCreateObject = 0x0004;
constexpr ACCESS_MASK kDirectoryCreateSubdirectory = 0x0008;
// What a program asks for when it first uses a named object: kernel32 opens the session's named
// objects directory with every right but DELETE, WRITE_DAC and WRITE_OWNER (0x0002000F), and when
// that is refused, with traverse alone. Through a handle with traverse alone the program can
// still make objects where the directory's permissions let it, since Windows checks creating
// against those permissions, not against the handle's rights. When even traverse is refused,
// every named object call of the program fails with access denied.
constexpr ACCESS_MASK kNamedObjectRights = READ_CONTROL | kDirectoryQuery | kDirectoryTraverse |
                                           kDirectoryCreateObject | kDirectoryCreateSubdirectory;

using NtOpenDirectoryObjectFn = NTSTATUS(NTAPI*)(HANDLE*, ACCESS_MASK, OBJECT_ATTRIBUTES*);
using RtlNtStatusToDosErrorFn = ULONG(NTAPI*)(NTSTATUS);

// The object directory with the named objects of the programs in session |session|: "Local\"
// names are made there, and "Global\" is a link in it to \BaseNamedObjects, session 0's own.
std::wstring NamedObjectsDirectory(DWORD session) {
  if (session == 0) return L"\\BaseNamedObjects";
  return L"\\Sessions\\" + std::to_wstring(session) + L"\\BaseNamedObjects";
}

// Opens the object directory |path| with |access| as whoever the calling thread runs as. No
// documented call opens one, so this takes ntdll's own (every process has ntdll loaded).
HRESULT OpenObjectDirectory(const std::wstring& path, ACCESS_MASK access,
                            wil::unique_handle* directory) {
  directory->reset();
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (!ntdll) return LastError();
  const auto open = reinterpret_cast<NtOpenDirectoryObjectFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "NtOpenDirectoryObject")));
  const auto win32Error = reinterpret_cast<RtlNtStatusToDosErrorFn>(
      reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
  if (!open || !win32Error) return LastError();
  UNICODE_STRING name = {};
  name.Buffer = const_cast<wchar_t*>(path.c_str());
  name.Length = static_cast<USHORT>(path.size() * sizeof(wchar_t));
  name.MaximumLength = name.Length;
  OBJECT_ATTRIBUTES attributes = {};
  attributes.Length = sizeof(attributes);
  attributes.ObjectName = &name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  HANDLE handle = nullptr;
  const NTSTATUS status = open(&handle, access, &attributes);
  if (!NT_SUCCESS(status)) return HRESULT_FROM_WIN32(win32Error(status));
  directory->reset(handle);
  return S_OK;
}

// What a program with |token| gets when it opens the object directory |path| with |access|: this
// thread tries it as |token| (impersonating it), with the same answer.
HRESULT OpenObjectDirectoryAs(HANDLE token, const std::wstring& path, ACCESS_MASK access) {
  if (!ImpersonateLoggedOnUser(token)) return LastError();
  wil::unique_handle directory;
  const HRESULT hr = OpenObjectDirectory(path, access, std::addressof(directory));
  directory.reset();
  // Never left impersonating: everything after this must run as this process again.
  if (!RevertToSelf()) {
    std::printf("[ system ] RevertToSelf failed: %lu\n", GetLastError());
    std::fflush(stdout);
    std::abort();
  }
  return hr;
}

// "yes" or "no" with |hr|, for the log.
std::string YesNo(HRESULT hr) {
  return std::string(SUCCEEDED(hr) ? "yes" : "no") + " (" + Text(HResultText(hr)) + ")";
}

// Whether a program with |token| can make named objects in the object directory |path|, which
// kernel32 opens for it as kNamedObjectRights says: S_OK when the open with every right succeeds,
// or when only the one with traverse alone does and the directory's permissions let |token|
// create objects there; otherwise the first open's error. |how| says what each step gave.
HRESULT NamedObjectsAs(HANDLE token, const std::wstring& path, std::string* how) {
  const HRESULT full = OpenObjectDirectoryAs(token, path, kNamedObjectRights);
  char rights[16] = {};
  sprintf_s(rights, "0x%08lX", static_cast<unsigned long>(kNamedObjectRights));
  *how = std::string("with ") + rights + ": " + YesNo(full);
  if (SUCCEEDED(full)) return S_OK;
  const HRESULT traverse = OpenObjectDirectoryAs(token, path, kDirectoryTraverse);
  *how += ", with traverse alone: " + YesNo(traverse);
  if (FAILED(traverse)) return full;
  const HRESULT create = OpenObjectDirectoryAs(token, path, kDirectoryCreateObject);
  *how += ", may create objects there: " + YesNo(create);
  return SUCCEEDED(create) ? S_OK : full;
}

// Whether |sid| is one of NT AUTHORITY's whose first sub-authority is |rid|
// (SECURITY_LOGON_IDS_RID: a logon session; SECURITY_NT_NON_UNIQUE: an account of a PC or
// domain).
bool IsNtSidOf(PSID sid, DWORD rid) {
  constexpr SID_IDENTIFIER_AUTHORITY kNt = SECURITY_NT_AUTHORITY;
  const SID_IDENTIFIER_AUTHORITY* authority = GetSidIdentifierAuthority(sid);
  return authority && memcmp(authority->Value, kNt.Value, sizeof(kNt.Value)) == 0 &&
         *GetSidSubAuthorityCount(sid) > 0 && *GetSidSubAuthority(sid, 0) == rid;
}

// Whether |sid| is of a kind that is the same on every PC and names nobody: the world, local and
// creator SIDs, NT AUTHORITY's but its accounts and logon sessions (SYSTEM, Administrators,
// services, ...), app packages and their capabilities, integrity labels. Not an account of a PC,
// a domain or the cloud, nor a logon session.
bool IsWellKnownKind(PSID sid) {
  const SID_IDENTIFIER_AUTHORITY* authority = GetSidIdentifierAuthority(sid);
  // Big-endian: every authority here fits in the last byte.
  if (!authority || authority->Value[0] || authority->Value[1] || authority->Value[2] ||
      authority->Value[3] || authority->Value[4]) {
    return false;
  }
  switch (authority->Value[5]) {
    case 1:   // SECURITY_WORLD_SID_AUTHORITY
    case 2:   // SECURITY_LOCAL_SID_AUTHORITY
    case 3:   // SECURITY_CREATOR_SID_AUTHORITY
    case 15:  // SECURITY_APP_PACKAGE_AUTHORITY
    case 16:  // SECURITY_MANDATORY_LABEL_AUTHORITY
      return true;
    case 5:  // SECURITY_NT_AUTHORITY
      return !IsNtSidOf(sid, SECURITY_NT_NON_UNIQUE) && !IsNtSidOf(sid, SECURITY_LOGON_IDS_RID);
    default:
      return false;
  }
}

// The logon SID in |token| (TokenLogonSid); empty when it has none.
std::vector<BYTE> LogonSidOf(HANDLE token) {
  alignas(TOKEN_GROUPS) BYTE buffer[sizeof(TOKEN_GROUPS) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  if (!GetTokenInformation(token, TokenLogonSid, buffer, sizeof(buffer), &size)) return {};
  const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(buffer);
  if (groups->GroupCount == 0 || !IsValidSid(groups->Groups[0].Sid)) return {};
  const auto* start = static_cast<const BYTE*>(groups->Groups[0].Sid);
  return std::vector<BYTE>(start, start + GetLengthSid(groups->Groups[0].Sid));
}

// The SIDs that the lines about permissions name by what they are.
struct KnownSids {
  const char* noun = "test account";  // what the lines call the account (TestAccount::noun)
  PSID account = nullptr;             // the account
  PSID accountLogon = nullptr;        // its logon session (the one of its interactive logon)
  PSID ownLogon = nullptr;            // this process's logon session
};

// |sid| for the log: the account's SIDs and this process's logon session by what they are, a
// well-known SID (IsWellKnownKind) as it is, and any other only by its kind. No SID of the
// runner's own accounts, nor of anyone else, is printed.
std::string SidForLog(PSID sid, const KnownSids& known) {
  if (known.account && EqualSid(sid, known.account)) return std::string("the ") + known.noun;
  if (known.accountLogon && EqualSid(sid, known.accountLogon)) {
    return std::string("the ") + known.noun + "'s logon";
  }
  if (known.ownLogon && EqualSid(sid, known.ownLogon)) return "this process's logon";
  if (IsNtSidOf(sid, SECURITY_LOGON_IDS_RID)) return "another logon";
  if (IsNtSidOf(sid, SECURITY_NT_NON_UNIQUE)) return "an account of this PC or its domain";
  if (!IsWellKnownKind(sid)) return "another account or group";
  wil::unique_hlocal_string text;
  return ConvertSidToStringSidW(sid, text.put()) ? Text(text.get()) : std::string("?");
}

// |dacl| for the log, its entries one after another as (<A: allowed, D: denied, else the entry's
// type>;<flags>;<rights>;<SidForLog>).
std::string DaclForLog(PACL dacl, const KnownSids& known) {
  if (!dacl) return "no DACL (everyone may use it)";
  std::string text;
  for (DWORD i = 0; i < dacl->AceCount; ++i) {
    ACE_HEADER* ace = nullptr;
    if (!GetAce(dacl, i, reinterpret_cast<void**>(&ace))) continue;
    char entry[48] = {};
    if (ace->AceType == ACCESS_ALLOWED_ACE_TYPE || ace->AceType == ACCESS_DENIED_ACE_TYPE) {
      // A denying entry has the same layout.
      auto* simple = reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace);
      sprintf_s(entry, "(%s;0x%02X;0x%08lX;", ace->AceType == ACCESS_ALLOWED_ACE_TYPE ? "A" : "D",
                static_cast<unsigned>(ace->AceFlags), static_cast<unsigned long>(simple->Mask));
      text += entry + SidForLog(&simple->SidStart, known) + ")";
    } else {
      sprintf_s(entry, "(type %u)", static_cast<unsigned>(ace->AceType));
      text += entry;
    }
  }
  return text.empty() ? std::string("an empty DACL (nobody may use it)") : text;
}

// Whether |dacl| lets in a logon session by its logon SID, as a session's named objects let in
// the session's own sign-in.
bool GrantsALogon(PACL dacl) {
  for (DWORD i = 0; dacl && i < dacl->AceCount; ++i) {
    ACE_HEADER* ace = nullptr;
    if (GetAce(dacl, i, reinterpret_cast<void**>(&ace)) &&
        ace->AceType == ACCESS_ALLOWED_ACE_TYPE &&
        IsNtSidOf(&reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, SECURITY_LOGON_IDS_RID)) {
      return true;
    }
  }
  return false;
}

// Reads |pipe| until its other end is closed.
void ReadAll(HANDLE pipe, std::string* text) {
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    text->append(buffer, read);
  }
}

}  // namespace

TestAccount* CurrentTestAccount() noexcept { return g_current; }

void SetCurrentTestAccount(TestAccount* account) noexcept { g_current = account; }

TestAccount* CurrentStandardAccount() noexcept { return g_standard; }

void SetCurrentStandardAccount(TestAccount* account) noexcept { g_standard = account; }

std::optional<std::string> WhyNoTestAccountHere() {
  AccountGuard guard;
  guard.systemTests = EnvironmentValue(kEnableEnv);
  guard.githubActions = EnvironmentValue(kGitHubActionsEnv);
  guard.runnerEnvironment = EnvironmentValue(kRunnerEnvironmentEnv);
  guard.throwawayVm = EnvironmentValue(kThrowawayVmEnv);
  bool elevated = false;
  guard.elevated = SUCCEEDED(GetTokenElevation(&elevated)) && elevated;
  ProcessContext context;
  guard.realRegistry = SUCCEEDED(GetProcessContext(ContextProbe::kWrite, &context)) &&
                       !context.packagedAncestor && context.virtualization == Virtualization::kNo;
  guard.productFound = !Leftovers().empty();
  return WhyNoTestAccount(guard);
}

std::unique_ptr<TestAccount> TestAccount::Create(Kind kind, std::string* error) {
  error->clear();
  // Asked again right before anything is made, whatever the caller checked.
  if (const std::optional<std::string> why = WhyNoTestAccountHere()) {
    *error = "not made here: " + *why;
    return nullptr;
  }
  RemoveEarlierTestAccounts();
  std::unique_ptr<TestAccount> account(new TestAccount());
  account->kind_ = kind;

  // A random name that no account has.
  NET_API_STATUS lookup = NERR_Success;
  for (int attempt = 0; attempt < 8 && account->name_.empty(); ++attempt) {
    std::array<BYTE, 3> random = {};
    if (const HRESULT hr = RandomBytes(random); FAILED(hr)) {
      *error = Failed("BCryptGenRandom", hr);
      return nullptr;
    }
    std::wstring name = TestAccountName(random);
    LPBYTE found = nullptr;
    lookup = NetUserGetInfo(nullptr, name.c_str(), 0, &found);
    if (found) NetApiBufferFree(found);
    if (lookup == NERR_UserNotFound) account->name_ = std::move(name);
  }
  if (account->name_.empty()) {
    *error = "no free account name found (NetUserGetInfo answered " +
             Text(HResultText(NetResult(lookup))) + ")";
    return nullptr;
  }

  // The password: made here, held only in this string, wiped once the account is logged on.
  std::wstring password;
  for (int attempt = 0; attempt < 16 && password.empty(); ++attempt) {
    std::array<BYTE, 256> random = {};
    const HRESULT hr = RandomBytes(random);
    if (SUCCEEDED(hr)) {
      if (std::optional<std::wstring> made = PasswordFromRandom(random, account->name_)) {
        password = std::move(*made);
      }
    }
    SecureZeroMemory(random.data(), random.size());
    if (FAILED(hr)) {
      *error = Failed("BCryptGenRandom", hr);
      return nullptr;
    }
  }
  if (password.empty()) {
    *error = "no password could be made";
    return nullptr;
  }

  USER_INFO_1 user = {};
  user.usri1_name = account->name_.data();
  user.usri1_password = password.data();
  user.usri1_priv = USER_PRIV_USER;
  user.usri1_comment = const_cast<wchar_t*>(kTestAccountComment);
  user.usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT | UF_DONT_EXPIRE_PASSWD;
  DWORD badParameter = 0;
  const NET_API_STATUS added =
      NetUserAdd(nullptr, 1, reinterpret_cast<LPBYTE>(&user), &badParameter);
  user.usri1_password = nullptr;
  if (added != NERR_Success) {
    Wipe(&password);
    *error = Failed("NetUserAdd", NetResult(added)) + " (parameter " +
             std::to_string(badParameter) + ")";
    return nullptr;
  }
  account->created_ = true;
  OwnAccounts().push_back(account->name_);
  const HRESULT made = account->LogOnAndLoad(&password, error);
  Wipe(&password);  // already wiped once it was used; this covers a failure before that
  if (FAILED(made)) return nullptr;  // the destructor removes what was made
  return account;
}

HRESULT TestAccount::LogOnAndLoad(std::wstring* password, std::string* error) {
  // Its SID, from the account database.
  LPBYTE buffer = nullptr;
  HRESULT hr = NetResult(NetUserGetInfo(nullptr, name_.c_str(), 23, &buffer));
  const auto freeBuffer = wil::scope_exit([&buffer] {
    if (buffer) NetApiBufferFree(buffer);
  });
  if (FAILED(hr)) {
    *error = Failed("NetUserGetInfo", hr);
    return hr;
  }
  const PSID sid = reinterpret_cast<const USER_INFO_23*>(buffer)->usri23_user_sid;
  if (!sid || !IsValidSid(sid)) {
    *error = "NetUserGetInfo gave no SID";
    return E_UNEXPECTED;
  }
  const BYTE* sidStart = static_cast<const BYTE*>(sid);
  sidBytes_.assign(sidStart, sidStart + GetLengthSid(sid));
  wil::unique_hlocal_string sidText;
  if (!ConvertSidToStringSidW(sid, sidText.put())) {
    hr = LastError();
    *error = Failed("ConvertSidToStringSidW", hr);
    return hr;
  }
  sid_ = sidText.get();

  // The test account: a member of Administrators, by the group's well-known SID (its name
  // depends on the language). The standard account stays a member of Users alone.
  if (kind_ == Kind::kAdministrator) {
    std::wstring group;
    if (hr = AdministratorsGroupName(&group); FAILED(hr)) {
      *error = Failed("looking up the Administrators group's name", hr);
      return hr;
    }
    LOCALGROUP_MEMBERS_INFO_0 member = {sidBytes_.data()};
    if (hr = NetResult(NetLocalGroupAddMembers(nullptr, group.c_str(), 0,
                                               reinterpret_cast<LPBYTE>(&member), 1));
        FAILED(hr)) {
      *error = Failed("NetLocalGroupAddMembers (Administrators)", hr);
      return hr;
    }
  }

  // Logged on as a person signs in. With UAC on, a member of Administrators gets its limited
  // token from that, with the full one linked to it; a standard user gets the only token it has.
  HANDLE token = nullptr;
  const BOOL loggedOn = LogonUserW(name_.c_str(), L".", password->c_str(),
                                   LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT, &token);
  hr = loggedOn ? S_OK : LastError();
  Wipe(password);  // used: never needed again
  if (FAILED(hr)) {
    *error = Failed("LogonUserW (interactive)", hr);
    return hr;
  }
  token_.reset(token);
  if (hr = ReadTokenFacts(token_.get(), &facts_); FAILED(hr)) {
    *error = Failed("reading its token", hr);
    return hr;
  }
  TOKEN_LINKED_TOKEN linked = {};
  DWORD size = 0;
  if (kind_ == Kind::kAdministrator &&
      GetTokenInformation(token_.get(), TokenLinkedToken, &linked, sizeof(linked), &size)) {
    const wil::unique_handle linkedToken(linked.LinkedToken);
    (void)ReadTokenFacts(linkedToken.get(), &linked_);
  }
  if (kind_ == Kind::kAdministrator && facts_.elevationType != TokenElevationTypeLimited) {
    *error = "its interactive logon gave this " + Text(TokenFactsText(facts_)) +
             ", not a limited token (elevation type 3) with a full one linked to it: UAC "
             "could not elevate its programs";
    return E_UNEXPECTED;
  }
  if (kind_ == Kind::kStandard &&
      (facts_.elevated || facts_.elevationType != TokenElevationTypeDefault ||
       facts_.administrators != TokenFacts::Group::kAbsent)) {
    *error = "its interactive logon gave this " + Text(TokenFactsText(facts_)) +
             ", not a standard user's token (not elevated, elevation type 1, without "
             "Administrators)";
    return E_UNEXPECTED;
  }

  // Its profile: its own HKEY_CURRENT_USER and %LOCALAPPDATA%. Loading a hive needs backup and
  // restore, enabled only for that.
  profile_.dwSize = sizeof(profile_);
  profile_.dwFlags = PI_NOUI;
  profile_.lpUserName = name_.data();
  {
    const EnabledPrivileges hives({SE_BACKUP_NAME, SE_RESTORE_NAME});
    if (!LoadUserProfileW(token_.get(), &profile_)) {
      hr = LastError();
      *error = Failed("LoadUserProfileW", hr);
      return hr;
    }
  }
  profileLoaded_ = true;
  if (!CreateEnvironmentBlock(environment_.put(), token_.get(), FALSE)) {
    hr = LastError();
    *error = Failed("CreateEnvironmentBlock", hr);
    return hr;
  }
  wchar_t profileDir[MAX_PATH] = {};
  DWORD length = ARRAYSIZE(profileDir);
  if (!GetUserProfileDirectoryW(token_.get(), profileDir, &length)) {
    hr = LastError();
    *error = Failed("GetUserProfileDirectoryW", hr);
    return hr;
  }
  profileDir_ = profileDir;
  path localAppData;
  path userPrograms;
  if (hr = KnownFolderOf(FOLDERID_LocalAppData, token_.get(), &localAppData); FAILED(hr)) {
    *error = Failed("finding its %LOCALAPPDATA%", hr);
    return hr;
  }
  if (hr = KnownFolderOf(FOLDERID_UserProgramFiles, token_.get(), &userPrograms); FAILED(hr)) {
    *error = Failed("finding its %LOCALAPPDATA%\\Programs", hr);
    return hr;
  }
  // As paths.h makes them for the user who runs the app.
  userRoot_ = userPrograms / kProductName;
  dataDir_ = localAppData / kProductName;
  tempDir_ = localAppData / L"Temp";
  std::error_code made;
  std::filesystem::create_directories(tempDir_, made);
  if (made) {
    *error = "making its temp folder failed: " + made.message();
    return E_FAIL;
  }
  // CreateProcessWithTokenW hands the account's programs every inheritable handle of this
  // process (Run): this process's own standard handles, which CTest's pipes may be, are not
  // theirs to have. (RunCaptured names the handles it passes on.)
  for (const DWORD which : {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE}) {
    const HANDLE handle = GetStdHandle(which);
    if (handle && handle != INVALID_HANDLE_VALUE) {
      SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    }
  }
  AllowDesktop();
  AllowNamedObjects();  // the tests stop when its programs cannot have them (main.cpp)
  return S_OK;
}

void TestAccount::AllowDesktop() {
  // A program loads user32, which opens the window station and desktop it starts on, as its own
  // account: CreateProcessWithTokenW leaves letting the account in to the caller.
  const std::wstring stationName = UserObjectName(GetProcessWindowStation());
  const std::wstring desktopName = UserObjectName(GetThreadDesktop(GetCurrentThreadId()));
  HRESULT station = E_FAIL;
  if (!stationName.empty()) {
    station_.reset(OpenWindowStationW(stationName.c_str(), FALSE, READ_CONTROL | WRITE_DAC));
    station =
        station_ ? GrantOnObject(station_.get(), SE_WINDOW_OBJECT, sidBytes_.data(), kStationRights)
                 : LastError();
    stationGranted_ = station == S_OK;
    if (!stationGranted_) station_.reset();
  }
  HRESULT desktop = E_FAIL;
  if (!desktopName.empty()) {
    desktop_.reset(OpenDesktopW(desktopName.c_str(), 0, FALSE, READ_CONTROL | WRITE_DAC));
    desktop =
        desktop_ ? GrantOnObject(desktop_.get(), SE_WINDOW_OBJECT, sidBytes_.data(), kDesktopRights)
                 : LastError();
    desktopGranted_ = desktop == S_OK;
    if (!desktopGranted_) desktop_.reset();
  }
  std::printf(
      "[ system ] the %s may use window station %s: %s; desktop %s: %s\n", noun(),
      Text(stationName).c_str(), SUCCEEDED(station) ? "yes" : Text(HResultText(station)).c_str(),
      Text(desktopName).c_str(), SUCCEEDED(desktop) ? "yes" : Text(HResultText(desktop)).c_str());
}

void TestAccount::RestoreDesktop() noexcept {
  if (desktopGranted_) {
    (void)RevokeOnObject(desktop_.get(), SE_WINDOW_OBJECT, sidBytes_.data(), kDesktopRights);
  }
  if (stationGranted_) {
    (void)RevokeOnObject(station_.get(), SE_WINDOW_OBJECT, sidBytes_.data(), kStationRights);
  }
  desktopGranted_ = false;
  desktop_.reset();
  stationGranted_ = false;
  station_.reset();
}

void TestAccount::AllowNamedObjects() {
  // The account's programs run in this process's session (CreateProcessWithTokenW): Windows looks
  // up every name they use through that session's directory.
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
    namedObjects_ = LastError();
    std::printf(
        "[ system ] the %s may use the session's named objects: no (%s: this process's session "
        "is unknown)\n",
        noun(), Text(HResultText(namedObjects_)).c_str());
    return;
  }
  const std::wstring path = NamedObjectsDirectory(session);
  std::vector<BYTE> accountLogon = LogonSidOf(token_.get());
  std::vector<BYTE> ownLogon;
  if (wil::unique_handle own; OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, own.put())) {
    ownLogon = LogonSidOf(own.get());
  }
  const KnownSids known = {noun(), sidBytes_.data(),
                           accountLogon.empty() ? nullptr : accountLogon.data(),
                           ownLogon.empty() ? nullptr : ownLogon.data()};

  // Its permissions as they are, printed once, and what the account's programs get there now.
  std::string permissions;
  bool byLogon = false;
  {
    wil::unique_handle directory;
    HRESULT hr = OpenObjectDirectory(path, READ_CONTROL, std::addressof(directory));
    PSID owner = nullptr;
    PACL dacl = nullptr;
    wil::unique_hlocal_security_descriptor descriptor;
    if (SUCCEEDED(hr)) {
      hr = HRESULT_FROM_WIN32(GetSecurityInfo(
          directory.get(), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
          &owner, nullptr, &dacl, nullptr, descriptor.put()));
    }
    if (SUCCEEDED(hr)) {
      permissions = "owner " + (owner ? SidForLog(owner, known) : std::string("none")) + ", " +
                    DaclForLog(dacl, known);
      // Granted the way the directory lets in the session's own sign-in: by the logon SID when it
      // names logons, else by the account's SID.
      byLogon = known.accountLogon && GrantsALogon(dacl);
    } else {
      permissions = "permissions unreadable (" + Text(HResultText(hr)) + ")";
    }
  }
  std::string how;
  const HRESULT before = NamedObjectsAs(token_.get(), path, &how);
  // "Global\" names are made in \BaseNamedObjects, through a link in the session's directory.
  // Nothing here changes that directory: it is the whole machine's.
  HRESULT global = S_OK;
  std::string globalText;
  if (session != 0) {
    global = OpenObjectDirectoryAs(token_.get(), L"\\BaseNamedObjects", kDirectoryCreateObject);
    globalText =
        "; it may make objects in \\BaseNamedObjects (\"Global\\\" names): " + YesNo(global);
  }
  std::printf(
      "[ system ] this session's named objects %s: %s; the %s may make objects there as its "
      "programs do: %s (%s)%s\n",
      Text(path).c_str(), permissions.c_str(), noun(), SUCCEEDED(before) ? "yes" : "no",
      how.c_str(), globalText.c_str());

  HRESULT hr = before;
  if (FAILED(before) && session == 0) {
    // There the directory is \BaseNamedObjects, the whole machine's: never changed.
    std::printf("[ system ] not granting anything on %s: this process runs in session 0\n",
                Text(path).c_str());
  } else if (FAILED(before)) {
    const PSID grantee = byLogon ? known.accountLogon : sidBytes_.data();
    std::printf("[ system ] granting the %s%s 0x%08lX on %s for this run\n", noun(),
                byLogon ? "'s logon" : "", static_cast<unsigned long>(kNamedObjectRights),
                Text(path).c_str());
    hr =
        OpenObjectDirectory(path, READ_CONTROL | WRITE_DAC, std::addressof(namedObjectsDirectory_));
    if (SUCCEEDED(hr)) {
      hr = GrantOnObject(namedObjectsDirectory_.get(), SE_KERNEL_OBJECT, grantee,
                         kNamedObjectRights);
    }
    if (hr == S_OK) {
      const auto* bytes = static_cast<const BYTE*>(grantee);
      namedObjectsGrantee_.assign(bytes, bytes + GetLengthSid(grantee));
      hr = NamedObjectsAs(token_.get(), path, &how);
    } else {
      // Nothing was changed: nothing to take back. (S_FALSE: no DACL, which lets everyone in,
      // yet the account was refused; the check's answer stands.)
      namedObjectsDirectory_.reset();
      if (SUCCEEDED(hr)) hr = before;
      how = Text(HResultText(hr));
    }
    std::printf("[ system ] the %s may make objects there as its programs do: %s (%s)\n", noun(),
                SUCCEEDED(hr) ? "yes" : "no", how.c_str());
  }
  std::printf("[ system ] the %s may use the session's named objects: %s\n", noun(),
              YesNo(hr).c_str());
  if (SUCCEEDED(hr) && FAILED(global)) {
    // The session's own sign-in may well be refused there too; the app's lock is a "Global\"
    // name, and nothing here grants that.
    std::printf("[ system ] the %s may not make \"Global\\\" names (%s)\n", noun(),
                Text(HResultText(global)).c_str());
    hr = global;
  }
  namedObjects_ = hr;
  std::fflush(stdout);
}

void TestAccount::RestoreNamedObjects() noexcept {
  if (namedObjectsDirectory_ && !namedObjectsGrantee_.empty()) {
    const HRESULT hr = RevokeOnObject(namedObjectsDirectory_.get(), SE_KERNEL_OBJECT,
                                      namedObjectsGrantee_.data(), kNamedObjectRights);
    const char* entry = "is still there";
    if (hr == S_OK) {
      entry = "taken away";
    } else if (hr == S_FALSE) {
      entry = "was gone already";
    }
    std::printf(
        "[ system ] this session's named objects: permissions put back %s (0x%08lX; the %s's "
        "entry %s)\n",
        SUCCEEDED(hr) ? "yes" : "no", static_cast<unsigned long>(hr), noun(), entry);
    std::fflush(stdout);
  }
  namedObjectsGrantee_.clear();
  namedObjectsDirectory_.reset();
}

void TestAccount::EndItsProcesses() noexcept {
  if (!sidBytes_.empty()) EndProcessesOf(sidBytes_.data());
}

TestAccount::~TestAccount() {
  if (g_current == this) g_current = nullptr;
  if (g_standard == this) g_standard = nullptr;
  if (!created_) return;  // nothing was made
  EndItsProcesses();
  RestoreDesktop();
  RestoreNamedObjects();
  environment_.reset();
  bool unloaded = true;
  if (profileLoaded_) {
    const EnabledPrivileges hives({SE_BACKUP_NAME, SE_RESTORE_NAME});
    unloaded = UnloadUserProfile(token_.get(), profile_.hProfile) != FALSE;
    if (!unloaded) {
      std::printf("[ system ] %s: unloading its profile failed: 0x%08lX\n", noun(),
                  static_cast<unsigned long>(LastError()));
    }
    profileLoaded_ = false;
  }
  token_.reset();  // its logon session ends with the last of its tokens and processes
  const NET_API_STATUS status = NetUserDel(nullptr, name_.c_str());
  const bool deleted = status == NERR_Success;
  if (!deleted) {
    std::printf("[ system ] %s: NetUserDel failed: 0x%08lX\n", noun(),
                static_cast<unsigned long>(NetResult(status)));
  }
  std::erase_if(OwnAccounts(), [this](const std::wstring& own) { return own == name_; });
  // The profile goes after the account, whenever its SID is known: a LoadUserProfileW that
  // failed part of the way may have left its folder and its ProfileList entry.
  const char* profileDeleted = sid_.empty() ? "unknown (no SID)" : DeleteProfileOf(sid_, noun());
  std::printf(
      "[ system ] %s %ls (%ls) removed: profile unloaded %s, account deleted %s, profile "
      "deleted %s\n",
      noun(), name_.c_str(), sid_.empty() ? L"no SID" : sid_.c_str(), unloaded ? "yes" : "no",
      deleted ? "yes" : "no", profileDeleted);
  std::fflush(stdout);
}

const char* TestAccount::noun() const {
  return kind_ == Kind::kStandard ? "standard account" : "test account";
}

UserPlaces TestAccount::Places() const {
  UserPlaces places;
  places.who = std::string("the ") + noun();
  places.hive = hive();
  places.sid = sid_;
  places.userRoot = userRoot_;
  places.dataDir = dataDir_;
  return places;
}

Outcome TestAccount::Run(const path& exe, const std::wstring& arguments, DWORD timeoutMs) const {
  Outcome run;
  // CreateProcessWithTokenW hands the program this process's inheritable handles (it takes no
  // list of them): only the pipes' write ends and the null device are, and only until the
  // program has them.
  SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), nullptr, TRUE};
  wil::unique_handle outRead;
  wil::unique_handle outWrite;
  wil::unique_handle errRead;
  wil::unique_handle errWrite;
  if (!CreatePipe(outRead.put(), outWrite.put(), &inheritable, 1 << 16) ||
      !CreatePipe(errRead.put(), errWrite.put(), &inheritable, 1 << 16) ||
      !SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0) ||
      !SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0)) {
    run.hr = LastError();
    return run;
  }
  // Standard input is the null device: a real handle, as the Secondary Logon service hands each
  // standard handle on, and one that never waits for input.
  wil::unique_hfile nothingIn(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          &inheritable, OPEN_EXISTING, 0, nullptr));
  if (!nothingIn) {
    run.hr = LastError();
    return run;
  }
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  startup.wShowWindow = SW_HIDE;
  startup.hStdInput = nothingIn.get();
  startup.hStdOutput = outWrite.get();
  startup.hStdError = errWrite.get();
  std::wstring commandLine = test::QuoteArgument(exe.native()) + L" " + arguments;
  PROCESS_INFORMATION process = {};
  // Started by the Secondary Logon service, so not this process's console child: a hidden
  // console of its own.
  if (!CreateProcessWithTokenW(token_.get(), 0, exe.c_str(), commandLine.data(),
                               CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT, environment_.get(),
                               profileDir_.c_str(), &startup, &process)) {
    run.hr = LastError();
    return run;
  }
  const wil::unique_handle thread(process.hThread);
  const wil::unique_handle child(process.hProcess);
  // Only the program holds the write ends now: reading ends when it ends.
  outWrite.reset();
  errWrite.reset();
  nothingIn.reset();
  std::thread outReader(ReadAll, outRead.get(), &run.out);
  std::thread errReader(ReadAll, errRead.get(), &run.err);
  run.hr = S_OK;
  if (WaitForSingleObject(child.get(), timeoutMs) != WAIT_OBJECT_0) {
    TerminateProcess(child.get(), 1);
    WaitForSingleObject(child.get(), 10000);
    run.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  } else if (!GetExitCodeProcess(child.get(), &run.exitCode)) {
    run.hr = LastError();
  }
  outReader.join();
  errReader.join();
  return run;
}

}  // namespace pdft::systest
