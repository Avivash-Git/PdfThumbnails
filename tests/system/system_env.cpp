#include "system_env.h"

#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <cstdio>
#include <cwchar>
#include <memory>
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
// Newer Windows may keep such operations here as well.
constexpr wchar_t kPendingRenames2Value[] = L"PendingFileRenameOperations2";

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

HRESULT ConnectTaskService(wil::com_ptr_nothrow<ITaskService>* service) {
  HRESULT hr = CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(service->put()));
  VARIANT empty;
  VariantInit(&empty);
  if (SUCCEEDED(hr)) hr = (*service)->Connect(empty, empty, empty, empty);
  return hr;
}

HRESULT GetTaskFolder(ITaskService* service, std::wstring_view path,
                      wil::com_ptr_nothrow<ITaskFolder>* folder) {
  const wil::unique_bstr name = wil::make_bstr_nothrow(std::wstring(path).c_str());
  if (!name) return E_OUTOFMEMORY;
  return service->GetFolder(name.get(), folder->put());
}

// The names of the tasks (|tasks|, hidden ones too) or of the subfolders in |folder|; what cannot
// be read is left out.
std::vector<std::wstring> NamesIn(ITaskFolder* folder, bool tasks) {
  std::vector<std::wstring> names;
  const auto add = [&names](const wil::unique_bstr& name) {
    if (name) names.emplace_back(name.get(), SysStringLen(name.get()));
  };
  LONG count = 0;
  if (tasks) {
    wil::com_ptr_nothrow<IRegisteredTaskCollection> all;
    if (FAILED(folder->GetTasks(TASK_ENUM_HIDDEN, all.put())) || FAILED(all->get_Count(&count))) {
      return names;
    }
    for (LONG index = 1; index <= count; ++index) {
      VARIANT item;
      VariantInit(&item);
      item.vt = VT_I4;
      item.lVal = index;
      wil::com_ptr_nothrow<IRegisteredTask> task;
      wil::unique_bstr name;
      if (SUCCEEDED(all->get_Item(item, task.put())) && SUCCEEDED(task->get_Name(name.put()))) {
        add(name);
      }
    }
    return names;
  }
  wil::com_ptr_nothrow<ITaskFolderCollection> all;
  if (FAILED(folder->GetFolders(0, all.put())) || FAILED(all->get_Count(&count))) return names;
  for (LONG index = 1; index <= count; ++index) {
    VARIANT item;
    VariantInit(&item);
    item.vt = VT_I4;
    item.lVal = index;
    wil::com_ptr_nothrow<ITaskFolder> subfolder;
    wil::unique_bstr name;
    if (SUCCEEDED(all->get_Item(item, subfolder.put())) &&
        SUCCEEDED(subfolder->get_Name(name.put()))) {
      add(name);
    }
  }
  return names;
}

// Deletes the task folder |path| with its tasks and subfolders, as far as this process may (best
// effort; a few levels deep at most).
void RemoveTaskFolderTree(ITaskService* service, const std::wstring& path, int depth) {
  wil::com_ptr_nothrow<ITaskFolder> folder;
  if (depth > 4 || FAILED(GetTaskFolder(service, path, std::addressof(folder)))) return;
  for (const std::wstring& name : NamesIn(folder.get(), true)) {
    if (const wil::unique_bstr text = wil::make_bstr_nothrow(name.c_str())) {
      folder->DeleteTask(text.get(), 0);
    }
  }
  for (const std::wstring& name : NamesIn(folder.get(), false)) {
    RemoveTaskFolderTree(service, path + L"\\" + name, depth + 1);
  }
  wil::com_ptr_nothrow<ITaskFolder> root;
  const wil::unique_bstr text = wil::make_bstr_nothrow(path.c_str());
  if (text && SUCCEEDED(GetTaskFolder(service, L"\\", std::addressof(root))))
    root->DeleteFolder(text.get(), 0);
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
  if (const TestAccount* standard = CurrentStandardAccount()) users.push_back(standard->Places());
  return users;
}

