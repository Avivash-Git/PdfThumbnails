// When the scheduled checks run: --checks registers the signed-in user's check task with the
// triggers asked for, or deletes it.
//
//   1. Refuse when this process's registry writes may not reach the real hive (exit 8), like
//      every command that writes, and in an elevated process (exit 8): the task belongs to the
//      signed-in user and runs with their own rights, and an elevated process may not even be
//      that user.
//   2. Take the operation lock (at most 30 s; exit 4 when another check or change keeps it, 5
//      when Windows does not let this process make it).
//   3. Gather the state, without the Explorer test. Not installed: exit 6, nothing changed.
//   4. Registering needs the installed settings executable (InstalledSettingsExe): an install
//      without it, such as a 1.x build, exits 6 and the task is left as it is, so a task never
//      points at a program that is not there, nor at a build tree. Turning the checks off needs
//      no program: deleting the user's task is right whatever is installed.
//   5. Register or delete (ApplyTaskOptions: the shared folder when it is there and safe, else
//      the root-folder name; one copy either way), then read the task again for the report. A
//      change that fails exits 5. One log line, without the user's SID.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>

#include "check.h"
#include "context.h"
#include "exit_code.h"
#include "registry_roots.h"
#include "snapshot.h"
#include "system_probes.h"
#include "tasks.h"

namespace pdft {

struct ScheduleOptions {
  bool afterPdfCraft = true;  // --checks pdfcraft
  bool atSignIn = true;       // --checks signin
  // The context the command read with the canary; when absent, probes.GetProcessContext with
  // ContextProbe::kWrite.
  std::optional<ProcessContext> context;
  std::optional<EnginePaths> paths;     // when absent, GetEnginePaths
  DWORD lockWaitMs = kCheckLockWaitMs;  // never more than kCheckLockWaitMs
};

// Why the task was left as it was.
enum class ScheduleRefusal {
  kNone,
  kWrongContext,   // exit 8: the registry may be virtualized, or the context is not the canary's
  kElevated,       // exit 8
  kNotInstalled,   // exit 6
  kNoSettingsExe,  // exit 6: installed, but not with the program the task would run
};

struct ScheduleResult {
  ExitCode code = ExitCode::kFailed;
  ScheduleRefusal refusal = ScheduleRefusal::kNone;
  bool busy = false;  // exit 4: the lock stayed taken
  // exit 5: Windows does not let this process make the lock (kNamedObjectsRefusedHr)
  bool namedObjectsRefused = false;
  ProcessContext context;
  Snapshot before;                  // the state gathered first (empty when refused or busy)
  bool askedAfterPdfCraft = false;  // the checkboxes asked for
  bool askedAtSignIn = false;
  bool remove = false;            // the checks were turned off: delete the task
  std::filesystem::path program;  // registering: what the task runs
  HRESULT error = S_OK;           // the change failed (exit 5)
  TaskState after;                // the task afterwards
  HRESULT readError = S_OK;       // ... could not be read (the change itself is done)
};

// Changes the check task as |options| asks. S_OK when it came to a verdict (*result's code,
// also for 4, 5, 6 and 8); a failure when the state could not be read (code kFailed). Never
// throws.
[[nodiscard]] HRESULT ChangeCheckSchedule(const ScheduleOptions& options,
                                          const RegistryRoots& roots, const SystemProbes& probes,
                                          ScheduleResult* result) noexcept;

}  // namespace pdft
