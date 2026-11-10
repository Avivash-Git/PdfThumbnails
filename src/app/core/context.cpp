#include "context.h"

#include <appmodel.h>
#include <objbase.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <filesystem>
#include <unordered_map>

#include "instance.h"
#include "paths.h"
#include "pe_info.h"
#include "project_config.h"
#include "registry_roots.h"
#include "strings.h"
#include "wmi_registry.h"
#include "zoom.h"

namespace pdft {
namespace {

// How far up the parent chain the package search looks.
constexpr int kMaxAncestors = 32;

// The canary's value name under the probe key.
constexpr wchar_t kProbeValue[] = L"Canary";

// The token's user SID, copied into |storage|.
HRESULT TokenUserSid(HANDLE token, BYTE (&storage)[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE],
                     PSID* sid) noexcept {
  DWORD size = 0;
  RETURN_IF_WIN32_BOOL_FALSE(
      GetTokenInformation(token, TokenUser, storage, sizeof(storage), &size));
  *sid = reinterpret_cast<TOKEN_USER*>(storage)->User.Sid;
  return S_OK;
}

// A new GUID as "{xxxxxxxx-...}".
HRESULT NewGuidString(std::wstring* text) noexcept try {
  GUID guid = {};
  RETURN_IF_FAILED(CoCreateGuid(&guid));
  wchar_t buffer[40] = {};
  RETURN_HR_IF(E_UNEXPECTED, StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) == 0);
  *text = buffer;
  return S_OK;
}
CATCH_RETURN();

// The package full name |process| runs in, if any.
std::optional<std::wstring> PackageOf(HANDLE process) {
  UINT32 length = 0;
  if (GetPackageFullName(process, &length, nullptr) != ERROR_INSUFFICIENT_BUFFER) {
    return std::nullopt;  // APPMODEL_ERROR_NO_PACKAGE: not packaged
  }
  std::wstring name(length, L'\0');
  if (GetPackageFullName(process, &length, name.data()) != ERROR_SUCCESS || length == 0) {
    return std::nullopt;
  }
  name.resize(length - 1);  // |length| counts the terminating null
  return name;
}

// The package family of a package full name.
std::optional<std::wstring> FamilyOf(const std::wstring& fullName) {
  UINT32 length = 0;
  if (PackageFamilyNameFromFullName(fullName.c_str(), &length, nullptr) !=
      ERROR_INSUFFICIENT_BUFFER) {
    return std::nullopt;
  }
  std::wstring family(length, L'\0');
  if (PackageFamilyNameFromFullName(fullName.c_str(), &length, family.data()) != ERROR_SUCCESS ||
      length == 0) {
    return std::nullopt;
  }
  family.resize(length - 1);
  return family;
}

// The package family this executable was installed from (a future MSIX build of the app), or
// std::nullopt. A process started from a packaged app can carry that app's identity too, so the
// identity counts as ours only when this executable lies inside the package's own folder.
std::optional<std::wstring> OwnPackageFamily() {
  std::optional<std::wstring> fullName = PackageOf(GetCurrentProcess());
  if (!fullName) return std::nullopt;
  UINT32 length = 0;
  if (GetCurrentPackagePath(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER) return std::nullopt;
  std::wstring folder(length, L'\0');
  if (GetCurrentPackagePath(&length, folder.data()) != ERROR_SUCCESS || length == 0) {
    return std::nullopt;
  }
  folder.resize(length - 1);
  std::filesystem::path exe;
  if (FAILED(GetOwnExe(&exe)) || !IsUnder(exe, folder)) return std::nullopt;
  return FamilyOf(*fullName);
}

// |package| is foreign: not this app's own package.
bool IsForeign(const std::wstring& package, const std::optional<std::wstring>& ownFamily) {
  if (!ownFamily) return true;
  const std::optional<std::wstring> family = FamilyOf(package);
  return !family || !IEquals(*family, *ownFamily);
}

// The machine type in this executable's own PE header: the process architecture even where
// IsWow64Process2 reports none (x64 code emulated on ARM64 is not WOW64).
USHORT OwnImageMachine() {
  const auto* base = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
  return nt->FileHeader.Machine;
}

// True when the desktop's shell runs as another user than |userSid|. A process can always
// query its own user's processes, so "access denied" means another user's shell.
bool DifferentFromShellUser(PSID userSid) {
  HWND shell = GetShellWindow();
  if (!shell) return false;
  DWORD processId = 0;
  if (!GetWindowThreadProcessId(shell, &processId) || !processId) return false;
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId));
  if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
  wil::unique_handle token;
  if (!OpenProcessToken(process.get(), TOKEN_QUERY, token.put())) {
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  alignas(TOKEN_USER) BYTE storage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  PSID shellSid = nullptr;
  if (FAILED(TokenUserSid(token.get(), storage, &shellSid))) return false;
  return !EqualSid(userSid, shellSid);
}

HRESULT FromStatus(LSTATUS status) noexcept {
  return HRESULT_FROM_WIN32(static_cast<DWORD>(status));
}

ValueView Unreadable(HRESULT error) { return {ValueState::kUnreadable, {}, error}; }

// A REG_SZ value of HKCU as this process sees it.
ValueView ReadOwnString(const std::wstring& subKey, const wchar_t* valueName) {
  ValueView view;
  DWORD size = 0;
  LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, subKey.c_str(), valueName, RRF_RT_REG_SZ,
                                nullptr, nullptr, &size);
  if (status == ERROR_FILE_NOT_FOUND) return view;
  if (status != ERROR_SUCCESS) return Unreadable(FromStatus(status));
  std::wstring data(size / sizeof(wchar_t) + 1, L'\0');
  size = static_cast<DWORD>(data.size() * sizeof(wchar_t));
  status = RegGetValueW(HKEY_CURRENT_USER, subKey.c_str(), valueName, RRF_RT_REG_SZ, nullptr,
                        data.data(), &size);
  if (status != ERROR_SUCCESS) return Unreadable(FromStatus(status));
  data.resize(wcsnlen(data.c_str(), data.size()));
  return {ValueState::kPresent, std::move(data)};
}

