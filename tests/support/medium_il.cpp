#include "medium_il.h"

#include <sddl.h>

#include <cstdio>
#include <vector>

namespace pdft::test {
namespace {

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE handle) : handle_(handle) {}
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  ~Handle() { reset(); }
  HANDLE get() const { return handle_; }
  HANDLE* out() {
    reset();
    return &handle_;
  }
  HANDLE release() {
    HANDLE handle = handle_;
    handle_ = nullptr;
    return handle;
  }
  void reset() {
    if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = nullptr;
  }

 private:
  HANDLE handle_ = nullptr;
};

HRESULT LastError() {
  const DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

// A well-known SID in caller-provided storage.
bool WellKnownSid(WELL_KNOWN_SID_TYPE type, BYTE (&storage)[SECURITY_MAX_SID_SIZE]) {
  DWORD size = sizeof(storage);
  return CreateWellKnownSid(type, nullptr, storage, &size) != FALSE;
}

// A new token takes its default DACL and owner from the source token. An elevated source grants
// only Administrators and SYSTEM, which a filtered token holds deny-only, so its process could
// not open its own token or the objects it creates. Gives |token| a standard user's defaults:
// owner = the token user, default DACL D:(A;;GA;;;<user>)(A;;GA;;;SY)(A;;GRGX;;;<logon SID>).
HRESULT SetUserDefaults(HANDLE token) {
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  if (!GetTokenInformation(token, TokenUser, user, sizeof(user), &size)) return LastError();
  PSID userSid = reinterpret_cast<TOKEN_USER*>(user)->User.Sid;
  BYTE system[SECURITY_MAX_SID_SIZE];
  if (!WellKnownSid(WinLocalSystemSid, system)) return LastError();
  alignas(TOKEN_GROUPS) BYTE logon[sizeof(TOKEN_GROUPS) + SECURITY_MAX_SID_SIZE];
  const bool hasLogon =
      GetTokenInformation(token, TokenLogonSid, logon, sizeof(logon), &size) &&
      reinterpret_cast<TOKEN_GROUPS*>(logon)->GroupCount > 0;  // none in some service tokens

  alignas(DWORD) BYTE acl[sizeof(ACL) + 3 * (sizeof(ACCESS_ALLOWED_ACE) + SECURITY_MAX_SID_SIZE)];
  PACL dacl = reinterpret_cast<PACL>(acl);
  if (!InitializeAcl(dacl, sizeof(acl), ACL_REVISION) ||
      !AddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, userSid) ||
      !AddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_ALL, system) ||
      (hasLogon && !AddAccessAllowedAce(dacl, ACL_REVISION, GENERIC_READ | GENERIC_EXECUTE,
                                        reinterpret_cast<TOKEN_GROUPS*>(logon)->Groups[0].Sid))) {
    return LastError();
  }
  TOKEN_DEFAULT_DACL defaultDacl = {dacl};
  TOKEN_OWNER owner = {userSid};
  if (!SetTokenInformation(token, TokenDefaultDacl, &defaultDacl, sizeof(defaultDacl)) ||
      !SetTokenInformation(token, TokenOwner, &owner, sizeof(owner))) {
    return LastError();
  }
  return S_OK;
}

// "<what> <SDDL>" for one part (OWNER_ or DACL_SECURITY_INFORMATION) of a descriptor.
void PrintSddl(const char* what, PSECURITY_DESCRIPTOR descriptor, SECURITY_INFORMATION part) {
  LPWSTR text = nullptr;
  if (ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, part, &text,
                                                           nullptr)) {
    std::printf("%s %ls\n", what, text);
    LocalFree(text);
  } else {
    std::printf("%s unreadable (error %lu)\n", what, GetLastError());
  }
}

}  // namespace

DWORD TokenIntegrityRid(HANDLE token) {
  alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  if (!GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
    return kUnknownIntegrity;
  }
  PSID sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer)->Label.Sid;
  const UCHAR count = *GetSidSubAuthorityCount(sid);
  return count ? *GetSidSubAuthority(sid, count - 1u) : kUnknownIntegrity;
}

DWORD CurrentIntegrityRid() {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.out())) return kUnknownIntegrity;
  return TokenIntegrityRid(token.get());
}

bool TokenHasEnabledAdministrators(HANDLE token) {
  Handle identification;
  if (!DuplicateToken(token, SecurityIdentification, identification.out())) return false;
  BYTE administrators[SECURITY_MAX_SID_SIZE];
  BOOL member = FALSE;
  if (!WellKnownSid(WinBuiltinAdministratorsSid, administrators) ||
      !CheckTokenMembership(identification.get(), administrators, &member)) {
    return false;
  }
  return member != FALSE;
}

bool CurrentTokenHasEnabledAdministrators() {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, token.out())) {
    return false;
  }
  return TokenHasEnabledAdministrators(token.get());
}

