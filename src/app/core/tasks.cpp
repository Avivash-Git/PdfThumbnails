#include "tasks.h"

#include <sddl.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <vector>

#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

// More triggers or actions than any task of ours has; a task with more is read only this far.
constexpr LONG kMaxItems = 64;

// The longest SID string Windows produces (SECURITY_MAX_SID_STRING_CHARACTERS).
constexpr size_t kMaxSidChars = 184;

// How deep DeleteTaskFolderAllUsers follows subfolders. Nothing of ours makes any; this only
// bounds what someone else may have added.
constexpr int kMaxFolderDepth = 8;

// What anyone but Administrators, SYSTEM and a task's own creator may hold on a trusted folder:
// adding tasks (FILE_GENERIC_WRITE, "FW"), which also lets them read its security descriptor.
constexpr ACCESS_MASK kAddTaskRights = FILE_GENERIC_WRITE;

// The check task's limits (Task Scheduler settings): it stops after five minutes.
constexpr wchar_t kTaskTimeLimit[] = L"PT5M";
constexpr wchar_t kPrincipalId[] = L"Author";

// GetSecurityDescriptor's information: the owner, the group and the DACL.
constexpr LONG kFolderSecurityInfo =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;

bool IsXmlSpace(wchar_t c) noexcept { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; }

// |xml| without white space at either end and between a '>' and the next '<'. May throw
// std::bad_alloc.
std::wstring WithoutSpaceBetweenTags(std::wstring_view xml) {
  size_t begin = 0;
  size_t end = xml.size();
  while (begin < end && IsXmlSpace(xml[begin])) ++begin;
  while (end > begin && IsXmlSpace(xml[end - 1])) --end;
  std::wstring result;
  result.reserve(end - begin);
  for (size_t at = begin; at < end;) {
    const wchar_t c = xml[at];
    result.push_back(c);
    ++at;
    if (c != L'>') continue;
    size_t next = at;
    while (next < end && IsXmlSpace(xml[next])) ++next;
    if (next < end && xml[next] == L'<') at = next;  // only space between two tags goes
  }
  return result;
}

HRESULT MakeBstr(std::wstring_view text, wil::unique_bstr& bstr) {
  bstr = wil::make_bstr_nothrow(std::wstring(text).c_str());
  return bstr ? S_OK : E_OUTOFMEMORY;
}

std::wstring BstrText(const wil::unique_bstr& bstr) {
  return bstr ? std::wstring(bstr.get(), SysStringLen(bstr.get())) : std::wstring();
}

HRESULT ConnectTaskService(wil::com_ptr_nothrow<ITaskService>& service) {
  RETURN_IF_FAILED(CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(service.put())));
  VARIANT empty;
  VariantInit(&empty);
  // This user on this computer.
  RETURN_IF_FAILED(service->Connect(empty, empty, empty, empty));
  return S_OK;
}

// Opens the folder |path|. S_FALSE (and no folder) when it does not exist; E_ACCESSDENIED when
// this user may not open it. No log line for either: they are expected answers.
HRESULT OpenFolder(ITaskService* service, std::wstring_view path,
                   wil::com_ptr_nothrow<ITaskFolder>& folder) {
  folder.reset();
  wil::unique_bstr folderPath;
  RETURN_IF_FAILED(MakeBstr(path, folderPath));
  const HRESULT hr = service->GetFolder(folderPath.get(), folder.put());
  if (IsTaskNotFound(hr)) {
    folder.reset();
    return S_FALSE;
  }
  return hr;
}

// Opens the task |name| in |folder|. S_FALSE (and no task) when the folder or the task does not
// exist, or the folder may not be opened; a task that is there but may not be opened fails with
// E_ACCESSDENIED. No log line for those answers: they are expected.
HRESULT OpenTask(ITaskService* service, std::wstring_view folder, std::wstring_view name,
                 wil::com_ptr_nothrow<IRegisteredTask>& task) {
  task.reset();
  wil::com_ptr_nothrow<ITaskFolder> taskFolder;
  HRESULT hr = OpenFolder(service, folder, taskFolder);
  if (hr == S_FALSE || hr == E_ACCESSDENIED) return S_FALSE;
  RETURN_IF_FAILED(hr);
  wil::unique_bstr taskName;
  RETURN_IF_FAILED(MakeBstr(name, taskName));
  hr = taskFolder->GetTask(taskName.get(), task.put());
  if (IsTaskNotFound(hr)) return S_FALSE;
  if (hr == E_ACCESSDENIED) return hr;
  RETURN_IF_FAILED(hr);
  return S_OK;
}

// The owner, group and DACL of |folder| as SDDL.
HRESULT FolderSddl(ITaskFolder* folder, std::wstring* sddl) {
  sddl->clear();
  wil::unique_bstr text;
  const HRESULT hr = folder->GetSecurityDescriptor(kFolderSecurityInfo, text.put());
  if (FAILED(hr)) return hr;
  *sddl = BstrText(text);
  return S_OK;
}

// Deletes the task |name| from |folder|: S_FALSE when it is not there, or when this user may not
// delete it (then it is not theirs: their own task gives them full control).
HRESULT DeleteTaskNamed(ITaskFolder* folder, std::wstring_view name) {
  wil::unique_bstr taskName;
  RETURN_IF_FAILED(MakeBstr(name, taskName));
  const HRESULT hr = folder->DeleteTask(taskName.get(), 0);
  if (IsTaskNotFound(hr) || hr == E_ACCESSDENIED) return S_FALSE;
  return hr;
}

