// The check task's writing side (tasks.h) against the real Task Scheduler. Every test works in a
// task folder and under root-folder names of its own:
//   \<Product>-Test-<pid>-<GUID>                         its folder (only some tests make it)
//   \<Product>-Test check <pid>-<GUID> <user SID>        its fallback name in the root folder
//   \<Product>-Test check <pid>-<GUID> ...               other root tasks a test plants
// and removes them in TearDown. Before the first test, leftovers of earlier runs that crashed are
// swept: only names that start like these, whose test process (<pid>) is gone, and that this user
// owns (or, elevated, Administrators). The real check task, its folder, 1.x's task and the
// developer tools' task folder are never touched. Task Scheduler is not virtualized, so this runs
// the same inside an app container.
//
// The tasks are registered for the signed-in user with their interactive token. Elevated (CI),
// the tests pass ApplyTaskOptions an unelevated copy of the context, which it would refuse
// otherwise; the shared folder's own tests (EnsureTaskFolder, DeleteTaskFolderAllUsers) need the
// elevation and skip without it. Some of those start this test program again as a standard user
// (a filtered Medium token) to make a folder or register a task as one would, and some register
// tasks without triggers that run as SYSTEM, which nothing starts, as another account's tasks
// under the user's names. One test makes Task Scheduler start a program (pdft-run-medium.exe
// --exit 0); it skips when Application Control blocks that program or when Task Scheduler has no
// session of this account to run it in (no interactive desktop, an elevated runner, or a prompt
// started with "runas"). The check's repair of a task that runs another program is tried here
// too, through the same place; its program is an empty file in a folder under %TEMP% that
// nothing starts.

#include <windows.h>

#include "tasks.h"

#include <gtest/gtest.h>
#include <objbase.h>
#include <sddl.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "child_process.h"
#include "context.h"
#include "medium_il.h"
#include "project_config.h"
#include "registry_sandbox.h"
#include "repair.h"
#include "snapshot.h"
#include "strings.h"
#include "system_probes.h"
#include "temp_dir.h"

namespace pdft {
namespace {

// The start of the test folders' names, and of the test tasks' names in the root folder.
std::wstring TestFolderStart() { return std::wstring(kProductName) + L"-Test-"; }
std::wstring TestRootTaskStart() { return std::wstring(kProductName) + L"-Test check "; }

std::wstring Bstr(const wil::unique_bstr& bstr) {
  return bstr ? std::wstring(bstr.get(), SysStringLen(bstr.get())) : std::wstring();
}

wil::unique_bstr MakeBstr(std::wstring_view text) {
  return wil::make_bstr_nothrow(std::wstring(text).c_str());
}

// A GUID string without braces.
std::wstring NewId() {
  GUID id = {};
  EXPECT_HRESULT_SUCCEEDED(CoCreateGuid(&id));
  wchar_t text[40] = {};
  EXPECT_GT(StringFromGUID2(id, text, ARRAYSIZE(text)), 0);
  const std::wstring result = text;
  return result.substr(1, result.size() - 2);
}

std::string Narrow(std::wstring_view text) { return WideToUtf8Lossy(text); }

std::string Hex(HRESULT hr) {
  char text[16];
  sprintf_s(text, "0x%08lX", static_cast<unsigned long>(hr));
  return text;
}

wil::com_ptr_nothrow<ITaskService> Connect() {
  wil::com_ptr_nothrow<ITaskService> service;
  if (FAILED(CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(service.put())))) {
    return nullptr;
  }
  VARIANT empty;
  VariantInit(&empty);
  if (FAILED(service->Connect(empty, empty, empty, empty))) return nullptr;
  return service;
}

wil::com_ptr_nothrow<ITaskFolder> GetFolder(ITaskService* service, std::wstring_view path) {
  wil::com_ptr_nothrow<ITaskFolder> folder;
  const wil::unique_bstr text = MakeBstr(path);
  if (!text || FAILED(service->GetFolder(text.get(), folder.put()))) return nullptr;
  return folder;
}

wil::com_ptr_nothrow<IRegisteredTask> GetTask(ITaskService* service, std::wstring_view folderPath,
                                              std::wstring_view name) {
  const wil::com_ptr_nothrow<ITaskFolder> folder = GetFolder(service, folderPath);
  wil::com_ptr_nothrow<IRegisteredTask> task;
  const wil::unique_bstr text = MakeBstr(name);
  if (!folder || !text || FAILED(folder->GetTask(text.get(), task.put()))) return nullptr;
  return task;
}

std::vector<std::wstring> TaskNamesIn(ITaskFolder* folder) {
  std::vector<std::wstring> names;
  wil::com_ptr_nothrow<IRegisteredTaskCollection> tasks;
  LONG count = 0;
  if (FAILED(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.put())) || FAILED(tasks->get_Count(&count))) {
    return names;
  }
  for (LONG index = 1; index <= count; ++index) {
    VARIANT item;
    VariantInit(&item);
    item.vt = VT_I4;
    item.lVal = index;
    wil::com_ptr_nothrow<IRegisteredTask> task;
    wil::unique_bstr name;
    if (SUCCEEDED(tasks->get_Item(item, task.put())) && SUCCEEDED(task->get_Name(name.put()))) {
      names.push_back(Bstr(name));
    }
  }
  return names;
}

std::vector<std::wstring> FolderNamesIn(ITaskFolder* folder) {
  std::vector<std::wstring> names;
  wil::com_ptr_nothrow<ITaskFolderCollection> folders;
  LONG count = 0;
  if (FAILED(folder->GetFolders(0, folders.put())) || FAILED(folders->get_Count(&count))) {
    return names;
  }
  for (LONG index = 1; index <= count; ++index) {
    VARIANT item;
    VariantInit(&item);
    item.vt = VT_I4;
    item.lVal = index;
    wil::com_ptr_nothrow<ITaskFolder> subfolder;
    wil::unique_bstr name;
    if (SUCCEEDED(folders->get_Item(item, subfolder.put())) &&
        SUCCEEDED(subfolder->get_Name(name.put()))) {
      names.push_back(Bstr(name));
    }
  }
  return names;
}

void DeleteNamedTask(ITaskFolder* folder, std::wstring_view name) {
  const wil::unique_bstr text = MakeBstr(name);
  if (text) folder->DeleteTask(text.get(), 0);
}

// Deletes a test folder with everything in it, as far as this process may. Best effort.
void RemoveFolderTree(ITaskService* service, const std::wstring& path, int depth = 0) {
  const wil::com_ptr_nothrow<ITaskFolder> folder = GetFolder(service, path);
  if (!folder || depth > 8) return;
  for (const std::wstring& name : TaskNamesIn(folder.get())) DeleteNamedTask(folder.get(), name);
  for (const std::wstring& name : FolderNamesIn(folder.get())) {
    RemoveFolderTree(service, path + L"\\" + name, depth + 1);
  }
  const wil::com_ptr_nothrow<ITaskFolder> root = GetFolder(service, L"\\");
  const wil::unique_bstr text = MakeBstr(path);
  if (root && text) root->DeleteFolder(text.get(), 0);
}

// The owner in |sddl| is |sid| (a SID string or an alias such as "BA").
bool OwnerIs(const std::wstring& sddl, const wchar_t* sid) {
  wil::unique_hlocal_security_descriptor descriptor;
  wil::unique_hlocal sidStorage;
  PSID wanted = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            descriptor.put(), nullptr) ||
      !ConvertStringSidToSidW(sid, &wanted)) {
    return false;
  }
  sidStorage.reset(wanted);
  PSID owner = nullptr;
  BOOL defaulted = FALSE;
  return GetSecurityDescriptorOwner(descriptor.get(), &owner, &defaulted) && owner &&
         EqualSid(owner, wanted);
}

// The process id at the start of |rest| ("<pid>-..."), or 0.
DWORD LeadingProcessId(std::wstring_view rest) {
  DWORD pid = 0;
  size_t at = 0;
  while (at < rest.size() && at < 10 && std::iswdigit(rest[at])) {
    pid = pid * 10 + static_cast<DWORD>(rest[at] - L'0');
    ++at;
  }
  return at > 0 && at < rest.size() && rest[at] == L'-' ? pid : 0;
}

// True only when no process |pid| exists (a process of another user still counts as there).
bool ProcessIsGone(DWORD pid) {
  if (pid == 0) return false;
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  if (process) {
    DWORD code = 0;
    return GetExitCodeProcess(process.get(), &code) && code != STILL_ACTIVE;
  }
  return GetLastError() == ERROR_INVALID_PARAMETER;
}

