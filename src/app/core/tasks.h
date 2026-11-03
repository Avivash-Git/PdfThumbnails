// The scheduled check task (Task Scheduler 2.0): one task per user that runs the check at sign-in
// and after PdfCraft installs or updates, and the 1.x task it replaces.
//
//   \<Product>\Check <user SID>     the task, in a folder of its own (one per user, so users never
//                                   collide, and an uninstall can remove them all with the folder)
//   \<Product> check <user SID>     the same task in the root folder, where it goes when the
//                                   folder is not there or cannot be trusted
//   \PdfThumbnails auto-repair      1.x's task (one name for the whole PC)
//
// The task's two triggers are what the settings window shows as its two checkboxes: an event
// trigger with PdfCraft's Windows Installer subscription ("after PdfCraft updates") and a logon
// trigger ("at sign-in"). Both are recognized by their kind and content, not by trigger IDs,
// so a task registered by hand or by an older build reads the same.
//
// The folder is shared by every account, so whoever creates it controls it: a standard user who
// made it would own it and could rewrite its permissions, and the tasks other users put in it
// would inherit them. Only elevated code creates it (EnsureTaskFolder), with a fixed protected
// security descriptor. Unelevated code never creates it and uses it only while its owner and
// permissions are the safe ones; otherwise the user's task goes under the root-folder name.
// Either way a user has one copy of the task: registering it in one place deletes their copy in
// the other.
//
// Both names are known to every account, and anyone may add tasks to the folder and the root
// folder, so another user can take a name first. Such a task is never registered over, nor
// deleted as the user's other copy: the user's task goes to the other place, and with both names
// taken it has none until an administrator removes those tasks (kTaskTakenHr).
//
// Every function needs COM initialized on the calling thread. Reading changes nothing; a folder
// or task that does not exist is an answer ("not there"), not a failure.

#pragma once

#include <windows.h>

#include <oleauto.h>
#include <taskschd.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "context.h"
#include "project_config.h"

namespace pdft {

// Where a task lives: its folder ("\\<Product>") and name, plus where to look when it is not
// there (the root-folder name of the same task; empty |fallbackName| = nowhere else), and whose
// task it is (|userSid|: a task there that runs as another account is not theirs; empty = any
// account's, for reading a task by its path). Tests use a folder of their own.
struct TaskLocation {
  std::wstring folder;
  std::wstring name;
  std::wstring fallbackFolder;
  std::wstring fallbackName;
  std::wstring userSid;
};

// "S-1-" followed by numbers separated by dashes (pure): the only kind of text that becomes
// part of a task name.
bool IsSidString(std::wstring_view sid) noexcept;

// The check task of the user |sid|: folder kTaskFolder, name kTaskPrefix + SID, fallback
// kTaskRootPrefix + SID in the root folder, user |sid|. E_INVALIDARG when |sid| is not
// IsSidString.
[[nodiscard]] HRESULT DefaultTaskLocation(std::wstring_view sid, TaskLocation* location) noexcept;

// A task path ("\\folder\\name" or "\\name") split into its folder ("\\folder", "\\") and name
// (pure). False when |path| does not start with a backslash or ends with one. May throw
// std::bad_alloc.
bool SplitTaskPath(std::wstring_view path, std::wstring* folder, std::wstring* name);

// Task Scheduler's answer for a folder or task that does not exist (pure).
bool IsTaskNotFound(HRESULT hr) noexcept;

// Another account's task holds every name the user's check task may be registered under:
// ApplyTaskOptions never registers over such a task, and ReadTask finds only those. Task
// Scheduler lets only that account or an administrator change or delete them, so the user has no
// check task until an administrator removes them. A code of this app's own (FACILITY_ITF), not a
// Windows error, so that no call on the way can return it.
inline constexpr HRESULT kTaskTakenHr = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x2E8);

// ---- Triggers -----------------------------------------------------------------------------

// What the classification needs to know about one trigger.
struct TriggerFacts {
  TASK_TRIGGER_TYPE2 type = TASK_TRIGGER_EVENT;
  bool enabled = true;
  std::wstring subscription;  // event triggers: the event query
};