// The same, with the folder given by its path: S_FALSE also when the folder does not exist or
// may not be opened.
HRESULT DeleteTaskAt(ITaskService* service, std::wstring_view folderPath, std::wstring_view name) {
  wil::com_ptr_nothrow<ITaskFolder> folder;
  const HRESULT hr = OpenFolder(service, folderPath, folder);
  if (hr == S_FALSE || hr == E_ACCESSDENIED) return S_FALSE;
  if (FAILED(hr)) return hr;
  return DeleteTaskNamed(folder.get(), name);
}

// The names of every task in |folder|, hidden ones included.
HRESULT TaskNames(ITaskFolder* folder, std::vector<std::wstring>* names) {
  names->clear();
  wil::com_ptr_nothrow<IRegisteredTaskCollection> tasks;
  RETURN_IF_FAILED(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.put()));
  LONG count = 0;
  RETURN_IF_FAILED(tasks->get_Count(&count));
  for (LONG index = 1; index <= count; ++index) {  // 1-based
    VARIANT item;
    VariantInit(&item);
    item.vt = VT_I4;
    item.lVal = index;
    wil::com_ptr_nothrow<IRegisteredTask> task;
    RETURN_IF_FAILED(tasks->get_Item(item, task.put()));
    wil::unique_bstr name;
    RETURN_IF_FAILED(task->get_Name(name.put()));
    names->push_back(BstrText(name));
  }
  return S_OK;
}

// The names of |folder|'s subfolders.
HRESULT SubfolderNames(ITaskFolder* folder, std::vector<std::wstring>* names) {
  names->clear();
  wil::com_ptr_nothrow<ITaskFolderCollection> folders;
  RETURN_IF_FAILED(folder->GetFolders(0, folders.put()));
  LONG count = 0;
  RETURN_IF_FAILED(folders->get_Count(&count));
  for (LONG index = 1; index <= count; ++index) {
    VARIANT item;
    VariantInit(&item);
    item.vt = VT_I4;
    item.lVal = index;
    wil::com_ptr_nothrow<ITaskFolder> subfolder;
    RETURN_IF_FAILED(folders->get_Item(item, subfolder.put()));
    wil::unique_bstr name;
    RETURN_IF_FAILED(subfolder->get_Name(name.put()));
    names->push_back(BstrText(name));
  }
  return S_OK;
}

// The first failure of a series of deletions that goes on after one, and whether any deleted
// something.
struct DeleteOutcome {
  HRESULT first = S_OK;
  bool deleted = false;

  void Note(HRESULT hr) noexcept {
    if (hr == S_OK) deleted = true;
    if (FAILED(hr) && SUCCEEDED(first)) first = hr;
  }
  HRESULT Result() const noexcept { return FAILED(first) ? first : deleted ? S_OK : S_FALSE; }
};

// Deletes every task in |folder| and in its subfolders, and the subfolders.
void EmptyFolder(ITaskFolder* folder, int depth, DeleteOutcome* outcome) {
  std::vector<std::wstring> names;
  HRESULT hr = TaskNames(folder, &names);
  outcome->Note(hr);
  if (SUCCEEDED(hr)) {
    for (const std::wstring& name : names) outcome->Note(DeleteTaskNamed(folder, name));
  }
  hr = SubfolderNames(folder, &names);
  outcome->Note(hr);
  if (FAILED(hr)) return;
  for (const std::wstring& name : names) {
    if (depth >= kMaxFolderDepth) {
      outcome->Note(HRESULT_FROM_WIN32(ERROR_DIR_NOT_EMPTY));
      return;
    }
    wil::unique_bstr subfolderName;
    hr = MakeBstr(name, subfolderName);
    wil::com_ptr_nothrow<ITaskFolder> subfolder;
    if (SUCCEEDED(hr)) hr = folder->GetFolder(subfolderName.get(), subfolder.put());
    outcome->Note(hr);
    if (FAILED(hr)) continue;
    EmptyFolder(subfolder.get(), depth + 1, outcome);
    outcome->Note(folder->DeleteFolder(subfolderName.get(), 0));
  }
}

// Administrators enabled in this process's token, else E_ACCESSDENIED.
HRESULT RequireAdministrators() {
  BYTE administrators[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(administrators);
  RETURN_IF_WIN32_BOOL_FALSE(
      CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &size));
  BOOL member = FALSE;
  RETURN_IF_WIN32_BOOL_FALSE(CheckTokenMembership(nullptr, administrators, &member));
  return member ? S_OK : E_ACCESSDENIED;
}

bool IsAdministratorsOrSystem(PSID sid) noexcept {
  return IsWellKnownSid(sid, WinBuiltinAdministratorsSid) || IsWellKnownSid(sid, WinLocalSystemSid);
}

// S_OK when |folder|'s descriptor, read back, is trusted.
HRESULT RequireTrusted(ITaskFolder* folder) {
  std::wstring sddl;
  RETURN_IF_FAILED(FolderSddl(folder, &sddl));
  return IsTrustedTaskFolderSddl(sddl) ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_SECURITY_DESCR);
}

// A program the task may run: an absolute path without "%" (Task Scheduler expands environment
// variables in it) or quotes, to an existing file.
HRESULT CheckProgram(const std::filesystem::path& program) {
  const std::wstring& text = program.native();
  if (text.empty() || !program.is_absolute() || text.find_first_of(L"%\"") != std::wstring::npos ||
      text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\")) {
    return E_INVALIDARG;
  }
  const DWORD attributes = GetFileAttributesW(text.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }
  return S_OK;
}