// Removes the test folders and root tasks that earlier runs of these tests left behind.
void SweepLeftovers(const ProcessContext& context) {
  const wil::com_ptr_nothrow<ITaskService> service = Connect();
  if (!service) return;
  const wil::com_ptr_nothrow<ITaskFolder> root = GetFolder(service.get(), L"\\");
  if (!root) return;
  const auto ours = [&](const std::wstring& sddl) {
    return OwnerIs(sddl, context.userSid.c_str()) || (context.elevated && OwnerIs(sddl, L"BA"));
  };
  constexpr LONG kOwner = OWNER_SECURITY_INFORMATION;
  int swept = 0;
  for (const std::wstring& name : FolderNamesIn(root.get())) {
    if (!IStartsWith(name, TestFolderStart()) ||
        !ProcessIsGone(
            LeadingProcessId(std::wstring_view(name).substr(TestFolderStart().size())))) {
      continue;
    }
    const wil::com_ptr_nothrow<ITaskFolder> folder = GetFolder(service.get(), L"\\" + name);
    wil::unique_bstr sddl;
    if (!folder || FAILED(folder->GetSecurityDescriptor(kOwner, sddl.put())) || !ours(Bstr(sddl))) {
      continue;
    }
    RemoveFolderTree(service.get(), L"\\" + name);
    if (!GetFolder(service.get(), L"\\" + name)) ++swept;
  }
  for (const std::wstring& name : TaskNamesIn(root.get())) {
    if (!IStartsWith(name, TestRootTaskStart()) ||
        !ProcessIsGone(
            LeadingProcessId(std::wstring_view(name).substr(TestRootTaskStart().size())))) {
      continue;
    }
    const wil::com_ptr_nothrow<IRegisteredTask> task = GetTask(service.get(), L"\\", name);
    wil::unique_bstr sddl;
    if (!task || FAILED(task->GetSecurityDescriptor(kOwner, sddl.put())) || !ours(Bstr(sddl))) {
      continue;
    }
    DeleteNamedTask(root.get(), name);
    ++swept;
  }
  if (swept > 0) std::printf("[ tasks ] swept %d leftovers of earlier runs\n", swept);
}

// A task without triggers for |sid| that runs |program|, registered as |name| in |folderPath|:
// a copy for ApplyTaskOptions to find, or a task that is not the check task. With
// TASK_LOGON_SERVICE_ACCOUNT, |sid| is a service account (only an administrator may do that).
HRESULT RegisterPlain(ITaskService* service, std::wstring_view folderPath, std::wstring_view name,
                      const std::wstring& program, const std::wstring& sid,
                      TASK_LOGON_TYPE logon = TASK_LOGON_INTERACTIVE_TOKEN) {
  const wil::com_ptr_nothrow<ITaskFolder> folder = GetFolder(service, folderPath);
  if (!folder) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
  wil::com_ptr_nothrow<ITaskDefinition> definition;
  RETURN_IF_FAILED(service->NewTask(0, definition.put()));
  wil::com_ptr_nothrow<IPrincipal> principal;
  RETURN_IF_FAILED(definition->get_Principal(principal.put()));
  RETURN_IF_FAILED(principal->put_UserId(MakeBstr(sid).get()));
  RETURN_IF_FAILED(principal->put_LogonType(logon));
  wil::com_ptr_nothrow<IActionCollection> actions;
  RETURN_IF_FAILED(definition->get_Actions(actions.put()));
  wil::com_ptr_nothrow<IAction> action;
  RETURN_IF_FAILED(actions->Create(TASK_ACTION_EXEC, action.put()));
  wil::com_ptr_nothrow<IExecAction> exec;
  RETURN_IF_FAILED(action->QueryInterface(IID_PPV_ARGS(exec.put())));
  RETURN_IF_FAILED(exec->put_Path(MakeBstr(program).get()));
  VARIANT empty;
  VariantInit(&empty);
  // A service account is named in the call too; the user's own task takes the principal's.
  wil::unique_variant user;
  if (logon == TASK_LOGON_SERVICE_ACCOUNT) {
    user.vt = VT_BSTR;
    user.bstrVal = SysAllocString(sid.c_str());
    if (!user.bstrVal) return E_OUTOFMEMORY;
  }
  wil::com_ptr_nothrow<IRegisteredTask> registered;
  return folder->RegisterTaskDefinition(MakeBstr(name).get(), definition.get(),
                                        TASK_CREATE_OR_UPDATE, user, empty, logon, empty,
                                        registered.put());
}

// The owner, group and DACL of |task| as SDDL; empty when they cannot be read.
std::wstring TaskSddl(IRegisteredTask* task) {
  wil::unique_bstr sddl;
  if (!task ||
      FAILED(task->GetSecurityDescriptor(
          OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
          sddl.put()))) {
    return {};
  }
  return Bstr(sddl);
}

// Whether the desktop's shell (Explorer) runs as this process's user. Under "runas" it is the
// signed-in user's, not this account's, and Task Scheduler has no session of this account to
// start an interactive-token task in.
bool ShellIsThisUsers() {
  DWORD pid = 0;
  const HWND shell = GetShellWindow();
  if (!shell || GetWindowThreadProcessId(shell, &pid) == 0 || pid == 0) return false;
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  wil::unique_handle shellToken;
  wil::unique_handle ownToken;
  if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, shellToken.put()) ||
      !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, ownToken.put())) {
    return false;  // another account's shell
  }
  const auto user = [](HANDLE token) {
    std::vector<BYTE> buffer(256);
    DWORD size = 0;
    if (!GetTokenInformation(token, TokenUser, buffer.data(), static_cast<DWORD>(buffer.size()),
                             &size)) {
      buffer.clear();
    }
    return buffer;
  };
  std::vector<BYTE> shellUser = user(shellToken.get());
  std::vector<BYTE> ownUser = user(ownToken.get());
  return !shellUser.empty() && !ownUser.empty() &&
         EqualSid(reinterpret_cast<TOKEN_USER*>(shellUser.data())->User.Sid,
                  reinterpret_cast<TOKEN_USER*>(ownUser.data())->User.Sid);
}

// The text between the start of the first element that begins with |open| and its |close|
// tag, both included; empty when there is none.
std::wstring Element(const std::wstring& xml, std::wstring_view open, std::wstring_view close) {
  const size_t begin = xml.find(open);
  if (begin == std::wstring::npos) return {};
  const size_t end = xml.find(close, begin);
  if (end == std::wstring::npos) return {};
  return xml.substr(begin, end + close.size() - begin);
}

std::wstring Contents(const std::wstring& xml, std::wstring_view tag) {
  const std::wstring open = L"<" + std::wstring(tag) + L">";
  const std::wstring close = L"</" + std::wstring(tag) + L">";
  const size_t begin = xml.find(open);
  if (begin == std::wstring::npos) return {};
  const size_t end = xml.find(close, begin);
  if (end == std::wstring::npos) return {};
  return xml.substr(begin + open.size(), end - begin - open.size());
}

bool Has(const std::wstring& text, std::wstring_view part) {
  return text.find(part) != std::wstring::npos;
}

// How SDDL writes the account |sid|: as its alias when it has one ("LA" for the built-in
// Administrator, the account CI's runners use), else as the SID string.
std::wstring SddlSid(const std::wstring& sid) {
  PSID binary = nullptr;
  if (!ConvertStringSidToSidW(sid.c_str(), &binary)) return sid;
  const wil::unique_hlocal owned(binary);
  SECURITY_DESCRIPTOR descriptor;
  wil::unique_hlocal_string text;
  if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorOwner(&descriptor, binary, FALSE) ||
      !ConvertSecurityDescriptorToStringSecurityDescriptorW(
          &descriptor, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION, text.put(), nullptr)) {
    return sid;
  }
  const std::wstring_view owner(text.get());
  return owner.starts_with(L"O:") ? std::wstring(owner.substr(2)) : sid;
}

bool NeedsXmlEscape(std::wstring_view text) {
  return text.find_first_of(L"&<>'\"") != std::wstring_view::npos;
}

// |task|'s definition as Task Scheduler holds it; empty when there is no task or it cannot be
// read. The same text before and after means nobody registered over the task.
std::wstring TaskXml(IRegisteredTask* task) {
  wil::unique_bstr xml;
  if (!task || FAILED(task->get_Xml(xml.put()))) return {};
  return Bstr(xml);
}

// The account the system runs services as: a task that runs as it is another account's, and
// only an administrator can register one.
constexpr wchar_t kSystemSid[] = L"S-1-5-18";