// A REG_DWORD value of HKCU as this process sees it, in decimal.
ValueView ReadOwnDword(const std::wstring& subKey, const wchar_t* valueName) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, subKey.c_str(), valueName,
                                      RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (status == ERROR_FILE_NOT_FOUND) return {};
  if (status != ERROR_SUCCESS) return Unreadable(FromStatus(status));
  return {ValueState::kPresent, std::to_wstring(value)};
}

// The first failed read among |values|, in order (this process's view first); S_OK if none.
HRESULT FirstReadError(std::span<const ValueViews> values) noexcept {
  for (const ValueViews& value : values) {
    if (FAILED(value.inProcess.error)) return value.inProcess.error;
    if (FAILED(value.outside.error)) return value.outside.error;
  }
  return S_OK;
}

}  // namespace

Virtualization DecideCanary(bool probeWritten, bool wmiAnswered,
                            const std::optional<std::wstring>& seen,
                            std::wstring_view written) noexcept {
  if (!probeWritten || !wmiAnswered) return Virtualization::kUnknown;
  if (!seen || *seen != written) return Virtualization::kYes;
  return Virtualization::kNo;
}

RegistryViews DecideFromViews(std::span<const ValueViews> values, bool packagedAncestor) noexcept {
  bool unreadable = false;
  bool comparedEqual = false;
  for (const ValueViews& value : values) {
    if (value.inProcess.state == ValueState::kUnreadable ||
        value.outside.state == ValueState::kUnreadable) {
      unreadable = true;
      continue;
    }
    if (value.inProcess.state != value.outside.state) return RegistryViews::kDiffer;
    if (value.inProcess.state == ValueState::kPresent) {
      if (value.inProcess.data != value.outside.data) return RegistryViews::kDiffer;
      comparedEqual = true;
    }
  }
  if (unreadable) return RegistryViews::kUnknown;
  if (comparedEqual) return RegistryViews::kAgree;
  // Nothing to compare. Outside any package that is fine; inside one it proves nothing.
  return packagedAncestor ? RegistryViews::kUnknown : RegistryViews::kAgree;
}

// The same mapping as for DLLs, so a process and a DLL of one architecture always compare equal.
Arch ArchFromMachine(USHORT machine) noexcept { return MachineToArch(machine); }