// The registration info, principal and settings of the check task.
HRESULT DescribeTask(ITaskDefinition* definition, const TaskOptions& options,
                     std::wstring_view sid) {
  wil::com_ptr_nothrow<IRegistrationInfo> info;
  RETURN_IF_FAILED(definition->get_RegistrationInfo(info.put()));
  wil::unique_bstr text;
  RETURN_IF_FAILED(MakeBstr(kProductName, text));
  RETURN_IF_FAILED(info->put_Author(text.get()));
  RETURN_IF_FAILED(MakeBstr(TaskDescription(), text));
  RETURN_IF_FAILED(info->put_Description(text.get()));
  RETURN_IF_FAILED(
      MakeBstr(std::to_wstring(kVersion4[0]) + L"." + std::to_wstring(kVersion4[1]), text));
  RETURN_IF_FAILED(info->put_Version(text.get()));

  // The user, with their interactive token (the task runs only while they are signed in, and
  // needs no stored password), without elevation.
  wil::com_ptr_nothrow<IPrincipal> principal;
  RETURN_IF_FAILED(definition->get_Principal(principal.put()));
  RETURN_IF_FAILED(MakeBstr(kPrincipalId, text));
  RETURN_IF_FAILED(principal->put_Id(text.get()));
  RETURN_IF_FAILED(MakeBstr(sid, text));
  RETURN_IF_FAILED(principal->put_UserId(text.get()));
  RETURN_IF_FAILED(principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN));
  RETURN_IF_FAILED(principal->put_RunLevel(TASK_RUNLEVEL_LUA));

  // Runs on batteries too, late if the computer was off when it should have run, never twice at
  // once, and never longer than five minutes.
  wil::com_ptr_nothrow<ITaskSettings> settings;
  RETURN_IF_FAILED(definition->get_Settings(settings.put()));
  RETURN_IF_FAILED(settings->put_Enabled(options.enabled ? VARIANT_TRUE : VARIANT_FALSE));
  RETURN_IF_FAILED(settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE));
  RETURN_IF_FAILED(settings->put_StopIfGoingOnBatteries(VARIANT_FALSE));
  RETURN_IF_FAILED(settings->put_StartWhenAvailable(VARIANT_TRUE));
  RETURN_IF_FAILED(MakeBstr(kTaskTimeLimit, text));
  RETURN_IF_FAILED(settings->put_ExecutionTimeLimit(text.get()));
  RETURN_IF_FAILED(settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW));
  RETURN_IF_FAILED(settings->put_Compatibility(TASK_COMPATIBILITY_V2_4));
  return S_OK;
}

HRESULT AddTrigger(ITriggerCollection* triggers, const TriggerSpec& spec) {
  wil::com_ptr_nothrow<ITrigger> trigger;
  RETURN_IF_FAILED(triggers->Create(spec.type, trigger.put()));
  wil::unique_bstr text;
  RETURN_IF_FAILED(MakeBstr(spec.id, text));
  RETURN_IF_FAILED(trigger->put_Id(text.get()));
  RETURN_IF_FAILED(trigger->put_Enabled(spec.enabled ? VARIANT_TRUE : VARIANT_FALSE));
  wil::unique_bstr delay;
  RETURN_IF_FAILED(MakeBstr(spec.delay, delay));
  if (spec.type == TASK_TRIGGER_LOGON) {
    wil::com_ptr_nothrow<ILogonTrigger> logon;
    RETURN_IF_FAILED(trigger->QueryInterface(IID_PPV_ARGS(logon.put())));
    RETURN_IF_FAILED(MakeBstr(spec.userId, text));
    RETURN_IF_FAILED(logon->put_UserId(text.get()));
    RETURN_IF_FAILED(logon->put_Delay(delay.get()));
  } else {
    wil::com_ptr_nothrow<IEventTrigger> event;
    RETURN_IF_FAILED(trigger->QueryInterface(IID_PPV_ARGS(event.put())));
    RETURN_IF_FAILED(MakeBstr(spec.subscription, text));
    RETURN_IF_FAILED(event->put_Subscription(text.get()));
    RETURN_IF_FAILED(event->put_Delay(delay.get()));
  }
  return S_OK;
}

// The check task's whole definition.
HRESULT DefineTask(ITaskService* service, const TaskOptions& options,
                   const std::filesystem::path& program, const ProcessContext& context,
                   wil::com_ptr_nothrow<ITaskDefinition>& definition) {
  RETURN_IF_FAILED(service->NewTask(0, definition.put()));
  RETURN_IF_FAILED(DescribeTask(definition.get(), options, context.userSid));

  wil::com_ptr_nothrow<ITriggerCollection> triggers;
  RETURN_IF_FAILED(definition->get_Triggers(triggers.put()));
  for (const TriggerSpec& spec : BuildTriggerSpecs(options, context.userAccount)) {
    RETURN_IF_FAILED(AddTrigger(triggers.get(), spec));
  }

  // The program as it is, without quotes: Task Scheduler starts it by this path, spaces and all.
  wil::com_ptr_nothrow<IActionCollection> actions;
  RETURN_IF_FAILED(definition->get_Actions(actions.put()));
  wil::com_ptr_nothrow<IAction> action;
  RETURN_IF_FAILED(actions->Create(TASK_ACTION_EXEC, action.put()));
  wil::com_ptr_nothrow<IExecAction> exec;
  RETURN_IF_FAILED(action->QueryInterface(IID_PPV_ARGS(exec.put())));
  wil::unique_bstr text;
  RETURN_IF_FAILED(MakeBstr(program.native(), text));
  RETURN_IF_FAILED(exec->put_Path(text.get()));
  RETURN_IF_FAILED(MakeBstr(kCheckTaskArguments, text));
  RETURN_IF_FAILED(exec->put_Arguments(text.get()));
  return S_OK;
}