std::optional<bool> TaskFolderExists(std::wstring_view folder) {
  wil::com_ptr_nothrow<ITaskService> service;
  HRESULT hr = ConnectTaskService(std::addressof(service));
  wil::com_ptr_nothrow<ITaskFolder> found;
  if (SUCCEEDED(hr)) hr = GetTaskFolder(service.get(), folder, std::addressof(found));
  if (SUCCEEDED(hr)) return true;
  if (IsTaskNotFound(hr)) return false;
  return std::nullopt;
}

std::vector<std::string> RemoveTaskTestLeftovers() {
  std::vector<std::string> lines;
  wil::com_ptr_nothrow<ITaskService> service;
  wil::com_ptr_nothrow<ITaskFolder> root;
  HRESULT hr = ConnectTaskService(std::addressof(service));
  if (SUCCEEDED(hr)) hr = GetTaskFolder(service.get(), L"\\", std::addressof(root));
  if (FAILED(hr)) {
    lines.push_back("Task Scheduler's root folder cannot be read: " + Text(HResultText(hr)));
    return lines;
  }
  const std::wstring start = std::wstring(kProductName) + L"-Test";
  for (const std::wstring& name : NamesIn(root.get(), false)) {
    if (!IStartsWith(name, start)) continue;
    RemoveTaskFolderTree(service.get(), L"\\" + name, 0);
    wil::com_ptr_nothrow<ITaskFolder> still;
    const bool gone =
        IsTaskNotFound(GetTaskFolder(service.get(), L"\\" + name, std::addressof(still)));
    lines.push_back("task folder \\" + Text(name) + (gone ? ": removed" : ": left"));
  }
  for (const std::wstring& name : NamesIn(root.get(), true)) {
    if (!IStartsWith(name, start)) continue;
    const wil::unique_bstr text = wil::make_bstr_nothrow(name.c_str());
    const bool gone = text && SUCCEEDED(root->DeleteTask(text.get(), 0));
    lines.push_back("task \\" + Text(name) + (gone ? ": removed" : ": left"));
  }
  return lines;
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
  bool activeSetup = false;
  if (FAILED(RegistryKeyExists(HKEY_LOCAL_MACHINE, ActiveSetupKeyPath(), &activeSetup)) ||
      activeSetup) {
    found.push_back("the Active Setup component " + Text(ActiveSetupKeyPath()));
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
  for (const wchar_t* value : {kPendingRenamesValue, kPendingRenames2Value}) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSessionManagerKey, value, RRF_RT_REG_MULTI_SZ, nullptr,
                     nullptr, &bytes) != ERROR_SUCCESS) {
      continue;
    }
    std::vector<wchar_t> data(bytes / sizeof(wchar_t) + 2, L'\0');
    bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kSessionManagerKey, value, RRF_RT_REG_MULTI_SZ, nullptr,
                     data.data(), &bytes) != ERROR_SUCCESS) {
      continue;
    }
    // Pairs of strings: the source ("*1\??\C:\..." or "\??\C:\..."), then the target, which is
    // empty for a deletion. Empty strings are entries here, so the value is split by its length,
    // not at the first empty string.
    const std::wstring_view all(data.data(), bytes / sizeof(wchar_t));
    std::vector<std::wstring_view> entries;
    for (size_t start = 0; start < all.size();) {
      const size_t end = all.find(L'\0', start);
      entries.push_back(all.substr(start, end == std::wstring_view::npos ? end : end - start));
      if (end == std::wstring_view::npos) break;
      start = end + 1;
    }
    for (size_t i = 0; i + 1 < entries.size(); i += 2) {
      if (entries[i + 1].empty() &&
          IEquals(PendingRenamePath(entries[i]), std::wstring_view(target.native()))) {
        return true;
      }
    }
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
  report("deleting the Active Setup component", DeleteActiveSetup(HKEY_LOCAL_MACHINE));
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