class TaskWriting : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    ProcessContext context;
    if (SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context))) SweepLeftovers(context);
  }

  void SetUp() override {
    ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context_));
    ASSERT_TRUE(IsSidString(context_.userSid));
    // As the signed-in user, unelevated: CI runs elevated, where ApplyTaskOptions refuses.
    user_ = context_;
    user_.elevated = false;
    service_ = Connect();
    ASSERT_TRUE(service_);
    id_ = std::to_wstring(GetCurrentProcessId()) + L"-" + NewId();
    location_.folder = L"\\" + TestFolderStart() + id_;
    location_.name = std::wstring(kTaskPrefix) + context_.userSid;
    location_.fallbackFolder = L"\\";
    rootPrefix_ = TestRootTaskStart() + id_ + L" ";
    location_.fallbackName = rootPrefix_ + context_.userSid;
    location_.userSid = context_.userSid;
    // A program that exists; the tasks only run it in the test that says so.
    program_ = test::SelfFolder() + L"\\pdft-run-medium.exe";
    ASSERT_NE(GetFileAttributesW(program_.c_str()), INVALID_FILE_ATTRIBUTES) << Narrow(program_);
  }

  void TearDown() override {
    if (!service_) return;
    RemoveFolderTree(service_.get(), location_.folder);
    if (const wil::com_ptr_nothrow<ITaskFolder> root = GetFolder(service_.get(), L"\\")) {
      for (const std::wstring& name : TaskNamesIn(root.get())) {
        if (IStartsWith(name, rootPrefix_)) DeleteNamedTask(root.get(), name);
      }
    }
    EXPECT_FALSE(GetFolder(service_.get(), location_.folder)) << "the test folder is left";
    EXPECT_FALSE(GetTask(service_.get(), L"\\", location_.fallbackName)) << "a test task is left";
  }

  bool FolderExists() const { return GetFolder(service_.get(), location_.folder) != nullptr; }
  wil::com_ptr_nothrow<IRegisteredTask> InFolder() const {
    return GetTask(service_.get(), location_.folder, location_.name);
  }
  wil::com_ptr_nothrow<IRegisteredTask> AtFallback() const {
    return GetTask(service_.get(), L"\\", location_.fallbackName);
  }

  // A folder made by this process like any user would make it: the root folder's descriptor
  // inherited, owned by this process's default owner (this user unelevated, Administrators
  // elevated). Never trusted: the inherited entries give every user more than adding tasks.
  void MakeUntrustedFolder() {
    const wil::com_ptr_nothrow<ITaskFolder> root = GetFolder(service_.get(), L"\\");
    ASSERT_TRUE(root);
    VARIANT inherit;
    VariantInit(&inherit);
    wil::com_ptr_nothrow<ITaskFolder> made;
    ASSERT_HRESULT_SUCCEEDED(
        root->CreateFolder(MakeBstr(location_.folder).get(), inherit, made.put()));
    wil::unique_bstr sddl;
    ASSERT_HRESULT_SUCCEEDED(made->GetSecurityDescriptor(
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        sddl.put()));
    EXPECT_FALSE(IsTrustedTaskFolderSddl(Bstr(sddl)));
  }

  // The registered task (at the folder or the fallback name) matches |options| and the check
  // task's fixed parts, in its XML and through Task Scheduler's interfaces.
  void ExpectRegistered(const TaskOptions& options, bool inFolder) {
    TaskState state;
    ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
    ASSERT_TRUE(state.exists);
    EXPECT_EQ(state.atFallback, !inFolder);
    EXPECT_EQ(state.enabled, options.enabled);
    EXPECT_EQ(state.options.afterPdfCraft, options.afterPdfCraft);
    EXPECT_EQ(state.options.atSignIn, options.atSignIn);
    EXPECT_EQ(state.options.enabled, options.enabled);
    EXPECT_EQ(state.afterPdfCraft, options.afterPdfCraft == TriggerSetting::kEnabled);
    EXPECT_EQ(state.atSignIn, options.atSignIn == TriggerSetting::kEnabled);
    EXPECT_EQ(state.actionPath, program_);
    EXPECT_EQ(state.actionArgs, kCheckTaskArguments);
    EXPECT_TRUE(IEquals(state.path, inFolder ? location_.folder + L"\\" + location_.name
                                             : L"\\" + location_.fallbackName))
        << Narrow(state.path);
    // One copy only.
    EXPECT_EQ(InFolder() != nullptr, inFolder);
    EXPECT_EQ(AtFallback() != nullptr, !inFolder);

    const wil::com_ptr_nothrow<IRegisteredTask> task = inFolder ? InFolder() : AtFallback();
    ASSERT_TRUE(task);
    wil::unique_bstr xmlText;
    ASSERT_HRESULT_SUCCEEDED(task->get_Xml(xmlText.put()));
    const std::wstring xml = Bstr(xmlText);
    SCOPED_TRACE(Narrow(xml));

    // Registration info.
    EXPECT_EQ(Contents(xml, L"Author"), kProductName);
    EXPECT_EQ(Contents(xml, L"Description"), TaskDescription());
    EXPECT_EQ(Contents(xml, L"Version"),
              std::to_wstring(kVersion4[0]) + L"." + std::to_wstring(kVersion4[1]));

    // The logon trigger: its ID, its delay, and the user it fires for.
    const std::wstring logon = Element(xml, L"<LogonTrigger", L"</LogonTrigger>");
    EXPECT_EQ(logon.empty(), options.atSignIn == TriggerSetting::kAbsent) << Narrow(logon);
    if (!logon.empty()) {
      EXPECT_TRUE(Has(logon, L"<LogonTrigger id=\"AtSignIn\">")) << Narrow(logon);
      EXPECT_EQ(Contents(logon, L"Delay"), L"PT1M");
      // Task Scheduler stores the account; a SID-only context (the account could not be looked
      // up) is stored as whatever account the SID is.
      if (!IsSidString(user_.userAccount)) {
        EXPECT_TRUE(IEquals(Contents(logon, L"UserId"), user_.userAccount))
            << Narrow(Contents(logon, L"UserId"));
      }
      EXPECT_EQ(Has(logon, L"<Enabled>false</Enabled>"),
                options.atSignIn == TriggerSetting::kDisabled);
    }
    // The event trigger: its ID and delay.
    const std::wstring event = Element(xml, L"<EventTrigger", L"</EventTrigger>");
    EXPECT_EQ(event.empty(), options.afterPdfCraft == TriggerSetting::kAbsent) << Narrow(event);
    if (!event.empty()) {
      EXPECT_TRUE(Has(event, L"<EventTrigger id=\"AfterPdfCraftInstall\">")) << Narrow(event);
      EXPECT_EQ(Contents(event, L"Delay"), L"PT30S");
      EXPECT_FALSE(Contents(event, L"Subscription").empty());
      EXPECT_EQ(Has(event, L"<Enabled>false</Enabled>"),
                options.afterPdfCraft == TriggerSetting::kDisabled);
    }

    // The principal: the user's SID, their interactive token, not elevated (the default run
    // level, which the XML leaves out).
    const std::wstring principal = Element(xml, L"<Principal id=\"Author\">", L"</Principal>");
    ASSERT_FALSE(principal.empty());
    EXPECT_EQ(Contents(principal, L"UserId"), context_.userSid);
    EXPECT_EQ(Contents(principal, L"LogonType"), L"InteractiveToken");
    EXPECT_FALSE(Has(principal, L"HighestAvailable"));

    // The settings.
    const std::wstring settings = Element(xml, L"<Settings>", L"</Settings>");
    ASSERT_FALSE(settings.empty());
    EXPECT_EQ(Contents(settings, L"DisallowStartIfOnBatteries"), L"false");
    EXPECT_EQ(Contents(settings, L"StopIfGoingOnBatteries"), L"false");
    EXPECT_EQ(Contents(settings, L"StartWhenAvailable"), L"true");
    EXPECT_EQ(Contents(settings, L"ExecutionTimeLimit"), L"PT5M");
    const std::wstring instances = Contents(settings, L"MultipleInstancesPolicy");
    EXPECT_TRUE(instances.empty() || instances == L"IgnoreNew") << Narrow(instances);  // default
    const std::wstring enabled = Contents(settings, L"Enabled");  // left out when true
    EXPECT_TRUE(options.enabled ? enabled.empty() || enabled == L"true" : enabled == L"false")
        << Narrow(enabled);

    // The action.
    const std::wstring exec = Element(xml, L"<Exec>", L"</Exec>");
    if (!NeedsXmlEscape(program_)) EXPECT_EQ(Contents(exec, L"Command"), program_);
    EXPECT_EQ(Contents(exec, L"Arguments"), kCheckTaskArguments);

    // The same through the interfaces, with the subscription character for character.
    wil::com_ptr_nothrow<ITaskDefinition> definition;
    ASSERT_HRESULT_SUCCEEDED(task->get_Definition(definition.put()));
    wil::com_ptr_nothrow<IPrincipal> owner;
    ASSERT_HRESULT_SUCCEEDED(definition->get_Principal(owner.put()));
    TASK_RUNLEVEL_TYPE runLevel = TASK_RUNLEVEL_HIGHEST;
    TASK_LOGON_TYPE logonType = TASK_LOGON_NONE;
    EXPECT_HRESULT_SUCCEEDED(owner->get_RunLevel(&runLevel));
    EXPECT_HRESULT_SUCCEEDED(owner->get_LogonType(&logonType));
    EXPECT_EQ(runLevel, TASK_RUNLEVEL_LUA);
    EXPECT_EQ(logonType, TASK_LOGON_INTERACTIVE_TOKEN);
    wil::com_ptr_nothrow<ITaskSettings> taskSettings;
    ASSERT_HRESULT_SUCCEEDED(definition->get_Settings(taskSettings.put()));
    TASK_COMPATIBILITY compatibility = TASK_COMPATIBILITY_AT;
    TASK_INSTANCES_POLICY policy = TASK_INSTANCES_PARALLEL;
    EXPECT_HRESULT_SUCCEEDED(taskSettings->get_Compatibility(&compatibility));
    EXPECT_HRESULT_SUCCEEDED(taskSettings->get_MultipleInstances(&policy));
    EXPECT_EQ(compatibility, TASK_COMPATIBILITY_V2_4);
    EXPECT_EQ(policy, TASK_INSTANCES_IGNORE_NEW);
    wil::com_ptr_nothrow<ITriggerCollection> triggers;
    ASSERT_HRESULT_SUCCEEDED(definition->get_Triggers(triggers.put()));
    LONG count = 0;
    ASSERT_HRESULT_SUCCEEDED(triggers->get_Count(&count));
    EXPECT_EQ(count, (options.atSignIn != TriggerSetting::kAbsent ? 1 : 0) +
                         (options.afterPdfCraft != TriggerSetting::kAbsent ? 1 : 0));
    for (LONG index = 1; index <= count; ++index) {
      wil::com_ptr_nothrow<ITrigger> trigger;
      ASSERT_HRESULT_SUCCEEDED(triggers->get_Item(index, trigger.put()));
      if (const auto eventTrigger = trigger.try_query<IEventTrigger>()) {
        wil::unique_bstr subscription;
        ASSERT_HRESULT_SUCCEEDED(eventTrigger->get_Subscription(subscription.put()));
        EXPECT_EQ(Bstr(subscription), kPdfCraftSubscription);
      }
    }
  }

  // Each checkbox combination in turn (each one updates the task the one before left), then
  // both off, which deletes it.
  void CheckEveryCombination(bool inFolder) {
    for (const auto& [afterPdfCraft, atSignIn] : {std::pair{true, true}, std::pair{true, false},
                                                  std::pair{false, true}, std::pair{true, true}}) {
      SCOPED_TRACE(testing::Message()
                   << "afterPdfCraft " << afterPdfCraft << ", atSignIn " << atSignIn);
      const TaskOptions options = OptionsFromCheckboxes(afterPdfCraft, atSignIn);
      ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, options, program_, user_));
      ExpectRegistered(options, inFolder);
    }
    ASSERT_EQ(ApplyTaskOptions(location_, OptionsFromCheckboxes(false, false), program_, user_),
              S_OK);
    TaskState state;
    ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
    EXPECT_FALSE(state.exists);
    EXPECT_FALSE(InFolder());
    EXPECT_FALSE(AtFallback());
    EXPECT_EQ(DeleteTask(location_), S_FALSE);
  }

  ProcessContext context_;
  ProcessContext user_;
  wil::com_ptr_nothrow<ITaskService> service_;
  std::wstring id_;
  TaskLocation location_;
  std::wstring rootPrefix_;
  std::wstring program_;
};