// Registers |definition| as |name| in |folder|, creating or replacing it, with the principal's
// interactive token. "Some triggers failed" is a failure (kSomeTriggersFailed). No log line for
// E_ACCESSDENIED: the caller falls back.
HRESULT RegisterIn(ITaskFolder* folder, std::wstring_view name, ITaskDefinition* definition) {
  wil::unique_bstr taskName;
  RETURN_IF_FAILED(MakeBstr(name, taskName));
  VARIANT empty;
  VariantInit(&empty);
  wil::com_ptr_nothrow<IRegisteredTask> registered;
  const HRESULT hr =
      folder->RegisterTaskDefinition(taskName.get(), definition, TASK_CREATE_OR_UPDATE, empty,
                                     empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, registered.put());
  if (hr == SCHED_S_SOME_TRIGGERS_FAILED) return kSomeTriggersFailed;
  return hr;
}

HRESULT ReadTriggers(ITaskDefinition* definition, TaskTriggers* result, TaskOptions* settings) {
  wil::com_ptr_nothrow<ITriggerCollection> triggers;
  RETURN_IF_FAILED(definition->get_Triggers(triggers.put()));
  LONG count = 0;
  RETURN_IF_FAILED(triggers->get_Count(&count));
  std::vector<TriggerFacts> facts;
  for (LONG index = 1; index <= count && index <= kMaxItems; ++index) {  // 1-based
    wil::com_ptr_nothrow<ITrigger> trigger;
    RETURN_IF_FAILED(triggers->get_Item(index, trigger.put()));
    TriggerFacts fact;
    RETURN_IF_FAILED(trigger->get_Type(&fact.type));
    VARIANT_BOOL enabled = VARIANT_TRUE;
    RETURN_IF_FAILED(trigger->get_Enabled(&enabled));
    fact.enabled = enabled != VARIANT_FALSE;
    if (fact.type == TASK_TRIGGER_EVENT) {
      if (const auto event = trigger.try_query<IEventTrigger>()) {
        wil::unique_bstr subscription;
        RETURN_IF_FAILED(event->get_Subscription(subscription.put()));
        fact.subscription = BstrText(subscription);
      }
    }
    facts.push_back(std::move(fact));
  }
  *result = ClassifyTriggers(facts);
  *settings = TriggerSettingsOf(facts);
  return S_OK;
}

// The first action that runs a program.
HRESULT ReadAction(ITaskDefinition* definition, std::wstring* path, std::wstring* args) {
  path->clear();
  args->clear();
  wil::com_ptr_nothrow<IActionCollection> actions;
  RETURN_IF_FAILED(definition->get_Actions(actions.put()));
  LONG count = 0;
  RETURN_IF_FAILED(actions->get_Count(&count));
  for (LONG index = 1; index <= count && index <= kMaxItems; ++index) {
    wil::com_ptr_nothrow<IAction> action;
    RETURN_IF_FAILED(actions->get_Item(index, action.put()));
    TASK_ACTION_TYPE type = TASK_ACTION_EXEC;
    RETURN_IF_FAILED(action->get_Type(&type));
    if (type != TASK_ACTION_EXEC) continue;
    wil::com_ptr_nothrow<IExecAction> exec;
    RETURN_IF_FAILED(action->QueryInterface(IID_PPV_ARGS(exec.put())));
    wil::unique_bstr text;
    RETURN_IF_FAILED(exec->get_Path(text.put()));
    *path = BstrText(text);
    text.reset();
    RETURN_IF_FAILED(exec->get_Arguments(text.put()));
    *args = BstrText(text);
    return S_OK;
  }
  return S_OK;
}

HRESULT ReadRegisteredTask(IRegisteredTask* task, TaskState* state) {
  state->exists = true;
  VARIANT_BOOL enabled = VARIANT_FALSE;
  RETURN_IF_FAILED(task->get_Enabled(&enabled));
  state->enabled = enabled != VARIANT_FALSE;
  wil::unique_bstr path;
  RETURN_IF_FAILED(task->get_Path(path.put()));
  state->path = BstrText(path);
  // The last run is information only; a task that cannot say keeps both unknown.
  LONG result = 0;
  if (SUCCEEDED(task->get_LastTaskResult(&result))) state->lastResult = result;
  DATE lastRun = 0;
  // A task that never ran reports SCHED_S_TASK_HAS_NOT_RUN (a success code) and a placeholder
  // date.
  if (task->get_LastRunTime(&lastRun) == S_OK && lastRun > 0 &&
      state->lastResult != SCHED_S_TASK_HAS_NOT_RUN) {
    state->lastRun = lastRun;
  }
  wil::com_ptr_nothrow<ITaskDefinition> definition;
  RETURN_IF_FAILED(task->get_Definition(definition.put()));
  TaskTriggers triggers;
  RETURN_IF_FAILED(ReadTriggers(definition.get(), &triggers, &state->options));
  state->options.enabled = state->enabled;
  state->afterPdfCraft = triggers.afterPdfCraft;
  state->atSignIn = triggers.atSignIn;
  RETURN_IF_FAILED(ReadAction(definition.get(), &state->actionPath, &state->actionArgs));
  return S_OK;
}

