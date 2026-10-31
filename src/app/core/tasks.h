// The scheduled check task (Task Scheduler 2.0): one task per user that runs the check at sign-in
// and after PdfCraft installs or updates, and the 1.x task it replaces.
//
//   \<Product>\Check <user SID>     the task, in a folder of its own (one per user, so users never
//                                   collide, and an uninstall can remove them all with the folder)
//   \<Product> check <user SID>     the same task in the root folder, where it goes when the
//                                   folder cannot be made
//   \PdfThumbnails auto-repair      1.x's task (one name for the whole PC)
//
// The task's two triggers are what the settings window shows as its two checkboxes: an event
// trigger with PdfCraft's Windows Installer subscription ("after PdfCraft updates") and a logon
// trigger ("at sign-in"). Both are recognized by their kind and content, not by trigger IDs,
// so a task registered by hand or by an older build reads the same.
//
// Every function needs COM initialized on the calling thread. Reading changes nothing; a folder
// or task that does not exist is an answer ("not there"), not a failure.

#pragma once

#include <windows.h>

#include <oleauto.h>
#include <taskschd.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace pdft {

// Where a task lives: its folder ("\\<Product>") and name, plus where to look when it is not
// there (the root-folder name of the same task; empty |fallbackName| = nowhere else). Tests use
// a folder of their own.
struct TaskLocation {
  std::wstring folder;
  std::wstring name;
  std::wstring fallbackFolder;
  std::wstring fallbackName;
};

// "S-1-" followed by numbers separated by dashes (pure): the only kind of text that becomes
// part of a task name.
bool IsSidString(std::wstring_view sid) noexcept;

// The check task of the user |sid|: folder kTaskFolder, name kTaskPrefix + SID, fallback
// kTaskRootPrefix + SID in the root folder. E_INVALIDARG when |sid| is not IsSidString.
[[nodiscard]] HRESULT DefaultTaskLocation(std::wstring_view sid, TaskLocation* location) noexcept;

// A task path ("\\folder\\name" or "\\name") split into its folder ("\\folder", "\\") and name
// (pure). False when |path| does not start with a backslash or ends with one. May throw
// std::bad_alloc.
bool SplitTaskPath(std::wstring_view path, std::wstring* folder, std::wstring* name);

// Task Scheduler's answer for a folder or task that does not exist (pure).
bool IsTaskNotFound(HRESULT hr) noexcept;

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
};

// Reads the task at |location| (then at its fallback). Read-only. A missing folder, a folder this
// user may not open, or a missing task gives exists = false; a task that is there but cannot be
// read fails.
[[nodiscard]] HRESULT ReadTask(const TaskLocation& location, TaskState* state) noexcept;

// Whether 1.x's task (kLegacyTask) exists and this user can see it. One that belongs to another
// account (Task Scheduler refuses to open it) is not this user's leftover and counts as absent.
[[nodiscard]] HRESULT LegacyTaskExists(bool* exists) noexcept;

}  // namespace pdft