// ---- As the signed-in user ----------------------------------------------------------------

// Without the folder (unelevated code never makes it), the task goes under the fallback name.
TEST_F(TaskWriting, EachCheckboxCombinationWithoutTheFolder) {
  ASSERT_FALSE(FolderExists());
  CheckEveryCombination(false);
  EXPECT_FALSE(FolderExists()) << "ApplyTaskOptions made the folder";
}

TEST_F(TaskWriting, DisabledTriggersAndTaskStayAsTheyAreWhenRegisteredAgain) {
  // As someone left it in Task Scheduler: sign-in on, PdfCraft's trigger and the task disabled;
  // and running an old program.
  const TaskOptions left{TriggerSetting::kDisabled, TriggerSetting::kEnabled, false};
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, left, test::SelfPath(), user_));
  TaskState state;
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  ASSERT_TRUE(state.exists);
  EXPECT_FALSE(state.enabled);
  EXPECT_FALSE(state.afterPdfCraft);
  EXPECT_TRUE(state.atSignIn);
  EXPECT_EQ(state.actionPath, test::SelfPath());

  // Registered again with the same triggers and the right program: only the program changes.
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, state.options, program_, user_));
  ExpectRegistered(left, false);

  // Both triggers disabled is still a task.
  const TaskOptions off{TriggerSetting::kDisabled, TriggerSetting::kDisabled, true};
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, off, program_, user_));
  ExpectRegistered(off, false);
}

// The check's repair of a stale task, through the real Task Scheduler: registered again for the
// installed program with the triggers and the enabled state it has, then left alone.
TEST_F(TaskWriting, TheCheckRegistersAStaleTaskAgain) {
  const test::TempDir root(L"task-root");
  ASSERT_TRUE(root.ok());
  const std::filesystem::path exe = std::filesystem::path(root.path()) / kSettingsExe;
  std::ofstream(exe) << "";  // never started
  const TaskOptions left{TriggerSetting::kDisabled, TriggerSetting::kEnabled, false};
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, left, program_, user_));

  const test::ScopedRegistrySandbox sandbox;
  ASSERT_TRUE(sandbox.ok()) << Hex(sandbox.error());
  const RegistryRoots roots{sandbox.user(), sandbox.machine()};
  Snapshot s;
  s.context = user_;
  s.scope = Scope::kUser;
  s.paths.userRoot = root.path();
  s.installRoot = root.path();
  s.settingsExeFound = true;
  s.taskLocation = location_;
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &s.task));
  ASSERT_TRUE(InstalledSettingsExe(s));
  RepairPlan plan;
  plan.user.push_back(RepairStep{RepairStep::Kind::kReRegisterTask});
  plan.user[0].path = *InstalledSettingsExe(s);
  // Only the task step is planned, but a private key stands in for the registry all the same.
  std::vector<RepairOutcome> outcomes;
  ASSERT_HRESULT_SUCCEEDED(ApplyUserRepairs(roots, RealSystemProbes(), s, plan, &outcomes));
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes[0].result, RepairResult::kDone) << Hex(outcomes[0].hr);

  TaskState state;
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  ASSERT_TRUE(state.exists);
  EXPECT_TRUE(state.atFallback);
  EXPECT_TRUE(IEquals(state.actionPath, exe.native())) << Narrow(state.actionPath);
  EXPECT_EQ(state.actionArgs, kCheckTaskArguments);
  EXPECT_EQ(state.options.afterPdfCraft, left.afterPdfCraft);
  EXPECT_EQ(state.options.atSignIn, left.atSignIn);
  EXPECT_EQ(state.options.enabled, left.enabled);
  EXPECT_FALSE(InFolder());

  // Sound now: the same step changes nothing.
  ASSERT_HRESULT_SUCCEEDED(ApplyUserRepairs(roots, RealSystemProbes(), s, plan, &outcomes));
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes[0].result, RepairResult::kUnchanged) << Hex(outcomes[0].hr);
}

TEST_F(TaskWriting, RefusesWithoutRegistering) {
  const auto nothing = [&] {
    EXPECT_FALSE(InFolder());
    EXPECT_FALSE(AtFallback());
  };
  const TaskOptions both = OptionsFromCheckboxes(true, true);
  ProcessContext elevated = user_;
  elevated.elevated = true;
  EXPECT_EQ(ApplyTaskOptions(location_, both, program_, elevated), E_ACCESSDENIED);
  EXPECT_EQ(ApplyTaskOptions(location_, OptionsFromCheckboxes(false, false), program_, elevated),
            E_ACCESSDENIED);
  ProcessContext noSid = user_;
  noSid.userSid = L"someone";
  EXPECT_EQ(ApplyTaskOptions(location_, both, program_, noSid), E_INVALIDARG);
  nothing();
  const std::wstring badPrograms[] = {
      L"pdft-run-medium.exe",   L"bin\\pdft-run-medium.exe", L"%SystemRoot%\\System32\\cmd.exe",
      L"\"" + program_ + L"\"", L"\\\\?\\" + program_,       std::wstring()};
  for (const std::wstring& bad : badPrograms) {
    EXPECT_EQ(ApplyTaskOptions(location_, both, bad, user_), E_INVALIDARG) << Narrow(bad);
  }
  const std::wstring missingPrograms[] = {test::SelfFolder() + L"\\pdft-no-such-program.exe",
                                          test::SelfFolder()};
  for (const std::wstring& missing : missingPrograms) {
    EXPECT_EQ(ApplyTaskOptions(location_, both, missing, user_),
              HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        << Narrow(missing);
  }
  nothing();
  // Nowhere to go: no folder and no fallback name.
  TaskLocation folderOnly = location_;
  folderOnly.fallbackName.clear();
  EXPECT_EQ(ApplyTaskOptions(folderOnly, both, program_, user_), E_ACCESSDENIED);
  nothing();
}

// A folder that a user made is not trusted: the task goes under the fallback name, and a copy
// that was in the folder goes away, so the user still has one task.
TEST_F(TaskWriting, UntrustedFolderMeansTheFallbackNameAndOneCopy) {
  MakeUntrustedFolder();
  ASSERT_HRESULT_SUCCEEDED(
      RegisterPlain(service_.get(), location_.folder, location_.name, program_, user_.userSid));
  ASSERT_TRUE(InFolder());
  TaskState state;
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  EXPECT_TRUE(state.exists);
  EXPECT_FALSE(state.atFallback);  // the copy in the folder is read first

  const TaskOptions both = OptionsFromCheckboxes(true, true);
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, both, program_, user_));
  ExpectRegistered(both, false);
  EXPECT_FALSE(InFolder());
  // Registering again changes nothing; the folder stays as it is (ApplyTaskOptions never
  // touches folders).
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, both, program_, user_));
  ExpectRegistered(both, false);
  EXPECT_TRUE(FolderExists());
  CheckEveryCombination(false);
}