HRESULT FindPackagedAncestor(std::optional<std::wstring>* packageFullName) noexcept try {
  packageFullName->reset();
  const std::optional<std::wstring> ownFamily = OwnPackageFamily();

  // This process first: one started from a packaged app usually runs in that app's package.
  if (std::optional<std::wstring> package = PackageOf(GetCurrentProcess());
      package && IsForeign(*package, ownFamily)) {
    *packageFullName = std::move(package);
    return S_OK;
  }

  HANDLE rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  RETURN_LAST_ERROR_IF(rawSnapshot == INVALID_HANDLE_VALUE);
  wil::unique_handle snapshot(rawSnapshot);
  std::unordered_map<DWORD, DWORD> parents;
  PROCESSENTRY32W entry = {sizeof(entry)};
  for (BOOL more = Process32FirstW(snapshot.get(), &entry); more;
       more = Process32NextW(snapshot.get(), &entry)) {
    parents[entry.th32ProcessID] = entry.th32ParentProcessID;
  }

  FILETIME childCreated = {};
  FILETIME unused[3] = {};
  RETURN_IF_WIN32_BOOL_FALSE(
      GetProcessTimes(GetCurrentProcess(), &childCreated, &unused[0], &unused[1], &unused[2]));
  DWORD child = GetCurrentProcessId();
  for (int level = 0; level < kMaxAncestors; ++level) {
    const auto found = parents.find(child);
    if (found == parents.end() || found->second == 0 || found->second == child) break;
    const DWORD parent = found->second;
    // A parent that is gone, or not ours to look at (a service): the walk ends here.
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parent));
    if (!process) break;
    FILETIME created = {};
    if (!GetProcessTimes(process.get(), &created, &unused[0], &unused[1], &unused[2])) break;
    // Created after its "child": the real parent ended and its process id was reused.
    if (CompareFileTime(&created, &childCreated) > 0) break;
    if (std::optional<std::wstring> package = PackageOf(process.get());
        package && IsForeign(*package, ownFamily)) {
      *packageFullName = std::move(package);
      return S_OK;
    }
    child = parent;
    childCreated = created;
  }
  return S_OK;
}
CATCH_RETURN();

namespace {

// A canary under |root| (opened in the registry view |view|): the volatile key
// <|keyPrefix|>.probe-<GUID> holding a random value, read back through WMI from |wmiRoot| at
// |wmiPrefix| + the key's name, and deleted again on every path (ProbeRegistryVirtualization)
// through the handle it was made with.
HRESULT RunCanary(HKEY root, REGSAM view, DWORD wmiRoot, std::wstring_view wmiPrefix,
                  std::wstring_view keyPrefix, Virtualization* result, HRESULT* failure,
                  std::wstring* probeKey) noexcept try {
  *result = Virtualization::kUnknown;
  if (failure) *failure = S_OK;
  if (probeKey) probeKey->clear();
  std::wstring guid;
  std::wstring written;
  RETURN_IF_FAILED(NewGuidString(&guid));
  RETURN_IF_FAILED(NewGuidString(&written));
  const std::wstring keyName = std::wstring(keyPrefix) + L".probe-" + guid;
  if (probeKey) *probeKey = keyName;

  wil::unique_hkey key;
  const LSTATUS created =
      RegCreateKeyExW(root, keyName.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
                      KEY_SET_VALUE | DELETE | view, nullptr, key.put(), nullptr);
  // From here on the key goes away on every path, whatever happened. The handle stays open until
  // then, and the key is deleted through it, never by its name: under HKCU\Software, which its
  // user may change, the key at that name may meanwhile be another one, or a link to a key that
  // only an administrator may delete. Such a key stays as it is.
  const auto remove = wil::scope_exit([&]() noexcept {
    if (key) (void)DeleteOpenRegistryKey(key.get());
  });
  // The first failure, kept for when the verdict is "unknown": the log is not on yet while the
  // context is read, so WIL's own reports of these failures go nowhere.
  HRESULT firstFailure = created == ERROR_SUCCESS ? S_OK : FromStatus(created);
  if (SUCCEEDED(firstFailure)) {
    const LSTATUS set = RegSetValueExW(key.get(), kProbeValue, 0, REG_SZ,
                                       reinterpret_cast<const BYTE*>(written.c_str()),
                                       static_cast<DWORD>((written.size() + 1) * sizeof(wchar_t)));
    if (set != ERROR_SUCCESS) firstFailure = FromStatus(set);
  }
  const bool probeWritten = SUCCEEDED(firstFailure);

  bool wmiAnswered = false;
  std::optional<std::wstring> seen;
  if (probeWritten) {
    RegistryThroughWmi wmi;
    HRESULT hr = wmi.Connect();
    if (SUCCEEDED(hr)) {
      hr = wmi.GetString(wmiRoot, std::wstring(wmiPrefix) + keyName, kProbeValue, &seen);
    }
    wmiAnswered = SUCCEEDED(hr);
    if (FAILED(hr)) firstFailure = hr;
  }
  *result = DecideCanary(probeWritten, wmiAnswered, seen, written);
  if (failure && *result == Virtualization::kUnknown) *failure = firstFailure;
  return S_OK;
}
CATCH_RETURN();

const wchar_t* VerdictName(Virtualization virtualization) noexcept {
  switch (virtualization) {
    case Virtualization::kNo:
      return L"real";
    case Virtualization::kYes:
      return L"virtualized";
    case Virtualization::kUnknown:
      break;
  }
  return L"unknown";
}

}  // namespace