// The stronger of two settings of the same kind of trigger: enabled over disabled over absent.
TriggerSetting Stronger(TriggerSetting a, TriggerSetting b) noexcept {
  if (a == TriggerSetting::kEnabled || b == TriggerSetting::kEnabled) {
    return TriggerSetting::kEnabled;
  }
  if (a == TriggerSetting::kDisabled || b == TriggerSetting::kDisabled) {
    return TriggerSetting::kDisabled;
  }
  return TriggerSetting::kAbsent;
}

bool IsDenyAce(BYTE type) noexcept {
  return type == ACCESS_DENIED_ACE_TYPE || type == ACCESS_DENIED_OBJECT_ACE_TYPE ||
         type == ACCESS_DENIED_CALLBACK_ACE_TYPE || type == ACCESS_DENIED_CALLBACK_OBJECT_ACE_TYPE;
}

// Opens |location|'s folder as it is (never created) and says where the user's task belongs:
// the folder when it is there and trusted, else the fallback name. |folder| is the folder when it
// could be opened. A folder that is not there or may not be opened is an answer, not a failure.
HRESULT PlaceOfTask(ITaskService* service, const TaskLocation& location,
                    wil::com_ptr_nothrow<ITaskFolder>& folder, TaskPlace* place) {
  *place = TaskPlace::kFallback;
  const HRESULT hr = OpenFolder(service, location.folder, folder);
  if (FAILED(hr) && hr != E_ACCESSDENIED) return hr;
  if (hr != S_OK) {
    folder.reset();
    return S_OK;
  }
  std::wstring sddl;
  if (FAILED(FolderSddl(folder.get(), &sddl))) sddl.clear();  // not trusted
  *place = ChooseTaskPlace(true, sddl);
  return S_OK;
}

// Whether |account| (a SID string, or the account name a hand-made task may hold instead) is the
// account |sid|. May throw std::bad_alloc.
bool IsAccount(std::wstring_view account, std::wstring_view sid) {
  if (account.empty()) return false;
  if (IEquals(account, sid)) return true;
  if (IsSidString(account)) return false;
  BYTE found[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(found);
  wchar_t domain[256];
  DWORD domainChars = ARRAYSIZE(domain);
  SID_NAME_USE use = SidTypeUnknown;
  if (!LookupAccountNameW(nullptr, std::wstring(account).c_str(), found, &size, domain,
                          &domainChars, &use)) {
    return false;  // not an account this computer knows: not the user
  }
  PSID wanted = nullptr;
  if (!ConvertStringSidToSidW(std::wstring(sid).c_str(), &wanted)) return false;
  const wil::unique_hlocal wantedStorage(wanted);
  return EqualSid(found, wanted) != FALSE;
}

// Whether |task| runs as the user |sid|; any task does when |sid| is empty.
HRESULT RunsAsUser(IRegisteredTask* task, std::wstring_view sid, bool* yes) {
  *yes = sid.empty();
  if (*yes) return S_OK;
  wil::unique_bstr xml;
  RETURN_IF_FAILED(task->get_Xml(xml.put()));
  *yes = IsAccount(TaskPrincipalUserId(BstrText(xml)), sid);
  return S_OK;
}

}  // namespace

bool IsSidString(std::wstring_view sid) noexcept {
  constexpr std::wstring_view kStart = L"S-1-";
  if (sid.size() <= kStart.size() || sid.size() > kMaxSidChars || !sid.starts_with(kStart)) {
    return false;
  }
  bool digitBefore = false;  // the part so far is not empty
  for (const wchar_t c : sid.substr(kStart.size())) {
    if (c >= L'0' && c <= L'9') {
      digitBefore = true;
    } else if (c == L'-' && digitBefore) {
      digitBefore = false;
    } else {
      return false;
    }
  }
  return digitBefore;
}

HRESULT DefaultTaskLocation(std::wstring_view sid, TaskLocation* location) noexcept try {
  *location = {};
  if (!IsSidString(sid)) return E_INVALIDARG;
  location->folder = kTaskFolder;
  location->name = std::wstring(kTaskPrefix) + std::wstring(sid);
  location->fallbackFolder = L"\\";
  location->fallbackName = std::wstring(kTaskRootPrefix) + std::wstring(sid);
  location->userSid = sid;
  return S_OK;
}
CATCH_RETURN();

bool SplitTaskPath(std::wstring_view path, std::wstring* folder, std::wstring* name) {
  folder->clear();
  name->clear();
  if (path.size() < 2 || path.front() != L'\\' || path.back() == L'\\') return false;
  const size_t last = path.rfind(L'\\');
  *folder = last == 0 ? std::wstring(L"\\") : std::wstring(path.substr(0, last));
  *name = std::wstring(path.substr(last + 1));
  return true;
}

