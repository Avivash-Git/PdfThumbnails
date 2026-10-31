#include "tasks.h"

#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <vector>

#include "project_config.h"

namespace pdft {
namespace {

// More triggers or actions than any task of ours has; a task with more is read only this far.
constexpr LONG kMaxItems = 64;

// The longest SID string Windows produces (SECURITY_MAX_SID_STRING_CHARACTERS).
constexpr size_t kMaxSidChars = 184;

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

// Opens the task |name| in |folder|. S_FALSE (and no task) when the folder or the task does not
// exist, or the folder may not be opened; a task that is there but may not be opened fails with
// E_ACCESSDENIED. No log line for those answers: they are expected.
HRESULT OpenTask(ITaskService* service, std::wstring_view folder, std::wstring_view name,
                 wil::com_ptr_nothrow<IRegisteredTask>& task) {
  task.reset();
  wil::unique_bstr folderPath;
  RETURN_IF_FAILED(MakeBstr(folder, folderPath));
  wil::com_ptr_nothrow<ITaskFolder> taskFolder;
  HRESULT hr = service->GetFolder(folderPath.get(), taskFolder.put());
  if (IsTaskNotFound(hr) || hr == E_ACCESSDENIED) return S_FALSE;
  RETURN_IF_FAILED(hr);
  wil::unique_bstr taskName;
  RETURN_IF_FAILED(MakeBstr(name, taskName));
  hr = taskFolder->GetTask(taskName.get(), task.put());
  if (IsTaskNotFound(hr)) return S_FALSE;
  if (hr == E_ACCESSDENIED) return hr;
  RETURN_IF_FAILED(hr);
  return S_OK;
}

HRESULT ReadTriggers(ITaskDefinition* definition, TaskTriggers* result) {
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
  RETURN_IF_FAILED(ReadTriggers(definition.get(), &triggers));
  state->afterPdfCraft = triggers.afterPdfCraft;
  state->atSignIn = triggers.atSignIn;
  RETURN_IF_FAILED(ReadAction(definition.get(), &state->actionPath, &state->actionArgs));
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

HRESULT ReadTask(const TaskLocation& location, TaskState* state) noexcept try {
  *state = {};
  wil::com_ptr_nothrow<ITaskService> service;
  RETURN_IF_FAILED(ConnectTaskService(service));
  wil::com_ptr_nothrow<IRegisteredTask> task;
  HRESULT hr = OpenTask(service.get(), location.folder, location.name, task);
  if (FAILED(hr)) return hr;
  if (hr == S_FALSE && !location.fallbackName.empty()) {
    hr = OpenTask(service.get(), location.fallbackFolder, location.fallbackName, task);
    if (FAILED(hr)) return hr;
    state->atFallback = hr == S_OK;
  }
  if (hr == S_FALSE) return S_OK;  // not there
  const HRESULT read = ReadRegisteredTask(task.get(), state);
  if (FAILED(read)) {
    *state = {};
    return read;
  }
  return S_OK;
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

}  // namespace pdft