HRESULT ProbeRegistryVirtualization(const std::wstring& userSid, Virtualization* result,
                                    HRESULT* failure, std::wstring* probeKey) noexcept try {
  *result = Virtualization::kUnknown;
  return RunCanary(HKEY_CURRENT_USER, 0, RegistryThroughWmi::kUsers, userSid + L"\\", kRegKey,
                   result, failure, probeKey);
}
CATCH_RETURN();

HRESULT ProbeMachineRegistryVirtualization(Virtualization* result, HRESULT* failure,
                                           std::wstring* probeKey) noexcept try {
  *result = Virtualization::kUnknown;
  // Where the class for all users is written, so that the test write goes through the same
  // redirection as the step's own writes would.
  return RunCanary(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, RegistryThroughWmi::kLocalMachine, L"",
                   std::wstring(L"Software\\Classes\\") + kProductName, result, failure, probeKey);
}
CATCH_RETURN();

bool BelowPackage(const MachineWriteCheck& check) noexcept {
  return check.packageAbove || FAILED(check.walk);
}

bool RefusesMachineWrites(const MachineWriteCheck& check) noexcept {
  return BelowPackage(check) &&
         (!check.probed || RefusesWrites(check.machine) || RefusesWrites(check.user));
}

std::wstring MachineWriteCheckText(const MachineWriteCheck& check) {
  if (!BelowPackage(check)) return L"no app package above";
  std::wstring text = check.packageAbove
                          ? std::wstring(L"app package above")
                          : L"processes above unreadable, walk-hr=" + HResultText(check.walk);
  if (!check.probed) {
    text += L"; not checked";
  } else {
    text += std::wstring(L"; HKLM ") + VerdictName(check.machine) + L", HKCU " +
            VerdictName(check.user);
  }
  if (FAILED(check.probeError)) text += L", probe-hr=" + HResultText(check.probeError);
  return text;
}

void ReadPackageAbove(MachineWriteCheck* check) noexcept {
  *check = MachineWriteCheck{};
  std::optional<std::wstring> package;
  const HRESULT walk = FindPackagedAncestor(&package);
  if (FAILED(walk)) {
    check->walk = walk;
  } else {
    check->packageAbove = package.has_value();
  }
}

void ProbeMachineWrites(MachineWriteCheck* check) noexcept {
  check->probed = true;
  check->probeError = S_OK;
  HRESULT machineFailure = S_OK;
  if (const HRESULT hr = ProbeMachineRegistryVirtualization(&check->machine, &machineFailure);
      FAILED(hr)) {
    check->machine = Virtualization::kUnknown;
    machineFailure = hr;
  }
  HRESULT userFailure = S_OK;
  std::wstring sid;
  HRESULT hr = GetCurrentUserSid(&sid);
  if (SUCCEEDED(hr)) hr = ProbeRegistryVirtualization(sid, &check->user, &userFailure);
  if (FAILED(hr)) {
    check->user = Virtualization::kUnknown;
    userFailure = hr;
  }
  check->probeError = FAILED(machineFailure) ? machineFailure : userFailure;
}

void CheckMachineWrites(MachineWriteCheck* check) noexcept {
  ReadPackageAbove(check);
  if (BelowPackage(*check)) ProbeMachineWrites(check);
}

HRESULT CompareRegistryViews(const std::wstring& userSid, bool packagedAncestor,
                             RegistryViews* result, HRESULT* failure) noexcept try {
  *result = RegistryViews::kUnknown;
  if (failure) *failure = S_OK;
  const std::wstring slotKey =
      std::wstring(L"Software\\Classes\\.pdf\\ShellEx\\") + kThumbSlotIidString;
  const std::wstring productKey = kRegKey;
  ValueViews views[2];
  views[0].inProcess = ReadOwnString(slotKey, nullptr);
  views[1].inProcess = ReadOwnDword(productKey, kZoomValueName);

  RegistryThroughWmi wmi;
  if (const HRESULT connected = wmi.Connect(); FAILED(connected)) {
    views[0].outside = Unreadable(connected);
    views[1].outside = Unreadable(connected);
  } else {
    std::optional<std::wstring> slot;
    if (const HRESULT hr =
            wmi.GetString(RegistryThroughWmi::kUsers, userSid + L"\\" + slotKey, L"", &slot);
        FAILED(hr)) {
      views[0].outside = Unreadable(hr);
    } else if (slot) {
      views[0].outside = {ValueState::kPresent, *slot};
    }
    std::optional<DWORD> zoom;
    if (const HRESULT hr = wmi.GetDword(RegistryThroughWmi::kUsers, userSid + L"\\" + productKey,
                                        kZoomValueName, &zoom);
        FAILED(hr)) {
      views[1].outside = Unreadable(hr);
    } else if (zoom) {
      views[1].outside = {ValueState::kPresent, std::to_wstring(*zoom)};
    }
  }
  *result = DecideFromViews(views, packagedAncestor);
  if (failure && *result == RegistryViews::kUnknown) *failure = FirstReadError(views);
  return S_OK;
}
CATCH_RETURN();