// Next to a folder that is not trusted, the task under the fallback name is the one read (and
// the one the check's repair registers again), whatever copy the folder holds: ApplyTaskOptions
// registers there, and a copy in the folder that it could not delete must not hide it.
TEST_F(TaskWriting, ReadTaskReadsTheFallbackNameFirstNextToAnUntrustedFolder) {
  const TaskOptions left{TriggerSetting::kDisabled, TriggerSetting::kEnabled, true};
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, left, program_, user_));
  MakeUntrustedFolder();
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), location_.folder, location_.name,
                                         test::SelfPath(), user_.userSid));
  ASSERT_TRUE(InFolder());
  ASSERT_TRUE(AtFallback());
  TaskState state;
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  ASSERT_TRUE(state.exists);
  EXPECT_TRUE(state.atFallback);
  EXPECT_EQ(state.actionPath, program_);
  EXPECT_EQ(state.options.afterPdfCraft, left.afterPdfCraft);
  EXPECT_EQ(state.options.atSignIn, left.atSignIn);

  // The stale repair keeps the fallback copy's triggers, not the folder copy's (it has none).
  const test::TempDir root(L"task-root");
  ASSERT_TRUE(root.ok());
  const std::filesystem::path exe = std::filesystem::path(root.path()) / kSettingsExe;
  std::ofstream(exe) << "";  // never started
  const test::ScopedRegistrySandbox sandbox;
  ASSERT_TRUE(sandbox.ok()) << Hex(sandbox.error());
  const RegistryRoots roots{sandbox.user(), sandbox.machine()};
  Snapshot s;
  s.context = user_;
  s.scope = Scope::kUser;
  s.paths.userRoot = root.path();
  s.installRoot = root.path();
  s.settingsExeFound = true;
  s.taskLocation = location_;
  s.task = state;
  RepairPlan plan;
  plan.user.push_back(RepairStep{RepairStep::Kind::kReRegisterTask});
  plan.user[0].path = exe;
  std::vector<RepairOutcome> outcomes;
  ASSERT_HRESULT_SUCCEEDED(ApplyUserRepairs(roots, RealSystemProbes(), s, plan, &outcomes));
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes[0].result, RepairResult::kDone) << Hex(outcomes[0].hr);
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  ASSERT_TRUE(state.exists);
  EXPECT_TRUE(state.atFallback);
  EXPECT_TRUE(IEquals(state.actionPath, exe.native())) << Narrow(state.actionPath);
  EXPECT_EQ(state.options.afterPdfCraft, left.afterPdfCraft);
  EXPECT_EQ(state.options.atSignIn, left.atSignIn);
  EXPECT_FALSE(InFolder());  // this user's own copy, which they may delete

  // With nothing under the fallback name, the user's own copy in the folder is still theirs.
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), location_.folder, location_.name,
                                         test::SelfPath(), user_.userSid));
  if (const wil::com_ptr_nothrow<ITaskFolder> rootFolder = GetFolder(service_.get(), L"\\")) {
    DeleteNamedTask(rootFolder.get(), location_.fallbackName);
  }
  ASSERT_FALSE(AtFallback());
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  ASSERT_TRUE(state.exists);
  EXPECT_FALSE(state.atFallback);
  EXPECT_EQ(state.actionPath, test::SelfPath());
}

// A task under the user's name that runs as another account is not the user's, in either place:
// someone else put it there. Here the tasks run as this account and the location names a
// made-up user. Under the fallback name, the only place the task can go next to a folder that is
// missing or not trusted, it takes the name (kTaskTakenHr); in that folder alone it does not.
TEST_F(TaskWriting, ReadTaskSkipsATaskThatRunsAsSomeoneElse) {
  TaskLocation someoneElse = location_;
  someoneElse.userSid = L"S-1-12-1-1-2-3-4";
  ASSERT_HRESULT_SUCCEEDED(
      RegisterPlain(service_.get(), L"\\", location_.fallbackName, program_, user_.userSid));
  TaskState state;
  state.exists = true;
  EXPECT_EQ(ReadTask(someoneElse, &state), kTaskTakenHr);
  EXPECT_FALSE(state.exists);
  MakeUntrustedFolder();
  ASSERT_HRESULT_SUCCEEDED(
      RegisterPlain(service_.get(), location_.folder, location_.name, program_, user_.userSid));
  EXPECT_EQ(ReadTask(someoneElse, &state), kTaskTakenHr);
  EXPECT_FALSE(state.exists);
  // For the user they run as, both are theirs, the fallback name first.
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  EXPECT_TRUE(state.exists);
  EXPECT_TRUE(state.atFallback);
  // Read by path, without a user, a task is whoever's it is.
  TaskLocation byPath = location_;
  byPath.userSid.clear();
  byPath.fallbackName.clear();
  ASSERT_HRESULT_SUCCEEDED(ReadTask(byPath, &state));
  EXPECT_TRUE(state.exists);
  EXPECT_FALSE(state.atFallback);
  // With the fallback name free, the copy in the folder is only skipped.
  const wil::com_ptr_nothrow<ITaskFolder> root = GetFolder(service_.get(), L"\\");
  ASSERT_TRUE(root);
  DeleteNamedTask(root.get(), location_.fallbackName);
  ASSERT_FALSE(AtFallback());
  EXPECT_EQ(ReadTask(someoneElse, &state), E_ACCESSDENIED);
  EXPECT_FALSE(state.exists);
}

// The user's own task is updated where it is, whatever it ran before.
TEST_F(TaskWriting, TheUsersOwnTaskIsUpdatedWhereItIs) {
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), L"\\", location_.fallbackName,
                                         test::SelfPath(), user_.userSid));
  const TaskOptions both = OptionsFromCheckboxes(true, true);
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, both, program_, user_));
  ExpectRegistered(both, false);
  const TaskOptions signIn = OptionsFromCheckboxes(false, true);
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, signIn, program_, user_));
  ExpectRegistered(signIn, false);
}

// Another account's task at every name the user's task may have is never registered over or
// deleted: ApplyTaskOptions answers kTaskTakenHr and changes nothing, and ReadTask gives the same
// answer. Here the tasks run as this account and the context names a made-up user, so no
// administrator is needed (the protected folder's tests show it with another account's tasks).
TEST_F(TaskWriting, AnotherAccountsTaskIsNeverRegisteredOver) {
  ProcessContext madeUpUser = user_;
  madeUpUser.userSid = L"S-1-12-1-1-2-3-4";
  TaskLocation madeUp = location_;
  madeUp.userSid = madeUpUser.userSid;
  const TaskOptions both = OptionsFromCheckboxes(true, true);
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), L"\\", location_.fallbackName,
                                         test::SelfPath(), user_.userSid));
  const std::wstring atFallback = TaskXml(AtFallback().get());
  ASSERT_FALSE(atFallback.empty());

  // No folder: the fallback name is the only place, and another account has it.
  EXPECT_EQ(ApplyTaskOptions(madeUp, both, program_, madeUpUser), kTaskTakenHr);
  EXPECT_EQ(TaskXml(AtFallback().get()), atFallback);
  TaskState state;
  state.exists = true;
  EXPECT_EQ(ReadTask(madeUp, &state), kTaskTakenHr);
  EXPECT_FALSE(state.exists);

  // A folder that is not trusted is no place for the task either, and nothing changes in it.
  MakeUntrustedFolder();
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), location_.folder, location_.name,
                                         test::SelfPath(), user_.userSid));
  const std::wstring inFolder = TaskXml(InFolder().get());
  ASSERT_FALSE(inFolder.empty());
  EXPECT_EQ(ApplyTaskOptions(madeUp, both, program_, madeUpUser), kTaskTakenHr);
  EXPECT_EQ(TaskXml(AtFallback().get()), atFallback);
  EXPECT_EQ(TaskXml(InFolder().get()), inFolder);

  // For the account they run as, both are its own: updated under the fallback name, and the copy
  // in the folder goes.
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, both, program_, user_));
  ExpectRegistered(both, false);
}

