// The check: --check, --fix, the scheduled task and the settings window's "Check now" all run it.
//
//   1. Refuse when this process's registry writes may not reach the real hive (exit 8): the
//      context must come from the canary (ContextProbe::kWrite) and rule virtualization out.
//      Refuse an elevated process too (exit 8): the repairs are per-user, and an administrator
//      process does not write into the user's profile (nor, elevated as another account, into
//      the wrong one).
//   2. Take the operation lock (at most 30 s; exit 4 when another check or change keeps it).
//   3. Gather (snapshot.h), evaluate (evaluate.h); not installed: exit 6, nothing written.
//   4. Plan and apply the per-user repairs (repair.h). The administrator step the plan names runs
//      only for --fix (a launcher is given and the check is not quiet): through UAC, with the
//      settings executable installed for all users (elevation.h); declined: exit 3, started
//      without administrator rights: 7, failed: 5.
//   5. When something changed, gather and evaluate again.
//   6. Record the check under HKCU State, tell Explorer when a slot or class changed (only for the
//      real registry roots), and write one log line.
//   7. Exit 0 when nothing keeps thumbnails from working, else 1; a quiet check that needs the
//      administrator step exits 7.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "elevation.h"
#include "evaluate.h"
#include "exit_code.h"
#include "registry_roots.h"
#include "repair.h"
#include "snapshot.h"
#include "system_probes.h"

namespace pdft {

// How long a check waits for the operation lock, at most.
inline constexpr DWORD kCheckLockWaitMs = 30000;

// The wait for the lock that a check asking for |requested| milliseconds gets (pure): never more
// than kCheckLockWaitMs, so a scheduled check cannot hang on a lock that stays taken.
constexpr DWORD EffectiveLockWaitMs(DWORD requested) noexcept {
  return requested < kCheckLockWaitMs ? requested : kCheckLockWaitMs;
}

struct CheckOptions {
  RepairMode repair = RepairMode::kSafe;
  bool liveTest = true;
  // A scheduled check (--check --quiet): nobody can answer an administrator prompt, so a needed
  // administrator step makes it exit 7.
  bool quiet = false;
  std::optional<std::filesystem::path> testFile;
  // The context the command read with the canary; when absent, probes.GetProcessContext with
  // ContextProbe::kWrite.
  std::optional<ProcessContext> context;
  std::optional<EnginePaths> paths;     // when absent, GetEnginePaths
  DWORD lockWaitMs = kCheckLockWaitMs;  // never more than kCheckLockWaitMs
  // --fix: starts the administrator step the plan names (never for a quiet check). Empty: the
  // step is only named. Tests pass a fake, so no test ever starts a real elevation.
  ElevationLauncher launcher;
  HWND owner = nullptr;  // the window that asks, if any
};

struct CheckResult {
  ExitCode code = ExitCode::kFailed;
  bool refused = false;          // exit 8: nothing was read (see refusedElevated)
  bool refusedElevated = false;  // ... because this process is elevated, not because the
                                 // registry may be virtualized
  bool busy = false;             // exit 4: the lock stayed taken
  bool lockAbandoned = false;    // the previous holder ended without releasing the lock
  std::vector<Finding> before;   // what the first gathering found
  RepairPlan plan;
  std::vector<RepairOutcome> repairs;
  Snapshot after;                 // the state after the repairs (the first one if none ran)
  std::vector<Finding> findings;  // ... and what it shows
  // The machine-wide part needs the administrator step plan.machine, which did not run (or did
  // not succeed).
  bool adminStepNeeded = false;
  // ... and could not have run here: no settings executable is installed for all users to run it.
  bool adminStepUnavailable = false;
  std::optional<ElevatedRun> elevated;  // the administrator step, when it was started
  bool recorded = false;
  HRESULT recordError = S_OK;
  bool notified = false;  // Explorer was told that associations changed
  std::wstring summary;   // what the record says
};

// Runs the check. S_OK when it came to a verdict (*result's code; also for 4, 6, 7 and 8); a
// failure when the state could not be read (code kFailed). Never throws.
[[nodiscard]] HRESULT RunCheck(const CheckOptions& options, const RegistryRoots& roots,
                               const SystemProbes& probes, CheckResult* result) noexcept;

// The exit code of a check (pure): |findings| after the repairs, whether a repair step failed,
// whether the machine-wide part needs the administrator step, and whether the check is quiet.
ExitCode CheckExitCode(std::span<const Finding> findings, bool repairFailed, bool adminStepNeeded,
                       bool quiet) noexcept;

// The one-line summary the record keeps (pure): "nothing to fix", "fixed 2", "2 problems
// remain; fixed 1", ... Names no user data. May throw std::bad_alloc.
std::wstring CheckSummary(ExitCode code, int fixed, int problems);

// The log's word for an exit code ("healthy", "problems", "busy", ...).
const wchar_t* CheckResultName(ExitCode code) noexcept;

}  // namespace pdft