HRESULT GetProcessContext(ContextProbe probe, ProcessContext* context) noexcept try {
  *context = ProcessContext{};
  wil::unique_handle token;
  RETURN_IF_WIN32_BOOL_FALSE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()));

  alignas(TOKEN_USER) BYTE userStorage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  PSID userSid = nullptr;
  RETURN_IF_FAILED(TokenUserSid(token.get(), userStorage, &userSid));
  wil::unique_hlocal_string sidText;
  RETURN_IF_WIN32_BOOL_FALSE(ConvertSidToStringSidW(userSid, sidText.put()));
  context->userSid = sidText.get();
  wchar_t name[256] = {};
  wchar_t domain[256] = {};
  DWORD nameLength = ARRAYSIZE(name);
  DWORD domainLength = ARRAYSIZE(domain);
  SID_NAME_USE use = SidTypeUnknown;
  if (LookupAccountSidW(nullptr, userSid, name, &nameLength, domain, &domainLength, &use)) {
    context->userAccount = std::wstring(domain) + L"\\" + name;
  } else {
    context->userAccount = context->userSid;  // e.g. a domain account while offline
  }

  alignas(TOKEN_MANDATORY_LABEL) BYTE label[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  RETURN_IF_WIN32_BOOL_FALSE(
      GetTokenInformation(token.get(), TokenIntegrityLevel, label, sizeof(label), &size));
  PSID labelSid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label)->Label.Sid;
  context->integrityRid = *GetSidSubAuthority(labelSid, *GetSidSubAuthorityCount(labelSid) - 1u);

  BYTE administrators[SECURITY_MAX_SID_SIZE];
  DWORD administratorsSize = sizeof(administrators);
  RETURN_IF_WIN32_BOOL_FALSE(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr,
                                                administrators, &administratorsSize));
  BOOL member = FALSE;
  RETURN_IF_WIN32_BOOL_FALSE(CheckTokenMembership(nullptr, administrators, &member));
  context->elevated = member || context->integrityRid > SECURITY_MANDATORY_MEDIUM_RID;

  context->differentFromShellUser = DifferentFromShellUser(userSid);

  USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
  USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
  if (IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) {
    context->osArch = ArchFromMachine(nativeMachine);
  }
  // Not WOW64 (IMAGE_FILE_MACHINE_UNKNOWN): the process runs its own image's architecture.
  context->processArch = ArchFromMachine(
      processMachine != IMAGE_FILE_MACHINE_UNKNOWN ? processMachine : OwnImageMachine());

  // A failed walk counts as "none found". Nothing records why: the log is not on yet while the
  // context is read, so WIL's report of the failure goes nowhere.
  if (FAILED(FindPackagedAncestor(&context->packagedAncestor))) context->packagedAncestor.reset();

  // Each check fills only its own field (context.h); a check that fails as a whole cannot
  // decide either.
  context->probe = probe;
  if (probe == ContextProbe::kWrite) {
    const HRESULT hr = ProbeRegistryVirtualization(context->userSid, &context->virtualization,
                                                   &context->probeError);
    if (FAILED(hr)) {
      context->virtualization = Virtualization::kUnknown;
      context->probeError = hr;
    }
  } else if (probe == ContextProbe::kReadOnly) {
    const HRESULT hr = CompareRegistryViews(context->userSid, context->packagedAncestor.has_value(),
                                            &context->views, &context->probeError);
    if (FAILED(hr)) {
      context->views = RegistryViews::kUnknown;
      context->probeError = hr;
    }
  }
  return S_OK;
}
CATCH_RETURN();

bool ShellRunsAsAnotherUser() noexcept {
  wil::unique_handle token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put())) return true;
  alignas(TOKEN_USER) BYTE storage[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  PSID userSid = nullptr;
  if (FAILED(TokenUserSid(token.get(), storage, &userSid))) return true;
  return DifferentFromShellUser(userSid);
}

}  // namespace pdft