TEST_F(TaskWriting, DeleteTaskTakesBothCopiesAndNothingElse) {
  MakeUntrustedFolder();
  ASSERT_HRESULT_SUCCEEDED(
      RegisterPlain(service_.get(), location_.folder, location_.name, program_, user_.userSid));
  ASSERT_HRESULT_SUCCEEDED(
      RegisterPlain(service_.get(), L"\\", location_.fallbackName, program_, user_.userSid));
  const std::wstring other = rootPrefix_ + L"other";
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), L"\\", other, program_, user_.userSid));
  EXPECT_EQ(DeleteTask(location_), S_OK);
  EXPECT_FALSE(InFolder());
  EXPECT_FALSE(AtFallback());
  EXPECT_TRUE(GetTask(service_.get(), L"\\", other));
  EXPECT_TRUE(FolderExists());
  EXPECT_EQ(DeleteTask(location_), S_FALSE);
}

// Task Scheduler stores a logon trigger's user as the account, whether it was given as
// "DOMAIN\user" or as the SID string: ApplyTaskOptions may use the SID when the account cannot
// be looked up.
TEST_F(TaskWriting, LogonTriggerUserGivenAsSidIsStoredAsTheAccount) {
  if (IsSidString(user_.userAccount)) {
    GTEST_SKIP() << "this account's name could not be looked up";
  }
  const auto storedUser = [&](const ProcessContext& context) {
    EXPECT_HRESULT_SUCCEEDED(
        ApplyTaskOptions(location_, OptionsFromCheckboxes(false, true), program_, context));
    const wil::com_ptr_nothrow<IRegisteredTask> task = AtFallback();
    if (!task) return std::wstring();
    wil::unique_bstr xml;
    EXPECT_HRESULT_SUCCEEDED(task->get_Xml(xml.put()));
    return Contents(Element(Bstr(xml), L"<LogonTrigger", L"</LogonTrigger>"), L"UserId");
  };
  const std::wstring byName = storedUser(user_);
  ProcessContext bySidContext = user_;
  bySidContext.userAccount = user_.userSid;
  const std::wstring bySid = storedUser(bySidContext);
  EXPECT_TRUE(IEquals(byName, user_.userAccount));
  EXPECT_TRUE(IEquals(bySid, byName));
  std::printf("[ tasks ] logon trigger user stored the same from the name and the SID: %s\n",
              IEquals(bySid, byName) ? "yes" : "no");
}

TEST_F(TaskWriting, DeletesATaskByPathLikeTheLegacyTask) {
  const std::wstring name = rootPrefix_ + L"legacy";
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), L"\\", name, program_, user_.userSid));
  EXPECT_EQ(DeleteLegacyTask(L"\\" + name), S_OK);
  EXPECT_FALSE(GetTask(service_.get(), L"\\", name));
  EXPECT_EQ(DeleteLegacyTask(L"\\" + name), S_FALSE);
  EXPECT_EQ(DeleteLegacyTask(L"\\" + TestFolderStart() + id_ + L"\\" + name), S_FALSE);
  EXPECT_EQ(DeleteLegacyTask(L"no-backslash"), E_INVALIDARG);
}

TEST_F(TaskWriting, SharedFolderStepsRefuseUnelevated) {
  if (context_.elevated) GTEST_SKIP() << "elevated here; the elevated tests cover these steps";
  EXPECT_EQ(EnsureTaskFolder(location_.folder), E_ACCESSDENIED);
  EXPECT_FALSE(FolderExists());
  EXPECT_EQ(DeleteTaskFolderAllUsers(location_.folder, rootPrefix_), E_ACCESSDENIED);
}

// Asked directly, Task Scheduler itself refuses to let an unelevated process make a folder owned
// by Administrators, so only an administrator can make the shared folder as it should be (and a
// folder a user makes instead is theirs, which ApplyTaskOptions does not trust). From a standard
// account this is part of the manual check in docs/BUILDING.md.
TEST_F(TaskWriting, UnelevatedCannotMakeAFolderOwnedByAdministrators) {
  if (context_.elevated) GTEST_SKIP() << "elevated here; EnsureTaskFolder's tests make the folder";
  const wil::com_ptr_nothrow<ITaskFolder> root = GetFolder(service_.get(), L"\\");
  ASSERT_TRUE(root);
  wil::unique_variant descriptor;
  descriptor.vt = VT_BSTR;
  descriptor.bstrVal = SysAllocString(kTaskFolderSddl);
  ASSERT_TRUE(descriptor.bstrVal);
  wil::com_ptr_nothrow<ITaskFolder> made;
  const HRESULT hr = root->CreateFolder(MakeBstr(location_.folder).get(), descriptor, made.put());
  std::printf("[ tasks ] unelevated folder owned by Administrators: %s\n", Hex(hr).c_str());
  EXPECT_TRUE(FAILED(hr)) << Hex(hr);
  EXPECT_FALSE(FolderExists());
}

// Task Scheduler starts the program as the user: the principal, interactive token and settings
// are ones it runs. The action becomes pdft-run-medium.exe --exit 0 for this.
TEST_F(TaskWriting, TaskSchedulerRunsTheTask) {
  if (!GetShellWindow()) {
    GTEST_SKIP() << "no interactive desktop here: Task Scheduler runs interactive-token tasks only "
                    "in the user's session";
  }
  ASSERT_HRESULT_SUCCEEDED(
      ApplyTaskOptions(location_, OptionsFromCheckboxes(true, true), program_, user_));
  wil::com_ptr_nothrow<IRegisteredTask> task = AtFallback();
  ASSERT_TRUE(task);
  {
    wil::com_ptr_nothrow<ITaskDefinition> definition;
    ASSERT_HRESULT_SUCCEEDED(task->get_Definition(definition.put()));
    wil::com_ptr_nothrow<IActionCollection> actions;
    ASSERT_HRESULT_SUCCEEDED(definition->get_Actions(actions.put()));
    wil::com_ptr_nothrow<IAction> action;
    ASSERT_HRESULT_SUCCEEDED(actions->get_Item(1, action.put()));
    wil::com_ptr_nothrow<IExecAction> exec;
    ASSERT_HRESULT_SUCCEEDED(action->QueryInterface(IID_PPV_ARGS(exec.put())));
    ASSERT_HRESULT_SUCCEEDED(exec->put_Arguments(MakeBstr(L"--exit 0").get()));
    const wil::com_ptr_nothrow<ITaskFolder> root = GetFolder(service_.get(), L"\\");
    ASSERT_TRUE(root);
    VARIANT empty;
    VariantInit(&empty);
    task.reset();
    ASSERT_HRESULT_SUCCEEDED(root->RegisterTaskDefinition(
        MakeBstr(location_.fallbackName).get(), definition.get(), TASK_UPDATE, empty, empty,
        TASK_LOGON_INTERACTIVE_TOKEN, empty, task.put()));
  }
  VARIANT noParameters;
  VariantInit(&noParameters);
  wil::com_ptr_nothrow<IRunningTask> running;
  const HRESULT started = task->Run(noParameters, running.put());
  if (test::IsApplicationControlBlock(started)) {
    GTEST_SKIP() << "Application Control blocked pdft-run-medium.exe (" << Hex(started) << ")";
  }
  // Task Scheduler has no session of this account to run the task in: an elevated runner (CI)
  // may have none for its account whatever its desktop shows, and a prompt started with "runas"
  // shows the signed-in user's desktop, not a session of its own account.
  const bool noSessionOfItsOwn = context_.elevated || !ShellIsThisUsers();
  const HRESULT refused = HRESULT_FROM_WIN32(ERROR_REQUEST_REFUSED);
  if (started == SCHED_E_USER_NOT_LOGGED_ON || started == SCHED_E_SERVICE_NOT_RUNNING ||
      (noSessionOfItsOwn && started == refused)) {
    GTEST_SKIP() << "Task Scheduler does not run interactive-token tasks here (" << Hex(started)
                 << ")";
  }
  ASSERT_HRESULT_SUCCEEDED(started);
  running.reset();

  LONG result = SCHED_S_TASK_HAS_NOT_RUN;
  TASK_STATE state = TASK_STATE_UNKNOWN;
  const ULONGLONG deadline = GetTickCount64() + 60000;
  while (GetTickCount64() < deadline) {
    Sleep(250);
    if (FAILED(task->get_State(&state)) || FAILED(task->get_LastTaskResult(&result))) continue;
    if (state != TASK_STATE_RUNNING && state != TASK_STATE_QUEUED &&
        result != SCHED_S_TASK_HAS_NOT_RUN && result != SCHED_S_TASK_RUNNING) {
      break;
    }
  }
  std::printf("[ tasks ] run: state %d, last result %s\n", static_cast<int>(state),
              Hex(result).c_str());
  if (test::IsApplicationControlBlock(result)) {
    GTEST_SKIP() << "Application Control blocked pdft-run-medium.exe (" << Hex(result)
                 << "); not retried";
  }
  if (noSessionOfItsOwn && (result == SCHED_S_TASK_HAS_NOT_RUN ||
                            result == SCHED_E_USER_NOT_LOGGED_ON || result == refused)) {
    GTEST_SKIP() << "Task Scheduler did not start the interactive-token task here (state "
                 << static_cast<int>(state) << ", " << Hex(result) << ")";
  }
  EXPECT_EQ(result, 0) << Hex(result);
}

