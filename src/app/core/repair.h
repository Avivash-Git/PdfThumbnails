// Repairs: the plan the findings call for (pure), and applying its per-user steps.
//
// What a check repairs on its own is safe: it fills empty slots but never takes one from another
// handler (the user may have chosen it), registers the per-user class again, removes a per-user
// class that hides the machine-wide one, removes 1.x's pending clean-up while installed, rewrites
// a broken zoom value, registers the check task again when it runs another program than the
// installed one, and removes per-user build folders nothing uses. Taking slots back needs an
// explicit request. The machine-wide part needs an administrator step, which the plan names but
// this code never runs.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

#include "evaluate.h"
#include "registry_roots.h"
#include "snapshot.h"
#include "system_probes.h"

namespace pdft {

enum class RepairMode {
  kNone,             // report only
  kSafe,             // what a check does on its own
  kSafeAndTakeBack,  // also take back slots from other handlers
};

struct RepairStep {
  enum class Kind {
    kDeleteRunOnce,      // 1.x's pending clean-up value
    kRegisterUserClass,  // the per-user class -> |path| (the handler DLL)
    kDeleteUserClass,    // the per-user class that hides the machine-wide one
    kFillSlot,           // an empty slot -> our handler
    kTakeBackSlot,       // a slot of another handler -> our handler
    kRewriteZoom,        // the Zoom value -> |zoom|
    kReRegisterTask,     // the task again, same triggers, the installed program |path|
    kRemoveUserBuild,    // the per-user build folder |path|
  };
  Kind kind = Kind::kFillSlot;
  int slot = -1;
  std::filesystem::path path;
  int zoom = 0;
};

// The step's name for the log ("fill-slot").
const wchar_t* RepairStepName(RepairStep::Kind kind) noexcept;

// The administrator steps a repair may need.
enum class ElevatedVerb {
  kMachineInstall,
  kMachineRepairClass,  // point the machine-wide class at a complete build again
  kMachineUninstall,
  kMachineMigrateV1,
};

struct RepairPlan {
  std::vector<RepairStep> user;         // in the order they are applied
  std::optional<ElevatedVerb> machine;  // the one administrator step that would help, if any
};

// The plan (pure): per-user steps for the findings this |mode| repairs, and the administrator
// step that would fix the machine-wide part (named for every mode but kNone, never run here).
// The order: the pending clean-up first (it would delete the per-user folder), then the class,
// the slots, the zoom and the task, and build folders last, once the class points where it
// stays. Nothing for a product that is not installed. May throw std::bad_alloc.
RepairPlan PlanRepairs(const Snapshot& snapshot, std::span<const Finding> findings,
                       RepairMode mode);

// What became of one step.
enum class RepairResult {
  kDone,
  kUnchanged,   // nothing to do any more (someone else did it meanwhile, or it changed)
  kKept,        // a build folder still in use: left for the next check
  kNotApplied,  // not done here, for the reason RepairOutcome::notApplied gives
  kFailed,
};

// Why a step was not applied.
enum class NotApplied {
  kNone,
  kElevated,   // an elevated process neither deletes in the user's profile nor registers the
               // user's task: a normal check does
  kNoProgram,  // the installed settings executable is not there (any more): a task must never
               // point at a program that is missing
  kNoTrigger,  // the task has neither of the check's triggers: registering it again with the
               // same triggers would delete it
};

struct RepairOutcome {
  RepairStep step;
  RepairResult result = RepairResult::kFailed;
  HRESULT hr = S_OK;
  NotApplied notApplied = NotApplied::kNone;  // with RepairResult::kNotApplied
};

// Applies |plan|'s per-user steps through |roots| (and the check task through |probes|), each
// checked again against the registry, the files and the task as they are now, and logged (step,
// slot or folder name, result; no paths). A step that fails is recorded in |outcomes| and the
// others still run; nothing throws. Fails only when out of memory. The administrator step is
// never run. The task is registered again only unelevated, only for the installed settings
// executable while it is there (InstalledSettingsExe), and with the triggers and the enabled
// state it has now, so what someone turned off stays off.
[[nodiscard]] HRESULT ApplyUserRepairs(const RegistryRoots& roots, const SystemProbes& probes,
                                       const Snapshot& snapshot, const RepairPlan& plan,
                                       std::vector<RepairOutcome>* outcomes) noexcept;

// Whether any outcome changed a slot or a class (Explorer must then be told; pure).
bool ChangedSlotOrClass(std::span<const RepairOutcome> outcomes) noexcept;

// How many steps were done, and whether any failed (pure).
int CountDone(std::span<const RepairOutcome> outcomes) noexcept;
bool AnyFailed(std::span<const RepairOutcome> outcomes) noexcept;

// ---- Removing a build folder ----------------------------------------------------------------

// Removes the 2.x build folder |dir| of the per-user install root |userRoot|: it must be
// <userRoot>\app\<build folder name> with a 2.x version, and neither |userRoot|, its app folder
// nor |dir| may be a link or junction (so a planted link never leads the deletion out of the
// root); links found inside are removed themselves, never followed. The handler DLL goes first:
// while Explorer's thumbnail process has it loaded that fails, and the folder stays complete
// (*kept = true) for the next check to retry. E_INVALIDARG for a folder that does not qualify.
[[nodiscard]] HRESULT RemoveUserBuildFolder(const std::filesystem::path& userRoot,
                                            const std::filesystem::path& dir, bool* kept) noexcept;

// An error that means "in use" (pure): access denied, a sharing violation, a mapped file.
bool IsInUseError(HRESULT hr) noexcept;

}  // namespace pdft
