#include "test_account.h"

#include <aclapi.h>
#include <bcrypt.h>
#include <knownfolders.h>
#include <lm.h>
#include <sddl.h>
#include <shlobj.h>
#include <wtsapi32.h>
// Again, after the headers above: WIL adds its wrappers for what they declare.
#include <wil/resource.h>

#include <array>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <thread>
#include <utility>

#include "account_rules.h"
#include "context.h"
#include "medium_il.h"
#include "project_config.h"
#include "strings.h"

namespace pdft::systest {
namespace {

using std::filesystem::path;

TestAccount* g_current = nullptr;

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
    const wil::unique_handle process(
        OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.ProcessId));
    const bool ended = process && TerminateProcess(process.get(), 1) &&
                       WaitForSingleObject(process.get(), 5000) == WAIT_OBJECT_0;
    std::printf("[ system ] %ls (process %lu) still ran as a test account: ended %s\n",
                entry.pProcessName ? entry.pProcessName : L"?", entry.ProcessId,
                ended ? "yes" : "no");
  }
}

// Deletes the profile of the account |sid| (its folder and its ProfileList entry), trying again
// for a while: Windows may hold its files a moment after it was unloaded. "yes", "none" when
// it had no profile, else "no" (and a line that says why).
const char* DeleteProfileOf(const std::wstring& sid) noexcept {
  DWORD lastError = ERROR_SUCCESS;
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (attempt > 0) Sleep(1000);
    if (DeleteProfileW(sid.c_str(), nullptr, nullptr)) return "yes";
    lastError = GetLastError();
    if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND) return "none";
  }
  std::printf("[ system ] test account: DeleteProfileW failed: %lu\n", lastError);
  return "no";
}