// ---- Elevated: the shared folder ----------------------------------------------------------

class TaskFolderElevated : public TaskWriting {
 protected:
  void SetUp() override {
    TaskWriting::SetUp();
    if (HasFatalFailure()) return;
    if (!context_.elevated) {
      GTEST_SKIP() << "needs elevation (the shared folder is made and removed by elevated code "
                      "only); runs on CI";
    }
  }

  std::wstring FolderSddl() {
    const wil::com_ptr_nothrow<ITaskFolder> folder = GetFolder(service_.get(), location_.folder);
    wil::unique_bstr sddl;
    if (!folder ||
        FAILED(folder->GetSecurityDescriptor(
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            sddl.put()))) {
      return {};
    }
    return Bstr(sddl);
  }

  // Runs this test program as the child "--pdft-child <arguments>" as a standard user (this
  // account with a filtered Medium token: Administrators deny-only) and gives its line of output.
  // The result is the child's start: an Application Control block is the caller's reason to skip.
  HRESULT RunAsStandardUser(const std::wstring& arguments, std::string* line) {
    line->clear();
    wil::unique_handle token;
    const HRESULT made = test::CreateMediumTokenForSelf(test::MediumToken::kFiltered, token.put());
    if (FAILED(made)) return made;
    test::PipedChild child;
    const HRESULT started =
        child.Start(test::SelfPath(), L"--pdft-child " + arguments, token.get());
    if (FAILED(started)) return started;
    *line = child.ReadLine();
    EXPECT_EQ(child.Wait(60000), 0u) << Narrow(arguments);
    return S_OK;
  }

  // The child arguments that register this user's check task in the test location.
  std::wstring ApplyTaskArguments() const {
    return L"apply-task " + test::QuoteArgument(location_.folder) + L" " +
           test::QuoteArgument(location_.name) + L" " +
           test::QuoteArgument(location_.fallbackName) + L" " + test::QuoteArgument(program_);
  }
};

TEST_F(TaskFolderElevated, EnsureTaskFolderMakesTheProtectedFolder) {
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  const std::wstring sddl = FolderSddl();
  std::printf("[ tasks ] folder made as %s\n", Narrow(sddl).c_str());
  EXPECT_TRUE(IsTrustedTaskFolderSddl(sddl)) << Narrow(sddl);
  EXPECT_TRUE(OwnerIs(sddl, L"BA")) << Narrow(sddl);
  EXPECT_TRUE(Has(sddl, L"D:P")) << Narrow(sddl);  // nothing inherited from the root folder
  EXPECT_TRUE(Has(sddl, L"(A;;FW;;;AU)")) << Narrow(sddl);
  EXPECT_TRUE(Has(sddl, L"(A;OIIO;FA;;;CO)")) << Narrow(sddl);
  EXPECT_FALSE(Has(sddl, L"(A;CI;FW;;;AU)")) << Narrow(sddl);
  EXPECT_EQ(EnsureTaskFolder(location_.folder), S_FALSE);  // there and trusted
  EXPECT_EQ(EnsureTaskFolder(L"\\a\\b"), E_INVALIDARG);

  // The user's task goes into it, and what the task inherits gives nobody but administrators,
  // SYSTEM and its own user any access.
  CheckEveryCombination(true);
  const TaskOptions both = OptionsFromCheckboxes(true, true);
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, both, program_, user_));
  const wil::com_ptr_nothrow<IRegisteredTask> task = InFolder();
  ASSERT_TRUE(task);
  wil::unique_bstr taskSddl;
  ASSERT_HRESULT_SUCCEEDED(task->GetSecurityDescriptor(
      OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
      taskSddl.put()));
  const std::wstring text = Bstr(taskSddl);
  std::printf("[ tasks ] task in the folder: %s\n", Narrow(text).c_str());
  for (const wchar_t* nobody : {L";AU)", L";WD)", L";BU)", L";IU)", L";NS)", L";LS)"}) {
    EXPECT_FALSE(Has(text, nobody)) << Narrow(text);
  }
  EXPECT_TRUE(Has(text, L"(A;ID;0x1f019f;;;BA)")) << Narrow(text);
  EXPECT_TRUE(Has(text, L"(A;ID;0x1f019f;;;SY)")) << Narrow(text);
}

// A standard user made the folder first, so they own it and could rewrite its permissions:
// EnsureTaskFolder takes it over, owner included.
TEST_F(TaskFolderElevated, EnsureTaskFolderRepairsAFolderAStandardUserMade) {
  std::string line;
  const HRESULT started =
      RunAsStandardUser(L"make-folder " + test::QuoteArgument(location_.folder), &line);
  if (test::IsApplicationControlBlock(started)) {
    GTEST_SKIP() << "Application Control blocked the test program at Medium integrity";
  }
  ASSERT_HRESULT_SUCCEEDED(started);
  ASSERT_EQ(line, "make-folder hr=0x00000000");
  const std::wstring before = FolderSddl();
  std::printf("[ tasks ] folder a standard user made: %s\n", Narrow(before).c_str());
  EXPECT_TRUE(OwnerIs(before, context_.userSid.c_str())) << Narrow(before);
  EXPECT_FALSE(IsTrustedTaskFolderSddl(before)) << Narrow(before);

  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  const std::wstring after = FolderSddl();
  std::printf("[ tasks ] repaired a folder a standard user made: %s\n", Narrow(after).c_str());
  EXPECT_TRUE(OwnerIs(after, L"BA")) << Narrow(after);
  EXPECT_TRUE(IsTrustedTaskFolderSddl(after)) << Narrow(after);
  EXPECT_EQ(EnsureTaskFolder(location_.folder), S_FALSE);
}

// An untrusted folder that an administrator made (owned by Administrators already, but with the
// root folder's permissions inherited): only its permissions need replacing.
TEST_F(TaskFolderElevated, EnsureTaskFolderRepairsAFolderAnAdministratorMade) {
  MakeUntrustedFolder();
  EXPECT_TRUE(OwnerIs(FolderSddl(), L"BA")) << Narrow(FolderSddl());
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  const std::wstring sddl = FolderSddl();
  EXPECT_TRUE(IsTrustedTaskFolderSddl(sddl)) << Narrow(sddl);
  EXPECT_EQ(EnsureTaskFolder(location_.folder), S_FALSE);
}

// A task under the user's name in the protected folder that runs as another account (only an
// administrator could put one there) is not the user's: the copy under the fallback name is.
TEST_F(TaskFolderElevated, ReadTaskSkipsSomeoneElsesTaskInTheProtectedFolder) {
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), location_.folder, location_.name, program_,
                                         L"S-1-5-18", TASK_LOGON_SERVICE_ACCOUNT));
  TaskState state;
  EXPECT_EQ(ReadTask(location_, &state), E_ACCESSDENIED);
  EXPECT_FALSE(state.exists);
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), L"\\", location_.fallbackName,
                                         test::SelfPath(), user_.userSid));
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  ASSERT_TRUE(state.exists);
  EXPECT_TRUE(state.atFallback);
  EXPECT_EQ(state.actionPath, test::SelfPath());
  std::printf("[ tasks ] someone else's task in the folder skipped: %s\n",
              state.atFallback ? "yes" : "no");
}

// Another account's task under the user's name in the protected folder (SYSTEM's here, which only
// an administrator can register) is neither registered over nor deleted: the user's task goes
// under the fallback name, registered there anew and then updated where it is.
TEST_F(TaskFolderElevated, AnotherAccountsTaskInTheFolderStaysAndTheFallbackNameIsUsed) {
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), location_.folder, location_.name,
                                         test::SelfPath(), kSystemSid, TASK_LOGON_SERVICE_ACCOUNT));
  const std::wstring theirs = TaskXml(InFolder().get());
  ASSERT_FALSE(theirs.empty());
  ASSERT_FALSE(IEquals(TaskPrincipalUserId(theirs), user_.userSid)) << Narrow(theirs);
  for (const bool atSignIn : {true, false}) {
    SCOPED_TRACE(testing::Message() << "atSignIn " << atSignIn);
    ASSERT_HRESULT_SUCCEEDED(
        ApplyTaskOptions(location_, OptionsFromCheckboxes(true, atSignIn), program_, user_));
    TaskState state;
    ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
    ASSERT_TRUE(state.exists);
    EXPECT_TRUE(state.atFallback);
    EXPECT_EQ(state.atSignIn, atSignIn);
    EXPECT_EQ(state.actionPath, program_);
    EXPECT_EQ(TaskXml(InFolder().get()), theirs);
  }
  const bool kept = TaskXml(InFolder().get()) == theirs && AtFallback();
  std::printf("[ tasks ] another account's task in the folder kept, the fallback name used: %s\n",
              kept ? "yes" : "no");
}

