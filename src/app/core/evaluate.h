// Judging a snapshot (pure): the findings, each with how bad it is, whether it keeps .pdf
// thumbnails from working, and what can fix it. The status, the check's repairs and its exit code
// all follow from them.
//
// Findings carry their data in typed fields (slot, CLSID, HRESULT, counts, versions); the text
// shown for them is the status formatter's business.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "exit_code.h"
#include "snapshot.h"

namespace pdft {

enum class Severity { kInfo, kWarning, kError };

// What can fix a finding.
enum class FixKind {
  kNone,        // nothing to do, or nothing this app can do
  kAuto,        // the check repairs it (per user, no prompt)
  kTakeBack,    // only on request: the slot belongs to another handler (--fix --take-back)
  kNeedsAdmin,  // the machine-wide part: an administrator step
  kUserAction,  // the user has to act (reinstall, change a Windows setting, ...)
};

enum class FindingId {
  kNotInstalled,             // no class, and no per-user build to register
  kClassMissingUser,         // no class, but a complete per-user build is there: register it
  kClassDamagedUser,         // the per-user class's DLL or PDFium is missing
  kClassDamagedMachine,      // the machine-wide class's DLL or PDFium is missing, or it points
                             // outside the machine-wide build folders
  kUserClassShadowsMachine,  // a per-user class hides the machine-wide one
  kThreadingModel,           // the class's ThreadingModel is not Apartment
  kDllArchMismatch,          // the DLL is built for another architecture than Windows
  kSlotEmpty,                // a slot is empty
  kSlotOtherHandler,         // a slot names another handler
  kEffectiveOther,           // Windows resolves another handler (or none) for .pdf
  kDefaultAppHandler,        // the default app's ProgID brings its own thumbnail handler
  kLiveTestFailed,           // the Explorer test failed
  kPerUserClassIgnored,      // ... with "class not registered" for a per-user install
  kLiveTestSkipped,          // the Explorer test did not run (a warning where the command needs it)
  kTestFileFailed,           // --test-file: no thumbnail, or the test could not start
  kTestFileSkipped,          // --test-file: not run here
  kSacBlocked,               // Smart App Control blocks the DLLs
  kSacEvaluation,            // Smart App Control is in evaluation mode
  kSacOn,                    // Smart App Control is on (sac says what is known)
  kSacUnknown,               // Smart App Control's state value is unknown
  kIconsOnly,                // Explorer shows icons only
  kThumbnailsDisabledByPolicy,
  kChecksOff,        // no check task
  kTaskStaleAction,  // the task runs another program than the installed settings executable
                     // (repaired only while that program is there)
  kTaskDisabled,     // the task is disabled
  kCleanupPending,   // a sign-in clean-up (RunOnce) would delete a folder at the next sign-in, or
                     // (information) files in use wait in a trash folder for the next restart
  kV1Leftovers,      // 1.x left files, settings, its task or its classes
  kOldBuildFolders,  // per-user 2.x build folders no class uses
  kZoomInvalid,      // the Zoom value has the wrong type or is out of range
  kPdfCraftChanged,  // PdfCraft's version differs from the last recorded check's
  kElevated,         // this process is elevated
  kDifferentUser,    // this process runs as another user than the desktop's
  kVirtualized,      // this process may not see Explorer's registry
};

// The finding's name as the JSON output and the log spell it ("SlotEmpty").
const wchar_t* FindingName(FindingId id) noexcept;

struct Finding {
  FindingId id = FindingId::kNotInstalled;
  Severity severity = Severity::kInfo;
  bool breaksThumbnails = false;
  FixKind fix = FixKind::kNone;
  Scope scope = Scope::kNone;  // kThreadingModel, kDllArchMismatch: whose class
  int slot = -1;               // kSlotEmpty, kSlotOtherHandler
  // kSlotOtherHandler, kDefaultAppHandler: the other handler; kEffectiveOther: what Windows
  // resolves ("" when nothing).
  std::wstring clsid;
  // kLiveTestFailed, kPerUserClassIgnored, kTestFileFailed: the result (for a test file that could
  // not start, why); kSacBlocked: the load-blocked error when no blocks were counted;
  // kLiveTestSkipped: why it could not start.
  std::optional<HRESULT> hr;
  int count = 0;  // kSacBlocked: logged blocks; kOldBuildFolders: how many; kCleanupPending: trash
                  // folders waiting for the restart (when no sign-in clean-up is pending)
  LiveTestSkip liveSkip = LiveTestSkip::kNone;              // kLiveTestSkipped, kTestFileSkipped
  WindowsChecksSkip checksSkip = WindowsChecksSkip::kNone;  // kVirtualized
  SacVerdictKind sac = SacVerdictKind::kNone;               // the kSac* findings
  std::wstring from, to;                                    // kPdfCraftChanged
  int zoom = 0;  // kZoomInvalid: the value a repair writes (the zoom in effect, clamped)
  // kClassMissingUser, kClassDamagedUser, kThreadingModel (per user): the DLL a repaired class
  // points at; kTaskStaleAction: the program the task should run; kClassDamagedMachine: the DLL
  // the class names when that is outside the machine-wide build folders (empty otherwise).
  std::filesystem::path path;
  std::vector<std::filesystem::path> paths;  // kOldBuildFolders: the folders
};

// The findings, in the order of the table they come from (pure). May throw std::bad_alloc.
std::vector<Finding> Evaluate(const Snapshot& snapshot);

// The per-user class's DLL after a repair (pure): the registered one when it is a complete 2.x
// per-user build for this Windows, else ChooseUserBuildDll. std::nullopt when there is none.
std::optional<std::filesystem::path> UserClassTarget(const Snapshot& snapshot);

// The check task |task| runs |program| with the check's arguments (pure): the path compared
// ignoring case and the quotes some tools add around it, the arguments exactly.
bool TaskRunsProgram(const TaskState& task, const std::filesystem::path& program) noexcept;

// Healthy: no finding keeps thumbnails from working (pure).
bool HealthyFromFindings(std::span<const Finding> findings) noexcept;

// The Explorer test did not run where the command needed its answer (pure): a
// LiveTestSkipped finding that is a warning (Snapshot::liveRequired), not information.
// Nothing is known to keep thumbnails from working, but nothing confirms that they work.
bool LiveTestUnconfirmed(std::span<const Finding> findings) noexcept;

// The exit code the findings call for (pure): 6 when not installed, else 1 when a finding keeps
// thumbnails from working or the Explorer test the command needed did not run
// (LiveTestUnconfirmed), else 0. The check adds 4, 7 and 8 (check.h).
ExitCode ExitCodeFromFindings(std::span<const Finding> findings) noexcept;

// Whether |findings| contains |id| (pure).
bool HasFinding(std::span<const Finding> findings, FindingId id) noexcept;

}  // namespace pdft
