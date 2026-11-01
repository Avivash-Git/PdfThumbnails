#include "system_env.h"

#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <cstdio>
#include <cwchar>
#include <utility>

#include "account_rules.h"
#include "child_process.h"
#include "context.h"
#include "elevation.h"
#include "file_ops.h"
#include "inno.h"
#include "instance.h"
#include "paths.h"
#include "project_config.h"
#include "registration.h"
#include "registry_roots.h"
#include "strings.h"
#include "tasks.h"
#include "test_account.h"

namespace pdft::systest {
namespace {

using std::filesystem::path;

constexpr wchar_t kSessionManagerKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
constexpr wchar_t kPendingRenamesValue[] = L"PendingFileRenameOperations";

bool Exists(const path& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Text(std::wstring_view text) { return WideToUtf8Lossy(text); }

RegistryRoots RootsOf(const UserPlaces& user) {
  return RegistryRoots{user.hive, HKEY_LOCAL_MACHINE};
}

// The per-user folders of |user| that exist or not, each named for the lines Leftovers prints.
std::vector<std::pair<std::string, path>> UserFolders(const UserPlaces& user) {
  std::vector<std::pair<std::string, path>> folders;
  if (!user.userRoot.empty())
    folders.emplace_back("the per-user folder of " + user.who, user.userRoot);
  if (!user.dataDir.empty()) folders.emplace_back("the data folder of " + user.who, user.dataDir);
  return folders;
}

Outcome ToOutcome(const test::CapturedRun& captured) {
  Outcome run;
  run.hr = captured.hr;
  run.exitCode = captured.exitCode;
  run.out = captured.out;
  run.err = captured.err;
  return run;
}

// What Leftovers finds of |user|'s own: their class, slots, settings, RunOnce values, check task,
// install and data folders, and an installer's record for them alone.
void UserLeftovers(const UserPlaces& user, std::vector<std::string>* found) {
  const RegistryRoots roots = RootsOf(user);
  const std::string of = " of " + user.who;
  bool exists = false;
  if (FAILED(RegistryKeyExists(user.hive, ClassKeyPath(), &exists)) || exists) {
    found->push_back("the handler's class in HKCU" + of);
  }
  SlotValues slots;
  if (SUCCEEDED(ReadSlots(roots, &slots))) {
    for (size_t i = 0; i < kSlotCount; ++i) {
      if (slots[i] && IsOurClsid(*slots[i]))
        found->push_back("HKCU slot " + Text(SlotName(i)) + of);
    }
  } else {
    found->push_back("the .pdf slots" + of + " (cannot be read)");
  }
  bool settings = false;
  if (FAILED(RegistryKeyExists(user.hive, kRegKey, &settings)) || settings) {
    found->push_back("HKCU\\" + Text(kRegKey) + of);
  }
  std::vector<std::wstring> runOnce;
  if (FAILED(FindRunOnceRemovals(roots, &runOnce)))
    found->push_back("RunOnce" + of + " (cannot be read)");
  for (const std::wstring& name : runOnce) found->push_back("RunOnce value " + Text(name) + of);
  TaskLocation location;
  TaskState task;
  if (SUCCEEDED(DefaultTaskLocation(user.sid, &location)) && SUCCEEDED(ReadTask(location, &task))) {
    if (task.exists) found->push_back("the check task" + of + " " + Text(task.path));
  } else {
    found->push_back("the check task" + of + " (cannot be read)");
  }
  for (const auto& [what, dir] : UserFolders(user)) {
    if (Exists(dir)) found->push_back(what + " " + Text(dir.native()));
  }
  bool record = false;
  if (FAILED(RegistryKeyExists(user.hive, InnoUninstallKey(), &record)) || record) {
    found->push_back("an installer's uninstall record" + of);
  }
}

}  // namespace

std::wstring EnvironmentValue(const wchar_t* name) {
  wchar_t buffer[2048] = {};
  const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
  if (length == 0 || length >= ARRAYSIZE(buffer)) return {};
  return std::wstring(buffer, length);
}

std::optional<std::string> WhyNotHere() {
  if (std::optional<std::string> why = WhyNotAThrowawayPc(
          EnvironmentValue(kEnableEnv), EnvironmentValue(kGitHubActionsEnv),
          EnvironmentValue(kRunnerEnvironmentEnv), EnvironmentValue(kThrowawayVmEnv))) {
    return why;
  }
  bool elevated = false;
  if (FAILED(GetTokenElevation(&elevated)) || !elevated) {
    return std::string(
        "not elevated: the tests check what the administrator steps did, run the installer's "
        "administrator commands themselves and make the account they run as");
  }
  ProcessContext context;
  if (const HRESULT hr = GetProcessContext(ContextProbe::kWrite, &context); FAILED(hr)) {
    return "cannot read this process's context: " + Text(HResultText(hr));
  }
  if (context.packagedAncestor) {
    return "a parent process runs in an app package (" + Text(*context.packagedAncestor) +
           "), so writes may not reach the real registry";
  }
  if (context.virtualization != Virtualization::kNo) {
    return std::string(
        "the registry may be virtualized here, so writes may not reach the real "
        "registry");
  }
  return std::nullopt;
}

std::vector<UserPlaces> TestedUsers() {
  std::vector<UserPlaces> users;
  UserPlaces self;
  self.who = "this account";
  self.hive = HKEY_CURRENT_USER;
  // What cannot be found is listed as unreadable by Leftovers, never skipped.
  (void)GetCurrentUserSid(&self.sid);
  (void)GetUserRoot(&self.userRoot);
  (void)GetDataDir(&self.dataDir);
  users.push_back(std::move(self));
  if (const TestAccount* account = CurrentTestAccount()) users.push_back(account->Places());
  return users;
}

std::optional<bool> TaskFolderExists(std::wstring_view folder) {
  wil::com_ptr_nothrow<ITaskService> service;
  HRESULT hr = CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(service.put()));
  VARIANT empty;
  VariantInit(&empty);
  if (SUCCEEDED(hr)) hr = service->Connect(empty, empty, empty, empty);
  const wil::unique_bstr name = wil::make_bstr_nothrow(std::wstring(folder).c_str());
  if (SUCCEEDED(hr) && !name) hr = E_OUTOFMEMORY;
  wil::com_ptr_nothrow<ITaskFolder> found;
  if (SUCCEEDED(hr)) hr = service->GetFolder(name.get(), found.put());
  if (SUCCEEDED(hr)) return true;
  if (IsTaskNotFound(hr)) return false;
  return std::nullopt;
}

std::vector<std::string> Leftovers() {
  std::vector<std::string> found;
  bool exists = false;
  if (FAILED(RegistryKeyExists(HKEY_LOCAL_MACHINE, ClassKeyPath(), &exists)) || exists) {
    found.push_back("the handler's class in HKLM");
  }
  SlotValues user;
  SlotValues machine;
  if (SUCCEEDED(ReadSlots(RegistryRoots{}, &user, &machine))) {
    for (size_t i = 0; i < kSlotCount; ++i) {
      if (machine[i] && IsOurClsid(*machine[i])) found.push_back("HKLM slot " + Text(SlotName(i)));
    }
  } else {
    found.push_back("the .pdf slots for all users (cannot be read)");
  }
  const std::optional<bool> folder = TaskFolderExists(kTaskFolder);
  if (!folder || *folder) found.push_back("the task folder " + Text(kTaskFolder));
  bool legacyTask = false;
  if (FAILED(LegacyTaskExists(&legacyTask)) || legacyTask) {
    found.push_back("1.x's task " + Text(kLegacyTask));
  }
  path machineRoot;
  if (SUCCEEDED(GetMachineRoot(&machineRoot)) && Exists(machineRoot)) {
    found.push_back("the folder for all users " + Text(machineRoot.native()));
  }
  bool record = false;
  if (FAILED(RegistryKeyExists(HKEY_LOCAL_MACHINE, InnoUninstallKey(), &record)) || record) {
    found.push_back("an installer's uninstall record for all users " + Text(InnoUninstallKey()));
  }
  for (const UserPlaces& tested : TestedUsers()) UserLeftovers(tested, &found);
  return found;
}

std::vector<path> TrashFolders() {
  std::vector<path> trash;
  path root;
  if (FAILED(GetMachineRoot(&root))) return trash;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(root.parent_path(), error)) {
    if (IsTrashFolderName(entry.path().filename().native())) trash.push_back(entry.path());
  }
  return trash;
}