// Another account's task under the user's fallback name (SYSTEM's here) is not the user's other
// copy: the user's task goes into the protected folder, and that task stays as it is.
TEST_F(TaskFolderElevated, AnotherAccountsTaskUnderTheFallbackNameStaysWhenTheFolderIsUsed) {
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), L"\\", location_.fallbackName,
                                         test::SelfPath(), kSystemSid, TASK_LOGON_SERVICE_ACCOUNT));
  const std::wstring theirs = TaskXml(AtFallback().get());
  ASSERT_FALSE(theirs.empty());
  ASSERT_FALSE(IEquals(TaskPrincipalUserId(theirs), user_.userSid)) << Narrow(theirs);
  ASSERT_HRESULT_SUCCEEDED(
      ApplyTaskOptions(location_, OptionsFromCheckboxes(true, true), program_, user_));
  ASSERT_TRUE(InFolder());
  TaskState state;
  ASSERT_HRESULT_SUCCEEDED(ReadTask(location_, &state));
  ASSERT_TRUE(state.exists);
  EXPECT_FALSE(state.atFallback);
  EXPECT_EQ(state.actionPath, program_);
  EXPECT_EQ(TaskXml(AtFallback().get()), theirs);
  const bool kept = InFolder() && TaskXml(AtFallback().get()) == theirs;
  std::printf(
      "[ tasks ] another account's task under the fallback name kept, the folder used: %s\n",
      kept ? "yes" : "no");
}

// The user's own task in the protected folder is updated where it is.
TEST_F(TaskFolderElevated, TheUsersOwnTaskInTheFolderIsUpdatedThere) {
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), location_.folder, location_.name,
                                         test::SelfPath(), user_.userSid));
  const TaskOptions both = OptionsFromCheckboxes(true, true);
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, both, program_, user_));
  ExpectRegistered(both, true);
}

// Another account's tasks under both of the user's names: the user's task is registered nowhere
// and both stay as they are, and the answer says why (kTaskTakenHr), for a standard user too,
// whom Task Scheduler does not even let open those tasks.
TEST_F(TaskFolderElevated, AnotherAccountsTasksAtBothNamesLeaveTheUserNone) {
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), location_.folder, location_.name,
                                         test::SelfPath(), kSystemSid, TASK_LOGON_SERVICE_ACCOUNT));
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), L"\\", location_.fallbackName,
                                         test::SelfPath(), kSystemSid, TASK_LOGON_SERVICE_ACCOUNT));
  const std::wstring inFolder = TaskXml(InFolder().get());
  const std::wstring atFallback = TaskXml(AtFallback().get());
  for (const std::wstring* xml : {&inFolder, &atFallback}) {
    ASSERT_FALSE(xml->empty());
    ASSERT_FALSE(IEquals(TaskPrincipalUserId(*xml), user_.userSid)) << Narrow(*xml);
  }
  const auto unchanged = [&] {
    EXPECT_EQ(TaskXml(InFolder().get()), inFolder);
    EXPECT_EQ(TaskXml(AtFallback().get()), atFallback);
  };

  EXPECT_EQ(ApplyTaskOptions(location_, OptionsFromCheckboxes(true, true), program_, user_),
            kTaskTakenHr);
  unchanged();
  TaskState state;
  state.exists = true;
  EXPECT_EQ(ReadTask(location_, &state), kTaskTakenHr);
  EXPECT_FALSE(state.exists);

  std::string line;
  const HRESULT started = RunAsStandardUser(ApplyTaskArguments(), &line);
  if (test::IsApplicationControlBlock(started)) {
    GTEST_SKIP() << "Application Control blocked the test program at Medium integrity";
  }
  ASSERT_HRESULT_SUCCEEDED(started);
  std::printf("[ tasks ] standard user with both names taken: %s\n", line.c_str());
  EXPECT_EQ(line, "apply-task hr=" + Hex(kTaskTakenHr) + " elevated=0");
  unchanged();
}

// Once the folder is there, the task moves into it and the copy under the fallback name goes.
TEST_F(TaskFolderElevated, OneCopyWhenTheFolderAppears) {
  const TaskOptions both = OptionsFromCheckboxes(true, true);
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, both, program_, user_));
  ExpectRegistered(both, false);
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  ASSERT_HRESULT_SUCCEEDED(ApplyTaskOptions(location_, both, program_, user_));
  ExpectRegistered(both, true);
  EXPECT_FALSE(AtFallback());
}

// A standard user may add their task to the protected folder.
TEST_F(TaskFolderElevated, StandardUserRegistersInTheProtectedFolder) {
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  std::string line;
  const HRESULT started = RunAsStandardUser(ApplyTaskArguments(), &line);
  if (test::IsApplicationControlBlock(started)) {
    GTEST_SKIP() << "Application Control blocked the test program at Medium integrity";
  }
  ASSERT_HRESULT_SUCCEEDED(started);
  std::printf("[ tasks ] standard user: %s\n", line.c_str());
  EXPECT_EQ(line, "apply-task hr=0x00000000 elevated=0");
  ExpectRegistered(OptionsFromCheckboxes(true, true), true);
}

TEST_F(TaskFolderElevated, DeleteTaskFolderAllUsersRemovesEveryUsersTask) {
  ASSERT_EQ(EnsureTaskFolder(location_.folder), S_OK);
  // The user's task as a standard user registers it: the entry for its creator gives them full
  // control of it, permissions included.
  std::string line;
  const HRESULT started = RunAsStandardUser(ApplyTaskArguments(), &line);
  if (test::IsApplicationControlBlock(started)) {
    GTEST_SKIP() << "Application Control blocked the test program at Medium integrity";
  }
  ASSERT_HRESULT_SUCCEEDED(started);
  ASSERT_EQ(line, "apply-task hr=0x00000000 elevated=0");
  ASSERT_TRUE(InFolder());
  const std::wstring userTask = TaskSddl(InFolder().get());
  std::printf("[ tasks ] a standard user's task in the folder: %s\n", Narrow(userTask).c_str());
  EXPECT_TRUE(Has(userTask, L"(A;ID;FA;;;" + SddlSid(context_.userSid) + L")")) << Narrow(userTask);
  // Another user's task in the folder, one in a subfolder, and under the root-folder name.
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), location_.folder,
                                         std::wstring(kTaskPrefix) + L"S-1-12-1-1-2-3-4", program_,
                                         user_.userSid));
  {
    const wil::com_ptr_nothrow<ITaskFolder> folder = GetFolder(service_.get(), location_.folder);
    ASSERT_TRUE(folder);
    VARIANT inherit;
    VariantInit(&inherit);
    wil::com_ptr_nothrow<ITaskFolder> subfolder;
    ASSERT_HRESULT_SUCCEEDED(
        folder->CreateFolder(MakeBstr(L"sub").get(), inherit, subfolder.put()));
  }
  ASSERT_HRESULT_SUCCEEDED(
      RegisterPlain(service_.get(), location_.folder + L"\\sub", L"task", program_, user_.userSid));
  const std::wstring otherUser = rootPrefix_ + L"S-1-12-1-1-2-3-5";
  const std::wstring notOurs = rootPrefix_ + L"not a SID";
  ASSERT_HRESULT_SUCCEEDED(
      RegisterPlain(service_.get(), L"\\", otherUser, program_, user_.userSid));
  ASSERT_HRESULT_SUCCEEDED(RegisterPlain(service_.get(), L"\\", notOurs, program_, user_.userSid));

  EXPECT_EQ(DeleteTaskFolderAllUsers(location_.folder, rootPrefix_), S_OK);
  EXPECT_FALSE(FolderExists());
  std::printf("[ tasks ] every user's task removed with the folder: %s\n",
              FolderExists() ? "no" : "yes");
  EXPECT_FALSE(GetTask(service_.get(), L"\\", otherUser));
  EXPECT_TRUE(GetTask(service_.get(), L"\\", notOurs));  // not prefix + SID: left alone
  EXPECT_EQ(DeleteTaskFolderAllUsers(location_.folder, rootPrefix_), S_FALSE);
  EXPECT_EQ(DeleteTaskFolderAllUsers(location_.folder, L""), E_INVALIDARG);
  EXPECT_EQ(DeleteTaskFolderAllUsers(L"\\", rootPrefix_), E_INVALIDARG);
}

}  // namespace
}  // namespace pdft