// Removes the test accounts that earlier runs made and could not delete (they were ended from
// outside: a timeout, a cancelled job), with their profiles: IsTestAccount's name and comment,
// nothing else.
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
                          users[i].usri1_comment ? users[i].usri1_comment : L"")) {
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
    const char* profile = sid.empty() ? "unknown (no SID)" : DeleteProfileOf(sid);
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

// Lets |sid| have |rights| on the window station or desktop |object| (opened with READ_CONTROL
// and WRITE_DAC); |was| receives the permissions it had (LocalFree it).
HRESULT GrantOnUserObject(HANDLE object, PSID sid, ACCESS_MASK rights, PSECURITY_DESCRIPTOR* was) {
  PACL dacl = nullptr;
  DWORD error = GetSecurityInfo(object, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                nullptr, &dacl, nullptr, was);
  if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
  EXPLICIT_ACCESSW access = {};
  access.grfAccessPermissions = rights;
  access.grfAccessMode = GRANT_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
  PACL updated = nullptr;
  error = SetEntriesInAclW(1, &access, dacl, &updated);
  if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
  const wil::unique_hlocal updatedMemory(updated);
  return HRESULT_FROM_WIN32(SetSecurityInfo(object, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION,
                                            nullptr, nullptr, updated, nullptr));
}

// Puts back the permissions |object| had (GrantOnUserObject's |was|).
void RestoreUserObject(HANDLE object, PSECURITY_DESCRIPTOR was) noexcept {
  if (!object || !was) return;
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  if (GetSecurityDescriptorDacl(was, &present, &dacl, &defaulted) && present) {
    SetSecurityInfo(object, SE_WINDOW_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl,
                    nullptr);
  }
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

std::unique_ptr<TestAccount> TestAccount::Create(std::string* error) {
  error->clear();
  // Asked again right before anything is made, whatever the caller checked.
  if (const std::optional<std::string> why = WhyNoTestAccountHere()) {
    *error = "not made here: " + *why;
    return nullptr;
  }
  RemoveEarlierTestAccounts();
  std::unique_ptr<TestAccount> account(new TestAccount());

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

  // A member of Administrators, by the group's well-known SID (its name depends on the language).
  std::wstring group;
  if (hr = AdministratorsGroupName(&group); FAILED(hr)) {
    *error = Failed("looking up the Administrators group's name", hr);
    return hr;
  }
  LOCALGROUP_MEMBERS_INFO_0 member = {sidBytes_.data()};
  if (hr = NetResult(
          NetLocalGroupAddMembers(nullptr, group.c_str(), 0, reinterpret_cast<LPBYTE>(&member), 1));
      FAILED(hr)) {
    *error = Failed("NetLocalGroupAddMembers (Administrators)", hr);
    return hr;
  }

  // Logged on as a person signs in. With UAC on, a member of Administrators gets its limited
  // token from that, with the full one linked to it.
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
  if (hr = ReadTokenFacts(token_.get(), &limited_); FAILED(hr)) {
    *error = Failed("reading its token", hr);
    return hr;
  }
  TOKEN_LINKED_TOKEN linked = {};
  DWORD size = 0;
  if (GetTokenInformation(token_.get(), TokenLinkedToken, &linked, sizeof(linked), &size)) {
    const wil::unique_handle linkedToken(linked.LinkedToken);
    (void)ReadTokenFacts(linkedToken.get(), &linked_);
  }
  if (limited_.elevationType != TokenElevationTypeLimited) {
    *error = "its interactive logon gave this " + Text(TokenFactsText(limited_)) +
             ", not a limited token (elevation type 3) with a full one linked to it: UAC "
             "could not elevate its programs";
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
    station = station_ ? GrantOnUserObject(station_.get(), sidBytes_.data(), kStationRights,
                                           stationWas_.put())
                       : LastError();
    if (FAILED(station)) {
      stationWas_.reset();
      station_.reset();
    }
  }
  HRESULT desktop = E_FAIL;
  if (!desktopName.empty()) {
    desktop_.reset(OpenDesktopW(desktopName.c_str(), 0, FALSE, READ_CONTROL | WRITE_DAC));
    desktop = desktop_ ? GrantOnUserObject(desktop_.get(), sidBytes_.data(), kDesktopRights,
                                           desktopWas_.put())
                       : LastError();
    if (FAILED(desktop)) {
      desktopWas_.reset();
      desktop_.reset();
    }
  }
  std::printf(
      "[ system ] the test account may use window station %s: %s; desktop %s: %s\n",
      Text(stationName).c_str(), SUCCEEDED(station) ? "yes" : Text(HResultText(station)).c_str(),
      Text(desktopName).c_str(), SUCCEEDED(desktop) ? "yes" : Text(HResultText(desktop)).c_str());
}

void TestAccount::RestoreDesktop() noexcept {
  RestoreUserObject(desktop_.get(), desktopWas_.get());
  RestoreUserObject(station_.get(), stationWas_.get());
  desktopWas_.reset();
  desktop_.reset();
  stationWas_.reset();
  station_.reset();
}

void TestAccount::EndItsProcesses() noexcept {
  if (!sidBytes_.empty()) EndProcessesOf(sidBytes_.data());
}

TestAccount::~TestAccount() {
  if (g_current == this) g_current = nullptr;
  if (!created_) return;  // nothing was made
  EndItsProcesses();
  RestoreDesktop();
  environment_.reset();
  bool unloaded = true;
  if (profileLoaded_) {
    const EnabledPrivileges hives({SE_BACKUP_NAME, SE_RESTORE_NAME});
    unloaded = UnloadUserProfile(token_.get(), profile_.hProfile) != FALSE;
    if (!unloaded) {
      std::printf("[ system ] test account: unloading its profile failed: 0x%08lX\n",
                  static_cast<unsigned long>(LastError()));
    }
    profileLoaded_ = false;
  }
  token_.reset();  // its logon session ends with the last of its tokens and processes
  const NET_API_STATUS status = NetUserDel(nullptr, name_.c_str());
  const bool deleted = status == NERR_Success;
  if (!deleted) {
    std::printf("[ system ] test account: NetUserDel failed: 0x%08lX\n",
                static_cast<unsigned long>(NetResult(status)));
  }
  // The profile goes after the account, whenever its SID is known: a LoadUserProfileW that
  // failed part of the way may have left its folder and its ProfileList entry.
  const char* profileDeleted = sid_.empty() ? "unknown (no SID)" : DeleteProfileOf(sid_);
  std::printf(
      "[ system ] test account %ls (%ls) removed: profile unloaded %s, account deleted %s, "
      "profile deleted %s\n",
      name_.c_str(), sid_.empty() ? L"no SID" : sid_.c_str(), unloaded ? "yes" : "no",
      deleted ? "yes" : "no", profileDeleted);
  std::fflush(stdout);
}

UserPlaces TestAccount::Places() const {
  UserPlaces places;
  places.who = "the test account";
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