bool QueuedForRestart(const path& target) {
  DWORD bytes = 0;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, kSessionManagerKey, kPendingRenamesValue,
                   RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) {
    return false;
  }
  std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 2, L'\0');
  bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
  if (RegGetValueW(HKEY_LOCAL_MACHINE, kSessionManagerKey, kPendingRenamesValue,
                   RRF_RT_REG_MULTI_SZ, nullptr, data.data(), &bytes) != ERROR_SUCCESS) {
    return false;
  }
  // Pairs of strings: the source ("\??\C:\..."), then the target, empty for a deletion.
  std::vector<std::wstring> entries;
  for (const wchar_t* at = data.data(); *at; at += wcslen(at) + 1) entries.emplace_back(at);
  for (size_t i = 0; i + 1 < entries.size(); i += 2) {
    std::wstring_view source = entries[i];
    if (source.starts_with(L"\\??\\")) source.remove_prefix(4);
    if (entries[i + 1].empty() && IEquals(source, target.native())) return true;
  }
  return false;
}

void ForceClean() {
  const auto report = [](const std::string& what, HRESULT hr) {
    if (FAILED(hr)) {
      std::printf("[ system ] clean-up: %s failed: %s\n", what.c_str(),
                  Text(HResultText(hr)).c_str());
    }
  };
  report("deleting the check tasks", DeleteTaskFolderAllUsers(kTaskFolder, kTaskRootPrefix));
  report("deleting 1.x's task", DeleteLegacyTask());
  report("deleting the HKLM class", DeleteRegistryTree(HKEY_LOCAL_MACHINE, ClassKeyPath()));
  for (const UserPlaces& user : TestedUsers()) {
    const RegistryRoots roots = RootsOf(user);
    const std::string of = " of " + user.who;
    report("removing the slots" + of, RemoveSlotsIfOurs(roots));
    report("deleting the HKCU class" + of, DeleteRegistryTree(user.hive, ClassKeyPath()));
    report("deleting the settings" + of, DeleteRegistryTree(user.hive, kRegKey));
    report("deleting the RunOnce values" + of, DeleteRunOnceRemovals(roots));
    for (const auto& [what, dir] : UserFolders(user)) {
      if (Exists(dir)) report("removing " + what, RemoveTreeNoFollow(dir));
    }
  }
  path machineRoot;
  if (SUCCEEDED(GetMachineRoot(&machineRoot)) && Exists(machineRoot)) {
    report("removing the folder for all users", RemoveTreeNoFollow(machineRoot));
  }
  for (const path& trash : TrashFolders()) {
    report("removing a trash folder", RemoveTreeNoFollow(trash));
  }
}

Outcome RunAsUser(const path& exe, const std::wstring& arguments) {
  const TestAccount* account = CurrentTestAccount();
  if (!account) {
    Outcome run;
    run.hr = E_UNEXPECTED;
    return run;
  }
  return account->Run(exe, arguments, kCommandTimeoutMs);
}

Outcome RunAsAdministrator(const path& exe, const std::wstring& arguments) {
  return ToOutcome(test::RunCaptured(exe.native(), arguments, kCommandTimeoutMs));
}

}  // namespace pdft::systest