bool IsTaskNotFound(HRESULT hr) noexcept {
  return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
         hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

bool IsPdfCraftSubscription(std::wstring_view subscription) noexcept {
  try {
    return WithoutSpaceBetweenTags(subscription) == WithoutSpaceBetweenTags(kPdfCraftSubscription);
  } catch (...) {
    return false;  // out of memory: not recognized
  }
}

TaskTriggers ClassifyTriggers(std::span<const TriggerFacts> triggers) noexcept {
  TaskTriggers result;
  for (const TriggerFacts& trigger : triggers) {
    if (!trigger.enabled) continue;
    if (trigger.type == TASK_TRIGGER_LOGON) result.atSignIn = true;
    if (trigger.type == TASK_TRIGGER_EVENT && IsPdfCraftSubscription(trigger.subscription)) {
      result.afterPdfCraft = true;
    }
  }
  return result;
}

TaskOptions OptionsFromCheckboxes(bool afterPdfCraft, bool atSignIn) noexcept {
  TaskOptions options;
  options.afterPdfCraft = afterPdfCraft ? TriggerSetting::kEnabled : TriggerSetting::kAbsent;
  options.atSignIn = atSignIn ? TriggerSetting::kEnabled : TriggerSetting::kAbsent;
  options.enabled = true;
  return options;
}

bool HasNoTriggers(const TaskOptions& options) noexcept {
  return options.afterPdfCraft == TriggerSetting::kAbsent &&
         options.atSignIn == TriggerSetting::kAbsent;
}

TaskOptions TriggerSettingsOf(std::span<const TriggerFacts> triggers) noexcept {
  TaskOptions options{TriggerSetting::kAbsent, TriggerSetting::kAbsent, true};
  for (const TriggerFacts& trigger : triggers) {
    const TriggerSetting setting =
        trigger.enabled ? TriggerSetting::kEnabled : TriggerSetting::kDisabled;
    if (trigger.type == TASK_TRIGGER_LOGON) {
      options.atSignIn = Stronger(options.atSignIn, setting);
    } else if (trigger.type == TASK_TRIGGER_EVENT && IsPdfCraftSubscription(trigger.subscription)) {
      options.afterPdfCraft = Stronger(options.afterPdfCraft, setting);
    }
  }
  return options;
}

std::vector<TriggerSpec> BuildTriggerSpecs(const TaskOptions& options, std::wstring_view account) {
  std::vector<TriggerSpec> specs;
  if (options.atSignIn != TriggerSetting::kAbsent) {
    TriggerSpec& logon = specs.emplace_back();
    logon.type = TASK_TRIGGER_LOGON;
    logon.id = kAtSignInTriggerId;
    logon.delay = kAtSignInDelay;
    logon.userId = account;
    logon.enabled = options.atSignIn == TriggerSetting::kEnabled;
  }
  if (options.afterPdfCraft != TriggerSetting::kAbsent) {
    TriggerSpec& event = specs.emplace_back();
    event.type = TASK_TRIGGER_EVENT;
    event.id = kAfterPdfCraftTriggerId;
    event.delay = kAfterPdfCraftDelay;
    event.subscription = kPdfCraftSubscription;
    event.enabled = options.afterPdfCraft == TriggerSetting::kEnabled;
  }
  return specs;
}

std::wstring TaskPrincipalUserId(std::wstring_view xml) {
  const size_t principals = xml.find(L"<Principals");
  if (principals == std::wstring_view::npos) return {};
  const size_t end = xml.find(L"</Principals>", principals);
  const std::wstring_view section = xml.substr(
      principals, end == std::wstring_view::npos ? std::wstring_view::npos : end - principals);
  constexpr std::wstring_view kOpen = L"<UserId>";
  const size_t open = section.find(kOpen);
  if (open == std::wstring_view::npos) return {};
  const size_t close = section.find(L"</UserId>", open);
  if (close == std::wstring_view::npos) return {};
  std::wstring_view user = section.substr(open + kOpen.size(), close - open - kOpen.size());
  while (!user.empty() && IsXmlSpace(user.front())) user.remove_prefix(1);
  while (!user.empty() && IsXmlSpace(user.back())) user.remove_suffix(1);
  return std::wstring(user);
}

HRESULT ReadTask(const TaskLocation& location, TaskState* state) noexcept try {
  *state = {};
  wil::com_ptr_nothrow<ITaskService> service;
  RETURN_IF_FAILED(ConnectTaskService(service));
  const bool hasFallback = !location.fallbackName.empty();
  // First where ApplyTaskOptions registers: a copy it left in the other place (one this user may
  // not delete) must not hide the one it registered.
  TaskPlace first = TaskPlace::kFolder;
  if (hasFallback) {
    wil::com_ptr_nothrow<ITaskFolder> folder;
    RETURN_IF_FAILED(PlaceOfTask(service.get(), location, folder, &first));
  }
  const TaskPlace second = first == TaskPlace::kFolder ? TaskPlace::kFallback : TaskPlace::kFolder;
  HRESULT refused = S_OK;
  for (const TaskPlace place : {first, second}) {
    const bool atFallback = place == TaskPlace::kFallback;
    if (atFallback && !hasFallback) continue;
    wil::com_ptr_nothrow<IRegisteredTask> task;
    HRESULT hr = atFallback
                     ? OpenTask(service.get(), location.fallbackFolder, location.fallbackName, task)
                     : OpenTask(service.get(), location.folder, location.name, task);
    if (hr == S_OK) {
      // A task under the user's name that runs as someone else was put there by someone else.
      bool users = false;
      if (const HRESULT checked = RunsAsUser(task.get(), location.userSid, &users);
          FAILED(checked)) {
        return checked;
      }
      if (!users) hr = E_ACCESSDENIED;
    }
    // One this user may not open is not theirs either: their own gives them full control.
    if (hr == E_ACCESSDENIED) {
      refused = hr;
      continue;
    }
    if (FAILED(hr)) return hr;
    if (hr == S_FALSE) continue;
    const HRESULT read = ReadRegisteredTask(task.get(), state);
    if (FAILED(read)) {
      *state = {};
      return read;
    }
    state->atFallback = atFallback;
    return S_OK;
  }
  return refused;  // not there (S_OK), or only someone else's
}
CATCH_RETURN();

HRESULT LegacyTaskExists(bool* exists) noexcept try {
  *exists = false;
  std::wstring folder;
  std::wstring name;
  RETURN_HR_IF(E_UNEXPECTED, !SplitTaskPath(kLegacyTask, &folder, &name));
  wil::com_ptr_nothrow<ITaskService> service;
  RETURN_IF_FAILED(ConnectTaskService(service));
  wil::com_ptr_nothrow<IRegisteredTask> task;
  const HRESULT hr = OpenTask(service.get(), folder, name, task);
  if (hr == E_ACCESSDENIED) return S_OK;  // another account's
  RETURN_IF_FAILED(hr);
  *exists = hr == S_OK;
  return S_OK;
}
CATCH_RETURN();

bool IsTrustedTaskFolderSddl(std::wstring_view sddl) noexcept {
  if (sddl.empty()) return false;
  try {
    const std::wstring text(sddl);
    wil::unique_hlocal_security_descriptor descriptor;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1,
                                                              descriptor.put(), nullptr)) {
      return false;
    }
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    if (!GetSecurityDescriptorOwner(descriptor.get(), &owner, &defaulted) || !owner ||
        !IsAdministratorsOrSystem(owner)) {
      return false;
    }
    BOOL present = FALSE;
    PACL dacl = nullptr;
    // No DACL at all would let everyone do everything.
    if (!GetSecurityDescriptorDacl(descriptor.get(), &present, &dacl, &defaulted) || !present ||
        !dacl) {
      return false;
    }
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
      void* ace = nullptr;
      if (!GetAce(dacl, index, &ace)) return false;
      const auto* header = static_cast<const ACE_HEADER*>(ace);
      if (IsDenyAce(header->AceType)) continue;  // takes rights away, never gives any
      if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
      const auto* allowed = static_cast<const ACCESS_ALLOWED_ACE*>(ace);
      PSID trustee = const_cast<DWORD*>(&allowed->SidStart);
      if (IsAdministratorsOrSystem(trustee)) continue;
      // Applies to nothing but what a user creates in the folder, and there to its creator.
      if (IsWellKnownSid(trustee, WinCreatorOwnerSid) && (header->AceFlags & INHERIT_ONLY_ACE)) {
        continue;
      }
      // Anyone else: adding tasks at most, on the folder itself only.
      if ((header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE)) !=
              0 ||
          (allowed->Mask & ~kAddTaskRights) != 0) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;  // out of memory: not trusted
  }
}