int ReportOwnToken() {
  Handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, token.out())) {
    std::printf("cannot open the process token (error %lu)\n", GetLastError());
    return 3;
  }
  const DWORD rid = TokenIntegrityRid(token.get());
  const bool admin = TokenHasEnabledAdministrators(token.get());
  TOKEN_ELEVATION_TYPE type = TokenElevationTypeDefault;
  DWORD size = 0;
  GetTokenInformation(token.get(), TokenElevationType, &type, sizeof(type), &size);
  // TokenElevation on its own: what the elevated side's entry check and UAC go by.
  TOKEN_ELEVATION elevation = {};
  const char* elevated =
      GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)
          ? (elevation.TokenIsElevated ? "yes" : "no")
          : "unknown";
  std::printf("integrity 0x%04lX, Administrators %s, elevation type %d, elevated %s\n", rid,
              admin ? "enabled" : "not enabled", static_cast<int>(type), elevated);

  // The owner and default DACL that objects this process creates get.
  SECURITY_DESCRIPTOR descriptor;
  InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION);
  alignas(TOKEN_OWNER) BYTE owner[sizeof(TOKEN_OWNER) + SECURITY_MAX_SID_SIZE];
  if (GetTokenInformation(token.get(), TokenOwner, owner, sizeof(owner), &size)) {
    SetSecurityDescriptorOwner(&descriptor, reinterpret_cast<TOKEN_OWNER*>(owner)->Owner, FALSE);
  }
  size = 0;
  GetTokenInformation(token.get(), TokenDefaultDacl, nullptr, 0, &size);  // the size it needs
  std::vector<BYTE> dacl(size);
  if (size && GetTokenInformation(token.get(), TokenDefaultDacl, dacl.data(), size, &size)) {
    SetSecurityDescriptorDacl(
        &descriptor, TRUE, reinterpret_cast<TOKEN_DEFAULT_DACL*>(dacl.data())->DefaultDacl, FALSE);
  }
  PrintSddl("owner", &descriptor, OWNER_SECURITY_INFORMATION);
  PrintSddl("default DACL", &descriptor, DACL_SECURITY_INFORMATION);
  std::fflush(stdout);
  return rid == SECURITY_MANDATORY_MEDIUM_RID && !admin ? 0 : 3;
}

bool IsApplicationControlBlock(HRESULT hr) {
  if (HRESULT_FACILITY(hr) != FACILITY_WIN32) return false;
  const DWORD code = HRESULT_CODE(hr);
  return code == ERROR_INVALID_IMAGE_HASH ||                 // 577
         code == ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION ||  // 4551
         (code >= 4556 && code <= 4559) || (code >= 4580 && code <= 4583);
}

HRESULT CreateMediumToken(HANDLE source, MediumToken kind, HANDLE* token) {
  if (!token) return E_POINTER;
  *token = nullptr;
  const DWORD rid = TokenIntegrityRid(source);
  if (rid == kUnknownIntegrity) return LastError();
  if (rid < SECURITY_MANDATORY_MEDIUM_RID) return E_INVALIDARG;

  Handle created;
  if (kind == MediumToken::kLabelOnly) {
    if (!DuplicateTokenEx(
            source, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY,
            nullptr, SecurityImpersonation, TokenPrimary, created.out())) {
      return LastError();
    }
  } else {
    BYTE administrators[SECURITY_MAX_SID_SIZE];
    if (!WellKnownSid(WinBuiltinAdministratorsSid, administrators)) return LastError();
    SID_AND_ATTRIBUTES disable = {administrators, 0};
    if (!CreateRestrictedToken(source, DISABLE_MAX_PRIVILEGE, 1, &disable, 0, nullptr, 0, nullptr,
                               created.out())) {
      return LastError();
    }
  }

  BYTE medium[SECURITY_MAX_SID_SIZE];
  if (!WellKnownSid(WinMediumLabelSid, medium)) return LastError();
  TOKEN_MANDATORY_LABEL label = {};
  label.Label.Attributes = SE_GROUP_INTEGRITY;
  label.Label.Sid = medium;
  if (!SetTokenInformation(created.get(), TokenIntegrityLevel, &label,
                           sizeof(label) + GetLengthSid(medium))) {
    return LastError();
  }
  if (TokenIntegrityRid(created.get()) != SECURITY_MANDATORY_MEDIUM_RID) return E_UNEXPECTED;
  const HRESULT hr = SetUserDefaults(created.get());
  if (FAILED(hr)) return hr;
  *token = created.release();
  return S_OK;
}

HRESULT CreateMediumTokenForSelf(MediumToken kind, HANDLE* token) {
  Handle self;
  if (!OpenProcessToken(GetCurrentProcess(),
                        TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ADJUST_DEFAULT | TOKEN_ASSIGN_PRIMARY,
                        self.out())) {
    return LastError();
  }
  return CreateMediumToken(self.get(), kind, token);
}

