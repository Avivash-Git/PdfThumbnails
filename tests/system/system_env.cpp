#include "system_env.h"

#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <array>
#include <cstdio>
#include <cwchar>

#include "child_process.h"
#include "context.h"
#include "elevation.h"
#include "file_ops.h"
#include "inno.h"
#include "instance.h"
#include "medium_il.h"
#include "paths.h"
#include "project_config.h"
#include "registration.h"
#include "registry_roots.h"
#include "strings.h"
#include "tasks.h"

namespace pdft::systest {
namespace {

using std::filesystem::path;

constexpr wchar_t kSessionManagerKey[] = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
constexpr wchar_t kPendingRenamesValue[] = L"PendingFileRenameOperations";

bool Exists(const path& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Text(std::wstring_view text) { return WideToUtf8Lossy(text); }

// The install folders and the data folder, each only when it can be found.
std::vector<std::pair<std::string, path>> ProductFolders() {
  std::vector<std::pair<std::string, path>> folders;
  path folder;
  if (SUCCEEDED(GetMachineRoot(&folder))) folders.emplace_back("the folder for all users", folder);
  if (SUCCEEDED(GetUserRoot(&folder))) folders.emplace_back("the per-user folder", folder);
  if (SUCCEEDED(GetDataDir(&folder))) folders.emplace_back("the data folder", folder);
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

}  // namespace

std::wstring EnvironmentValue(const wchar_t* name) {
  wchar_t buffer[2048] = {};
  const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
  if (length == 0 || length >= ARRAYSIZE(buffer)) return {};
  return std::wstring(buffer, length);
}

std::optional<std::string> WhyNotHere() {
  if (EnvironmentValue(kEnableEnv) != L"1") {
    return Text(kEnableEnv) +
           " is not 1: these tests install and uninstall for real, so they run only on a "
           "throw-away PC that asks for them (the CI job system-x64)";
  }
  bool elevated = false;
  if (FAILED(GetTokenElevation(&elevated)) || !elevated) {
    return std::string(
        "not elevated: the tests check what the administrator steps did, and run "
        "the installer's administrator commands themselves");
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
  const RegistryRoots roots;
  for (const auto& [where, root] :
       {std::pair{"HKLM", HKEY_LOCAL_MACHINE}, std::pair{"HKCU", HKEY_CURRENT_USER}}) {
    bool exists = false;
    if (FAILED(RegistryKeyExists(root, ClassKeyPath(), &exists)) || exists) {
      found.push_back(std::string("the handler's class in ") + where);
    }
  }
  SlotValues user;
  SlotValues machine;
  if (SUCCEEDED(ReadSlots(roots, &user, &machine))) {
    for (size_t i = 0; i < kSlotCount; ++i) {
      if (user[i] && IsOurClsid(*user[i])) found.push_back("HKCU slot " + Text(SlotName(i)));
      if (machine[i] && IsOurClsid(*machine[i])) found.push_back("HKLM slot " + Text(SlotName(i)));
    }
  } else {
    found.push_back("the .pdf slots (cannot be read)");
  }
  bool settings = false;
  if (FAILED(RegistryKeyExists(HKEY_CURRENT_USER, kRegKey, &settings)) || settings) {
    found.push_back("HKCU\\" + Text(kRegKey));
  }
  std::vector<std::wstring> runOnce;
  if (FAILED(FindRunOnceRemovals(roots, &runOnce))) {
    found.push_back("RunOnce (cannot be read)");
  }
  for (const std::wstring& name : runOnce) found.push_back("RunOnce value " + Text(name));

  const std::optional<bool> folder = TaskFolderExists(kTaskFolder);
  if (!folder || *folder) found.push_back("the task folder " + Text(kTaskFolder));
  std::wstring sid;
  TaskLocation location;
  TaskState task;
  if (SUCCEEDED(GetCurrentUserSid(&sid)) && SUCCEEDED(DefaultTaskLocation(sid, &location)) &&
      SUCCEEDED(ReadTask(location, &task))) {
    if (task.exists) found.push_back("this user's check task " + Text(task.path));
  } else {
    found.push_back("this user's check task (cannot be read)");
  }
  bool legacyTask = false;
  if (FAILED(LegacyTaskExists(&legacyTask)) || legacyTask) {
    found.push_back("1.x's task " + Text(kLegacyTask));
  }

  for (const auto& [what, dir] : ProductFolders()) {
    if (Exists(dir)) found.push_back(what + " " + Text(dir.native()));
  }
  std::optional<InnoInstall> inno;
  if (FAILED(FindInnoInstall(roots, &inno)) || inno) {
    found.push_back("an installer's uninstall record " + Text(InnoUninstallKey()));
  }
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
  const RegistryRoots roots;
  const auto report = [](const char* what, HRESULT hr) {
    if (FAILED(hr))
      std::printf("[ system ] clean-up: %s failed: %s\n", what, Text(HResultText(hr)).c_str());
  };
  report("deleting the check tasks", DeleteTaskFolderAllUsers(kTaskFolder, kTaskRootPrefix));
  report("deleting 1.x's task", DeleteLegacyTask());
  report("removing the slots", RemoveSlotsIfOurs(roots));
  report("deleting the HKLM class", DeleteRegistryTree(HKEY_LOCAL_MACHINE, ClassKeyPath()));
  report("deleting the HKCU class", DeleteRegistryTree(HKEY_CURRENT_USER, ClassKeyPath()));
  report("deleting the settings", DeleteRegistryTree(HKEY_CURRENT_USER, kRegKey));
  report("deleting the RunOnce values", DeleteRunOnceRemovals(roots));
  for (const auto& [what, dir] : ProductFolders()) {
    if (Exists(dir)) report(what.c_str(), RemoveTreeNoFollow(dir));
  }
  for (const path& trash : TrashFolders())
    report("removing a trash folder", RemoveTreeNoFollow(trash));
}

Outcome RunAsUser(const path& exe, const std::wstring& arguments) {
  wil::unique_handle token;
  if (const HRESULT hr = test::CreateMediumTokenForSelf(test::MediumToken::kFiltered, token.put());
      FAILED(hr)) {
    Outcome run;
    run.hr = hr;
    return run;
  }
  return ToOutcome(test::RunCaptured(exe.native(), arguments, kCommandTimeoutMs, token.get()));
}

Outcome RunAsAdministrator(const path& exe, const std::wstring& arguments) {
  return ToOutcome(test::RunCaptured(exe.native(), arguments, kCommandTimeoutMs));
}

}  // namespace pdft::systest