TaskPlace ChooseTaskPlace(bool folderExists, std::wstring_view folderSddl) noexcept {
  return folderExists && IsTrustedTaskFolderSddl(folderSddl) ? TaskPlace::kFolder
                                                             : TaskPlace::kFallback;
}

bool IsTopLevelTaskFolder(std::wstring_view folder) noexcept {
  if (folder.size() < 2 || folder.front() != L'\\') return false;
  const std::wstring_view name = folder.substr(1);
  return name.find_first_of(L"\\/") == std::wstring_view::npos && name != L"." && name != L"..";
}

std::wstring TaskDescription() {
  return std::wstring(L"Checks the ") + kProductName + L" registration at sign-in and after " +
         kPdfCraftProductName + L" installs or updates.";
}

HRESULT ApplyTaskOptions(const TaskLocation& location, const TaskOptions& options,
                         const std::filesystem::path& program,
                         const ProcessContext& context) noexcept try {
  if (!IsSidString(context.userSid)) return E_INVALIDARG;
  if (context.elevated) return E_ACCESSDENIED;
  if (HasNoTriggers(options)) {
    const HRESULT hr = DeleteTask(location);
    return FAILED(hr) ? hr : S_OK;
  }
  if (const HRESULT hr = CheckProgram(program); FAILED(hr)) return hr;
  const bool hasFallback = !location.fallbackName.empty();

  wil::com_ptr_nothrow<ITaskService> service;
  RETURN_IF_FAILED(ConnectTaskService(service));
  // The folder, only as it is: never created here.
  wil::com_ptr_nothrow<ITaskFolder> folder;
  TaskPlace place = TaskPlace::kFallback;
  RETURN_IF_FAILED(PlaceOfTask(service.get(), location, folder, &place));
  if (place == TaskPlace::kFallback && !hasFallback) return E_ACCESSDENIED;

  wil::com_ptr_nothrow<ITaskDefinition> definition;
  RETURN_IF_FAILED(DefineTask(service.get(), options, program, context, definition));
  HRESULT hr = S_OK;
  if (place == TaskPlace::kFolder) {
    hr = RegisterIn(folder.get(), location.name, definition.get());
    if (hr == E_ACCESSDENIED && hasFallback) place = TaskPlace::kFallback;
  }
  if (place == TaskPlace::kFallback) {
    wil::com_ptr_nothrow<ITaskFolder> fallback;
    hr = OpenFolder(service.get(), location.fallbackFolder, fallback);
    if (hr == S_FALSE) hr = HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    if (SUCCEEDED(hr)) hr = RegisterIn(fallback.get(), location.fallbackName, definition.get());
  }
  // Task Scheduler registered the task even when some triggers failed: the other copy goes then
  // too.
  if (FAILED(hr) && hr != kSomeTriggersFailed) return hr;
  const HRESULT registered = hr;

  // One copy per user.
  HRESULT other = S_FALSE;
  if (place == TaskPlace::kFolder) {
    if (hasFallback) {
      other = DeleteTaskAt(service.get(), location.fallbackFolder, location.fallbackName);
    }
  } else if (folder) {
    other = DeleteTaskNamed(folder.get(), location.name);
  }
  if (FAILED(registered)) return registered;
  return FAILED(other) ? other : S_OK;
}
CATCH_RETURN();