// The two checkbox states a task's triggers stand for.
struct TaskTriggers {
  bool afterPdfCraft = false;
  bool atSignIn = false;
};

// True when |subscription| is PdfCraft's event query (kPdfCraftSubscription), compared exactly
// but for white space between XML tags and at either end, which tools that rewrite the XML may
// add (pure). The query itself is case-sensitive and stays so.
bool IsPdfCraftSubscription(std::wstring_view subscription) noexcept;

// The checkbox states (pure): afterPdfCraft from an enabled event trigger whose subscription is
// PdfCraft's, atSignIn from an enabled logon trigger (of any user: the task is the user's own,
// and a logon trigger without a user fires at theirs too). Disabled triggers never fire and do
// not count; other kinds of triggers are ignored.
TaskTriggers ClassifyTriggers(std::span<const TriggerFacts> triggers) noexcept;

// The arguments the check task runs the settings executable with.
inline constexpr wchar_t kCheckTaskArguments[] = L"--check --quiet";

// What a trigger of the task is, as registered or to be registered.
enum class TriggerSetting {
  kAbsent,    // no such trigger: the checkbox is off
  kEnabled,   // there and enabled: the checkbox is on
  kDisabled,  // there but disabled (someone turned it off in Task Scheduler): it never fires,
              // the checkbox shows off, and registering the task again with the same triggers
              // keeps it so
};

// The task as ApplyTaskOptions registers it: its two triggers and whether the task itself is
// enabled.
struct TaskOptions {
  TriggerSetting afterPdfCraft = TriggerSetting::kEnabled;
  TriggerSetting atSignIn = TriggerSetting::kEnabled;
  bool enabled = true;
};

// The options for two checkbox states (pure): a checked box is an enabled trigger, an unchecked
// one no trigger, and the task is enabled.
TaskOptions OptionsFromCheckboxes(bool afterPdfCraft, bool atSignIn) noexcept;

// True when |options| has no trigger at all (pure): ApplyTaskOptions deletes the task then.
bool HasNoTriggers(const TaskOptions& options) noexcept;

// The setting of each kind of trigger (pure), recognized as ClassifyTriggers recognizes them:
// enabled when an enabled trigger of that kind is there, disabled when only disabled ones are,
// absent otherwise. |enabled| of the result is true; the caller sets it from the task.
TaskOptions TriggerSettingsOf(std::span<const TriggerFacts> triggers) noexcept;

// What the registered triggers carry.
inline constexpr wchar_t kAtSignInTriggerId[] = L"AtSignIn";
inline constexpr wchar_t kAfterPdfCraftTriggerId[] = L"AfterPdfCraftInstall";
// The wait between the event and the check (ISO 8601 durations): a minute after sign-in, so the
// shell is up; half a minute after PdfCraft's installer logs its event, so it has finished.
inline constexpr wchar_t kAtSignInDelay[] = L"PT1M";
inline constexpr wchar_t kAfterPdfCraftDelay[] = L"PT30S";

// One trigger to register.
struct TriggerSpec {
  TASK_TRIGGER_TYPE2 type = TASK_TRIGGER_LOGON;
  std::wstring id;
  std::wstring delay;
  std::wstring userId;        // logon trigger: the account whose sign-in fires it
  std::wstring subscription;  // event trigger: the event query
  bool enabled = true;
};

// The triggers |options| asks for (pure), the logon trigger first: a logon trigger for
// |account| ("DOMAIN\\user", or the user's SID string, which Task Scheduler stores as the same
// account) and PdfCraft's event trigger, each with its ID and delay. May throw std::bad_alloc.
std::vector<TriggerSpec> BuildTriggerSpecs(const TaskOptions& options, std::wstring_view account);

// ---- Reading ------------------------------------------------------------------------------