HRESULT RunWithToken(HANDLE token, const std::wstring& application, const std::wstring& commandLine,
                     DWORD* exitCode) {
  if (!exitCode) return E_POINTER;
  *exitCode = 0;

  // The child gets inheritable copies of our standard handles, and only those.
  const DWORD ids[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
  Handle copies[3];
  std::vector<HANDLE> inherit;
  for (int i = 0; i < 3; ++i) {
    HANDLE handle = GetStdHandle(ids[i]);
    if (handle && handle != INVALID_HANDLE_VALUE &&
        DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), copies[i].out(), 0, TRUE,
                        DUPLICATE_SAME_ACCESS)) {
      inherit.push_back(copies[i].get());
    }
  }

  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  std::vector<BYTE> attributes;
  DWORD flags = 0;
  if (!inherit.empty()) {
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    attributes.resize(size);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) {
      return LastError();
    }
    if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherit.data(), inherit.size() * sizeof(HANDLE), nullptr,
                                   nullptr)) {
      const HRESULT hr = LastError();
      DeleteProcThreadAttributeList(startup.lpAttributeList);
      return hr;
    }
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = copies[0].get();
    startup.StartupInfo.hStdOutput = copies[1].get();
    startup.StartupInfo.hStdError = copies[2].get();
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }

  std::wstring line = commandLine;  // CreateProcessAsUserW may write to it
  PROCESS_INFORMATION process = {};
  const BOOL started = CreateProcessAsUserW(
      token, application.empty() ? nullptr : application.c_str(), line.data(), nullptr, nullptr,
      inherit.empty() ? FALSE : TRUE, flags, nullptr, nullptr, &startup.StartupInfo, &process);
  const HRESULT startError = started ? S_OK : LastError();
  if (startup.lpAttributeList) DeleteProcThreadAttributeList(startup.lpAttributeList);
  if (!started) return startError;

  Handle processHandle(process.hProcess);
  Handle threadHandle(process.hThread);
  if (WaitForSingleObject(processHandle.get(), INFINITE) != WAIT_OBJECT_0) return LastError();
  if (!GetExitCodeProcess(processHandle.get(), exitCode)) return LastError();
  return S_OK;
}

std::wstring QuoteArgument(const std::wstring& argument) {
  if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
    return argument;
  }
  std::wstring out = L"\"";
  for (size_t i = 0;; ++i) {
    size_t backslashes = 0;
    while (i < argument.size() && argument[i] == L'\\') {
      ++i;
      ++backslashes;
    }
    if (i == argument.size()) {
      out.append(backslashes * 2, L'\\');  // they precede the closing quote
      break;
    }
    if (argument[i] == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
    } else {
      out.append(backslashes, L'\\');
    }
    out.push_back(argument[i]);
  }
  out.push_back(L'"');
  return out;
}

RelaunchDecision DecideRelaunch(DWORD integrityRid, bool administratorsEnabled,
                                bool alreadyRelaunched) {
  if (integrityRid != kUnknownIntegrity && integrityRid <= SECURITY_MANDATORY_MEDIUM_RID &&
      !administratorsEnabled) {
    return RelaunchDecision::kRunHere;
  }
  return alreadyRelaunched ? RelaunchDecision::kRefuse : RelaunchDecision::kRelaunch;
}

bool RelaunchAtMediumIfElevated(int* exitCode) {
  wchar_t flag[4] = {};
  const bool relaunched = GetEnvironmentVariableW(kRelaunchedEnv, flag, 4) == 1 && flag[0] == L'1';
  const DWORD rid = CurrentIntegrityRid();
  const bool administrators = CurrentTokenHasEnabledAdministrators();
  const char* const state = administrators ? "enabled" : "not enabled";
  switch (DecideRelaunch(rid, administrators, relaunched)) {
    case RelaunchDecision::kRunHere:
      if (relaunched) {
        SetEnvironmentVariableW(kRelaunchedEnv, nullptr);
        std::printf("pdft: relaunched at Medium integrity (0x%04lX), Administrators %s\n", rid,
                    state);
        std::fflush(stdout);
      }
      return false;
    case RelaunchDecision::kRefuse:
      std::fprintf(stderr,
                   "pdft: still at integrity 0x%04lX, Administrators %s, after relaunching at "
                   "Medium\n",
                   rid, state);
      *exitCode = 1;
      return true;
    case RelaunchDecision::kRelaunch:
      break;
  }

  std::printf(
      "pdft: running at integrity 0x%04lX, Administrators %s: relaunching at Medium as a standard "
      "user\n",
      rid, state);
  std::fflush(stdout);
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  Handle token;
  // Filtered, not label-only: the handler also refuses to render where Administrators is enabled.
  HRESULT hr = length == 0 || length >= MAX_PATH
                   ? E_FAIL
                   : CreateMediumTokenForSelf(MediumToken::kFiltered, token.out());
  DWORD code = 1;
  if (SUCCEEDED(hr)) {
    SetEnvironmentVariableW(kRelaunchedEnv, L"1");
    hr = RunWithToken(token.get(), self, GetCommandLineW(), &code);
    SetEnvironmentVariableW(kRelaunchedEnv, nullptr);
  }
  if (FAILED(hr)) {
    std::fprintf(stderr, "pdft: cannot relaunch at Medium integrity: 0x%08lX\n",
                 static_cast<unsigned long>(hr));
    code = 1;
  }
  *exitCode = static_cast<int>(code);
  return true;
}

}  // namespace pdft::test