HRESULT DeleteTask(const TaskLocation& location) noexcept try {
  wil::com_ptr_nothrow<ITaskService> service;
  RETURN_IF_FAILED(ConnectTaskService(service));
  DeleteOutcome outcome;
  outcome.Note(DeleteTaskAt(service.get(), location.folder, location.name));
  if (!location.fallbackName.empty()) {
    outcome.Note(DeleteTaskAt(service.get(), location.fallbackFolder, location.fallbackName));
  }
  return outcome.Result();
}
CATCH_RETURN();

HRESULT EnsureTaskFolder(std::wstring_view folder) noexcept try {
  if (const HRESULT hr = RequireAdministrators(); FAILED(hr)) return hr;
  if (!IsTopLevelTaskFolder(folder)) return E_INVALIDARG;
  wil::com_ptr_nothrow<ITaskService> service;
  RETURN_IF_FAILED(ConnectTaskService(service));
  wil::com_ptr_nothrow<ITaskFolder> root;
  RETURN_IF_FAILED(OpenFolder(service.get(), L"\\", root));
  RETURN_HR_IF(E_UNEXPECTED, !root);
  wil::unique_bstr path;
  RETURN_IF_FAILED(MakeBstr(folder, path));
  wil::unique_variant descriptor;
  descriptor.vt = VT_BSTR;
  descriptor.bstrVal = SysAllocString(kTaskFolderSddl);
  RETURN_IF_NULL_ALLOC(descriptor.bstrVal);

  // Twice: someone else may make the folder between looking and making it.
  for (int attempt = 0; attempt < 2; ++attempt) {
    wil::com_ptr_nothrow<ITaskFolder> existing;
    HRESULT hr = OpenFolder(service.get(), folder, existing);
    if (hr == S_OK) {
      std::wstring sddl;
      if (SUCCEEDED(FolderSddl(existing.get(), &sddl)) && IsTrustedTaskFolderSddl(sddl)) {
        return S_FALSE;
      }
      // Whoever made it, it gets the descriptor it should have had.
      RETURN_IF_FAILED(existing->SetSecurityDescriptor(descriptor.bstrVal, 0));
      return RequireTrusted(existing.get());
    }
    RETURN_IF_FAILED(hr);
    wil::com_ptr_nothrow<ITaskFolder> created;
    hr = root->CreateFolder(path.get(), descriptor, created.put());
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) continue;
    RETURN_IF_FAILED(hr);
    return RequireTrusted(created.get());
  }
  return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}
CATCH_RETURN();

HRESULT DeleteTaskFolderAllUsers(std::wstring_view folder, std::wstring_view rootPrefix) noexcept
    try {
  if (const HRESULT hr = RequireAdministrators(); FAILED(hr)) return hr;
  if (!IsTopLevelTaskFolder(folder) || rootPrefix.empty() ||
      rootPrefix.find(L'\\') != std::wstring_view::npos) {
    return E_INVALIDARG;
  }
  wil::com_ptr_nothrow<ITaskService> service;
  RETURN_IF_FAILED(ConnectTaskService(service));
  wil::com_ptr_nothrow<ITaskFolder> root;
  RETURN_IF_FAILED(OpenFolder(service.get(), L"\\", root));
  RETURN_HR_IF(E_UNEXPECTED, !root);
  DeleteOutcome outcome;

  // Every task in the folder, whoever registered it.
  wil::com_ptr_nothrow<ITaskFolder> taskFolder;
  const HRESULT opened = OpenFolder(service.get(), folder, taskFolder);
  if (opened == S_OK) {
    EmptyFolder(taskFolder.get(), 0, &outcome);
  } else {
    outcome.Note(opened);
  }

  // The tasks that went under the root-folder name instead.
  std::vector<std::wstring> names;
  const HRESULT listed = TaskNames(root.get(), &names);
  outcome.Note(listed);
  if (SUCCEEDED(listed)) {
    for (const std::wstring& name : names) {
      if (IStartsWith(name, rootPrefix) &&
          IsSidString(std::wstring_view(name).substr(rootPrefix.size()))) {
        outcome.Note(DeleteTaskNamed(root.get(), name));
      }
    }
  }

  if (taskFolder) {
    taskFolder.reset();
    wil::unique_bstr path;
    RETURN_IF_FAILED(MakeBstr(folder, path));
    const HRESULT hr = root->DeleteFolder(path.get(), 0);
    outcome.Note(IsTaskNotFound(hr) ? S_FALSE : hr);
  }
  return outcome.Result();
}
CATCH_RETURN();

HRESULT DeleteLegacyTask(std::wstring_view taskPath) noexcept try {
  std::wstring folder;
  std::wstring name;
  if (!SplitTaskPath(taskPath, &folder, &name)) return E_INVALIDARG;
  wil::com_ptr_nothrow<ITaskService> service;
  RETURN_IF_FAILED(ConnectTaskService(service));
  return DeleteTaskAt(service.get(), folder, name);
}
CATCH_RETURN();

}  // namespace pdft