struct TaskState {
  bool exists = false;
  bool enabled = false;
  bool afterPdfCraft = false;  // ClassifyTriggers
  bool atSignIn = false;
  bool atFallback = false;  // found under the fallback name, not in the folder
  std::wstring path;        // the task's full path, as Task Scheduler reports it
  // The program and arguments of the task's first program action; empty when it runs none.
  std::wstring actionPath;
  std::wstring actionArgs;
  std::optional<DATE> lastRun;  // std::nullopt when it never ran
  std::optional<LONG> lastResult;
  // Each trigger as it is, disabled ones included, and whether the task is enabled: what
  // registering the task again with the same triggers passes to ApplyTaskOptions. No triggers
  // and not enabled when there is no task.
  TaskOptions options{TriggerSetting::kAbsent, TriggerSetting::kAbsent, false};
};

// Reads the user's task at |location|. Read-only. With a fallback name both places are read,
// first the one ApplyTaskOptions registers in (the folder when ChooseTaskPlace says so, else the
// fallback name), so a copy it could not delete in the other place never hides the one it
// registered. A copy is the user's only when this user may open it and, with |userSid| set, it
// runs as that user (Task Scheduler lets only the user or an administrator register a task that
// runs as them); anyone else's is skipped. A missing folder, a folder this user may not open, or
// a missing task gives exists = false; when the only copies are someone else's, the result is
// E_ACCESSDENIED, or kTaskTakenHr when, with |userSid| set, they hold every name ApplyTaskOptions
// may register the task under (the folder's when it registers there first, and the fallback
// name), so registering it would fail too. A task that is there but cannot be read otherwise
// fails.
[[nodiscard]] HRESULT ReadTask(const TaskLocation& location, TaskState* state) noexcept;

// The account a task runs as (pure): the contents of the UserId element of the principal in the
// task's XML (as IRegisteredTask::get_Xml gives it, which stores the account as a SID string),
// without white space at either end. Empty when the principal names no user (a group, or no
// principal at all). May throw std::bad_alloc.
std::wstring TaskPrincipalUserId(std::wstring_view xml);

// Whether 1.x's task (kLegacyTask) exists and this user can see it. One that belongs to another
// account (Task Scheduler refuses to open it) is not this user's leftover and counts as absent.
[[nodiscard]] HRESULT LegacyTaskExists(bool* exists) noexcept;

// ---- The shared folder --------------------------------------------------------------------

// The security descriptor EnsureTaskFolder gives the folder, protected from the root folder's:
// owned by Administrators; Administrators and SYSTEM have full control of the folder, and each
// task in it inherits their rights to tasks (0x1f019f, which leaves out only execute and
// delete-child, neither of which means anything for a task); every signed-in user may add tasks
// to the folder and read its security descriptor, but not list, read, change or delete what is
// in it, and that right is not inherited; each task gives full control to the account that
// registered it. Task Scheduler also gives the task's own user read access to it.
inline constexpr wchar_t kTaskFolderSddl[] =
    L"O:BAG:SYD:P(A;CI;FA;;;BA)(A;OI;0x1f019f;;;BA)(A;CI;FA;;;SY)(A;OI;0x1f019f;;;SY)"
    L"(A;;FW;;;AU)(A;OIIO;FA;;;CO)";

// Whether a folder whose security descriptor is |sddl| (as ITaskFolder::GetSecurityDescriptor
// gives it, with at least the owner and the DACL) is safe for a user's task (pure). The owner
// must be Administrators or SYSTEM, and the DACL must be there and allow nothing to anyone else
// but adding tasks (file write rights at most, on the folder only, never inherited), except an
// inherit-only creator-owner entry, which gives each task's registering account its own task.
// Deny entries are fine. kTaskFolderSddl is trusted; the root folder's descriptor, and that of a
// folder a standard user created, are not. False when |sddl| cannot be parsed.
bool IsTrustedTaskFolderSddl(std::wstring_view sddl) noexcept;

// Where ApplyTaskOptions registers the task.
enum class TaskPlace {
  kFolder,    // TaskLocation::folder and name
  kFallback,  // TaskLocation::fallbackFolder and fallbackName
};

// The place for the task (pure): the folder when it exists and IsTrustedTaskFolderSddl
// (|folderSddl|, empty when it could not be read), otherwise the fallback.
TaskPlace ChooseTaskPlace(bool folderExists, std::wstring_view folderSddl) noexcept;

// A top-level task folder path, "\\name" (pure): what EnsureTaskFolder and
// DeleteTaskFolderAllUsers accept.
bool IsTopLevelTaskFolder(std::wstring_view folder) noexcept;

// ---- Writing ------------------------------------------------------------------------------

// The check task's description, which Task Scheduler shows. May throw std::bad_alloc.
std::wstring TaskDescription();

// SCHED_S_SOME_TRIGGERS_FAILED (a success code) as a failure.
inline constexpr HRESULT kSomeTriggersFailed = static_cast<HRESULT>(0x8004131BL);

// Registers (creates or updates) the check task of |context|'s user with |options|; no trigger
// at all deletes it instead (DeleteTask, and S_FALSE becomes S_OK). The task runs |program| with
// kCheckTaskArguments, as that user with their interactive token and without elevation.
//   - Place: the folder when ChooseTaskPlace says so; otherwise, or when Task Scheduler refuses
//     the folder or another account's task holds the name there, the fallback name. The folder is
//     never created here. Afterwards the user's own copy in the other place is deleted, so the
//     user has one task; anyone else's there stays.
//   - Never over another account's task: a name is registered anew (TASK_CREATE) when no task has
//     it, so a task someone makes meanwhile makes the call fail rather than be replaced, and
//     updated only when the task there is the user's own (one this user may open that runs as
//     the context's user). Another account's task at the last place left (the fallback name, or
//     the folder's without one) gives kTaskTakenHr, and nothing changes.
//   - Refused: an elevated |context| (E_ACCESSDENIED: the task belongs to the signed-in user,
//     whom an elevated process may not even be), a context user that is not a SID
//     (E_INVALIDARG), a |program| that is not an absolute path or holds "%" or a quote, which
//     Task Scheduler would change (E_INVALIDARG), or that is not an existing file
//     (ERROR_FILE_NOT_FOUND). Which program is the right one is the caller's decision; this only
//     keeps a task from pointing at nothing.
//   - Task Scheduler's "some triggers failed" comes back as kSomeTriggersFailed; the task stays
//     registered as Task Scheduler left it.
[[nodiscard]] HRESULT ApplyTaskOptions(const TaskLocation& location, const TaskOptions& options,
                                       const std::filesystem::path& program,
                                       const ProcessContext& context) noexcept;

// Deletes the task at |location| in both places (the folder, then the fallback name). S_FALSE
// when neither had it; a copy this user may not delete is not theirs and is skipped. Never
// deletes a folder.
[[nodiscard]] HRESULT DeleteTask(const TaskLocation& location) noexcept;

// Elevated only (E_ACCESSDENIED unless Administrators is enabled in this process's token): makes
// the top-level folder |folder| (E_INVALIDARG unless IsTopLevelTaskFolder) with kTaskFolderSddl.
// One that is there already is kept when IsTrustedTaskFolderSddl, else given kTaskFolderSddl.
// The result is read back: a folder that still is not trusted fails with
// ERROR_INVALID_SECURITY_DESCR. S_OK when it was made or its descriptor replaced, S_FALSE when it
// was there and trusted already.
[[nodiscard]] HRESULT EnsureTaskFolder(std::wstring_view folder) noexcept;

// Elevated only, like EnsureTaskFolder: removes every user's check task. Deletes every task in
// |folder| and its subfolders (hidden ones too), then the root-folder tasks named |rootPrefix|
// followed by a SID string, then the subfolders and |folder| itself. Goes on after a failure and
// returns the first one; S_FALSE when there was nothing to delete. E_INVALIDARG unless
// IsTopLevelTaskFolder(|folder|) and |rootPrefix| is a name start without a backslash.
[[nodiscard]] HRESULT DeleteTaskFolderAllUsers(std::wstring_view folder,
                                               std::wstring_view rootPrefix) noexcept;

// Deletes 1.x's task, or the task at |taskPath| (tests). S_FALSE when it is not there, or
// belongs to another account (Task Scheduler refuses to open it), as LegacyTaskExists counts it.
[[nodiscard]] HRESULT DeleteLegacyTask(std::wstring_view taskPath = kLegacyTask) noexcept;

}  // namespace pdft
