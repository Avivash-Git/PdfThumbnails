#include "evaluate.h"

#include <algorithm>
#include <utility>

#include "pe_info.h"
#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

Finding Make(FindingId id, Severity severity, bool breaks, FixKind fix) {
  Finding finding;
  finding.id = id;
  finding.severity = severity;
  finding.breaksThumbnails = breaks;
  finding.fix = fix;
  return finding;
}

// The DLL and PDFium are both where the class says. 1.x checked the same before calling a class
// healthy; ThreadingModel has a finding of its own.
bool ClassFilesPresent(const ClassReg& reg) noexcept { return reg.dllExists && reg.pdfiumExists; }

bool IsComplete2xBuild(const BuildFolder& folder) noexcept {
  return folder.complete && !folder.is1x && !folder.version.empty();
}

// The program path as a task stores it, without the quotes some tools add.
std::wstring_view Unquoted(std::wstring_view path) noexcept {
  if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') {
    return path.substr(1, path.size() - 2);
  }
  return path;
}

// The machine-wide class points into a build folder of a machine-wide install, the DLL directly
// in it: <machineRoot>\app\<build folder name>\, or a 1.x build folder under 1.x's machine-wide
// root (an install that 2.0 has not replaced yet). Anywhere else, every user's File Explorer may
// load a DLL from a folder that is not protected like Program Files.
bool MachineClassInBuildFolder(const Snapshot& s) {
  const std::filesystem::path dll = std::filesystem::path(s.machineClass->dll).lexically_normal();
  const std::filesystem::path name = dll.parent_path().filename();
  if (name.empty()) return false;
  const std::optional<BuildFolderNameParts> parts = ParseBuildFolderName(name.native());
  if (!parts) return false;
  const auto inFolderOf = [&](const std::filesystem::path& root) {
    return !root.empty() && DllIsInFolder(dll, root / kBuildsFolderName / name);
  };
  const bool is1x = parts->version == L"1" || parts->version.starts_with(L"1.");
  return inFolderOf(s.paths.machineRoot) || (is1x && inFolderOf(s.paths.legacyMachineRoot));
}

// Per-user 2.x build folders that neither class uses and that a repair will not point the
// per-user class at. Folders a class uses stay even when the repair moves the class elsewhere:
// they are removed by the next check, once nothing points at them.
std::vector<std::filesystem::path> OldUserBuilds(
    const Snapshot& snapshot, const std::optional<std::filesystem::path>& target) {
  std::vector<std::filesystem::path> old;
  for (const BuildInfo& build : snapshot.userBuilds) {
    const BuildFolder& folder = build.folder;
    if (folder.is1x || folder.version.empty()) continue;  // 1.x and odd names are not ours
    const auto uses = [&](const std::optional<ClassReg>& reg) {
      return reg && DllIsInFolder(reg->dll, folder.dir);
    };
    if (uses(snapshot.userClass) || uses(snapshot.machineClass)) continue;
    if (target && DllIsInFolder(*target, folder.dir)) continue;
    old.push_back(folder.dir);
  }
  return old;
}

}  // namespace

const wchar_t* FindingName(FindingId id) noexcept {
  switch (id) {
    case FindingId::kNotInstalled:
      return L"NotInstalled";
    case FindingId::kClassMissingUser:
      return L"ClassMissingUser";
    case FindingId::kClassDamagedUser:
      return L"ClassDamagedUser";
    case FindingId::kClassDamagedMachine:
      return L"ClassDamagedMachine";
    case FindingId::kUserClassShadowsMachine:
      return L"UserClassShadowsMachine";
    case FindingId::kThreadingModel:
      return L"ThreadingModel";
    case FindingId::kDllArchMismatch:
      return L"DllArchMismatch";
    case FindingId::kSlotEmpty:
      return L"SlotEmpty";
    case FindingId::kSlotOtherHandler:
      return L"SlotOtherHandler";
    case FindingId::kEffectiveOther:
      return L"EffectiveOther";
    case FindingId::kDefaultAppHandler:
      return L"DefaultAppHandler";
    case FindingId::kLiveTestFailed:
      return L"LiveTestFailed";
    case FindingId::kPerUserClassIgnored:
      return L"PerUserClassIgnored";
    case FindingId::kLiveTestSkipped:
      return L"LiveTestSkipped";
    case FindingId::kTestFileFailed:
      return L"TestFileFailed";
    case FindingId::kTestFileSkipped:
      return L"TestFileSkipped";
    case FindingId::kSacBlocked:
      return L"SacBlocked";
    case FindingId::kSacEvaluation:
      return L"SacEvaluation";
    case FindingId::kSacOn:
      return L"SacOn";
    case FindingId::kSacUnknown:
      return L"SacUnknown";
    case FindingId::kIconsOnly:
      return L"IconsOnly";
    case FindingId::kThumbnailsDisabledByPolicy:
      return L"ThumbnailsDisabledByPolicy";
    case FindingId::kChecksOff:
      return L"ChecksOff";
    case FindingId::kTaskStaleAction:
      return L"TaskStaleAction";
    case FindingId::kTaskDisabled:
      return L"TaskDisabled";
    case FindingId::kCleanupPending:
      return L"CleanupPending";
    case FindingId::kV1Leftovers:
      return L"V1Leftovers";
    case FindingId::kOldBuildFolders:
      return L"OldBuildFolders";
    case FindingId::kZoomInvalid:
      return L"ZoomInvalid";
    case FindingId::kPdfCraftChanged:
      return L"PdfCraftChanged";
    case FindingId::kElevated:
      return L"Elevated";
    case FindingId::kDifferentUser:
      return L"DifferentUser";
    case FindingId::kVirtualized:
      return L"Virtualized";
  }
  return L"Unknown";
}

std::optional<std::filesystem::path> UserClassTarget(const Snapshot& snapshot) {
  if (snapshot.userClass && ClassFilesPresent(*snapshot.userClass)) {
    for (const BuildInfo& build : snapshot.userBuilds) {
      if (IsComplete2xBuild(build.folder) && build.dllMachine &&
          snapshot.context.osArch != Arch::kOther &&
          MachineToArch(*build.dllMachine) == snapshot.context.osArch &&
          DllIsInFolder(snapshot.userClass->dll, build.folder.dir)) {
        return snapshot.userClass->dll;
      }
    }
  }
  return ChooseUserBuildDll(snapshot);
}

std::vector<Finding> Evaluate(const Snapshot& s) {
  std::vector<Finding> findings;
  const auto add = [&](Finding finding) -> Finding& {
    findings.push_back(std::move(finding));
    return findings.back();
  };

  // ---- Is it installed, and is the class sound? ----
  const std::optional<std::filesystem::path> newestBuild = ChooseUserBuildDll(s);
  bool installed = true;
  std::optional<std::filesystem::path> registerTarget;  // where a repaired per-user class points
  if (s.scope == Scope::kNone) {
    if (newestBuild) {
      // A per-user install whose class went missing: 1.x's repair registered the build again.
      registerTarget = newestBuild;
      add(Make(FindingId::kClassMissingUser, Severity::kError, true, FixKind::kAuto)).path =
          *newestBuild;
    } else {
      installed = false;
      add(Make(FindingId::kNotInstalled, Severity::kError, true, FixKind::kUserAction));
    }
  } else if (s.scope == Scope::kUser) {
    if (!ClassFilesPresent(*s.userClass)) {
      if (newestBuild) registerTarget = newestBuild;
      Finding& finding = add(Make(FindingId::kClassDamagedUser, Severity::kError, true,
                                  newestBuild ? FixKind::kAuto : FixKind::kUserAction));
      if (newestBuild) finding.path = *newestBuild;
    }
  } else {
    const bool filesPresent = ClassFilesPresent(*s.machineClass);
    const bool inBuildFolder = MachineClassInBuildFolder(s);
    const bool machineOk = filesPresent && inBuildFolder;
    if (!machineOk) {
      Finding& finding =
          add(Make(FindingId::kClassDamagedMachine, Severity::kError, true, FixKind::kNeedsAdmin));
      if (!inBuildFolder) finding.path = s.machineClass->dll;
    }
    if (s.userClass && !s.windowsChecksSkipped) {
      // 1.x counted this against health; it removed the per-user class only once the
      // machine-wide one was sound, and so does this. A per-user class of this version over a
      // machine-wide 1.x class (a per-user install forced over it) stays until this version is
      // installed for all users.
      const bool forcedOverV1 = s.legacy.machineClassInV1 && !s.legacy.userClassInV1;
      add(Make(FindingId::kUserClassShadowsMachine, Severity::kWarning, true,
               machineOk && !forcedOverV1 ? FixKind::kAuto : FixKind::kNone));
    }
  }

  if (const ClassReg* active = s.ActiveClass()) {
    if (!IEquals(active->threading, kApartmentThreading)) {
      Finding finding = Make(FindingId::kThreadingModel, Severity::kError, true, FixKind::kNone);
      finding.scope = s.scope;
      if (s.scope == Scope::kMachine) {
        finding.fix = FixKind::kNeedsAdmin;
      } else {
        // Rewriting the class rewrites its threading model; it points at the build a repair
        // would choose anyway.
        if (!registerTarget) registerTarget = UserClassTarget(s);
        finding.fix = registerTarget ? FixKind::kAuto : FixKind::kUserAction;
        if (registerTarget) finding.path = *registerTarget;
      }
      add(std::move(finding));
    }
    if (active->dllExists && active->peMachine && s.context.osArch != Arch::kOther &&
        MachineToArch(*active->peMachine) != s.context.osArch) {
      Finding finding =
          Make(FindingId::kDllArchMismatch, Severity::kError, true, FixKind::kUserAction);
      finding.scope = s.scope;
      add(std::move(finding));
    }
  }

  // ---- The slots and what Windows makes of them ----
  // The slots are read in-process: when this process may see a private copy of the registry, what
  // it reads is not what Explorer reads, and they are not judged (the Virtualized finding says
  // why). The same goes for a per-user class hiding the machine-wide one, above.
  if (installed && !s.windowsChecksSkipped) {
    for (size_t slot = 0; slot < kSlotCount; ++slot) {
      const std::optional<std::wstring>& value = s.slots[slot];
      if (SlotIsEmpty(value)) {
        add(Make(FindingId::kSlotEmpty, Severity::kError, true, FixKind::kAuto)).slot =
            static_cast<int>(slot);
      } else if (!IsOurClsid(*value)) {
        // The owner rule: another handler may be the user's choice; only an explicit request
        // takes the slot back.
        Finding& finding =
            add(Make(FindingId::kSlotOtherHandler, Severity::kError, true, FixKind::kTakeBack));
        finding.slot = static_cast<int>(slot);
        finding.clsid = *value;
      }
    }
    if (!s.EffectiveIsOurs()) {
      add(Make(FindingId::kEffectiveOther, Severity::kError, true, FixKind::kNone)).clsid =
          s.effective.value_or(L"");
    }
  }
  if (!s.windowsChecksSkipped && !SlotIsEmpty(s.defaultApp.handler) &&
      !IsOurClsid(*s.defaultApp.handler)) {
    // Shown, as 1.x did, but whether it wins is what the effective handler says.
    add(Make(FindingId::kDefaultAppHandler, Severity::kWarning, false, FixKind::kNone)).clsid =
        *s.defaultApp.handler;
  }

  // ---- The Explorer test ----
  if (installed) {
    if (s.live) {
      if (s.live->hr == REGDB_E_CLASSNOTREG && s.scope == Scope::kUser) {
        // Explorer's thumbnail process does not load per-user classes on current Windows
        // builds; only a machine-wide install helps.
        add(Make(FindingId::kPerUserClassIgnored, Severity::kError, true, FixKind::kUserAction))
            .hr = s.live->hr;
      } else if (s.live->hr != S_OK) {
        add(Make(FindingId::kLiveTestFailed, Severity::kError, true, FixKind::kNone)).hr =
            s.live->hr;
      }
    } else {
      // Not run: skipped (liveSkip says why), or due but it could not start (liveError). Either
      // says nothing about File Explorer. Where the command needs the answer, its result is left
      // unconfirmed: a warning, which the exit code counts.
      Finding& finding =
          add(Make(FindingId::kLiveTestSkipped,
                   s.liveRequired ? Severity::kWarning : Severity::kInfo, false, FixKind::kNone));
      finding.liveSkip = s.liveSkip;
      if (FAILED(s.liveError)) finding.hr = s.liveError;
    }
  }
  if (s.testFile && s.testFile->problem == TestFileProblem::kNone) {
    const TestFileRun& run = *s.testFile;
    if (run.result && run.result->hr != S_OK) {
      // 1.x counted a test file without a thumbnail against health.
      add(Make(FindingId::kTestFileFailed, Severity::kError, true, FixKind::kNone)).hr =
          run.result->hr;
    } else if (run.skip != LiveTestSkip::kNone) {
      add(Make(FindingId::kTestFileSkipped, Severity::kInfo, false, FixKind::kNone)).liveSkip =
          run.skip;
    } else if (!run.result) {
      // The test could not even start: no thumbnail either, and 1.x counted that too.
      add(Make(FindingId::kTestFileFailed, Severity::kError, true, FixKind::kNone)).hr =
          FAILED(run.error) ? run.error : E_FAIL;
    }
  }

  // ---- Smart App Control ----
  switch (s.sacVerdict.kind) {
    case SacVerdictKind::kBlocked: {
      Finding& finding =
          add(Make(FindingId::kSacBlocked, Severity::kError, true, FixKind::kUserAction));
      finding.sac = s.sacVerdict.kind;
      finding.count = s.sacVerdict.blocks;
      finding.hr = s.sacVerdict.evidence;
      break;
    }
    case SacVerdictKind::kEval:
      add(Make(FindingId::kSacEvaluation, Severity::kInfo, false, FixKind::kNone)).sac =
          s.sacVerdict.kind;
      break;
    case SacVerdictKind::kOk:
    case SacVerdictKind::kOn:
    case SacVerdictKind::kOnUnknown:
      add(Make(FindingId::kSacOn, Severity::kInfo, false, FixKind::kNone)).sac = s.sacVerdict.kind;
      break;
    case SacVerdictKind::kUnknown:
      add(Make(FindingId::kSacUnknown, Severity::kInfo, false, FixKind::kNone)).sac =
          s.sacVerdict.kind;
      break;
    case SacVerdictKind::kNone:
      break;
  }

  // ---- Explorer settings: they hide thumbnails, but 1.x did not count them against health ----
  if (s.explorer.iconsOnly) {
    add(Make(FindingId::kIconsOnly, Severity::kWarning, false, FixKind::kUserAction));
  }
  if (s.ThumbnailsDisabledByPolicy()) {
    add(Make(FindingId::kThumbnailsDisabledByPolicy, Severity::kWarning, false,
             FixKind::kUserAction));
  }

  // ---- The check task ----
  if (installed) {
    if (!s.task.exists) {
      add(Make(FindingId::kChecksOff, Severity::kInfo, false, FixKind::kUserAction));
    } else {
      if (!s.installRoot.empty()) {
        // Registered again only for a program that is there: never a task that points at
        // nothing (an install without the settings executable needs reinstalling first).
        const std::optional<std::filesystem::path> program = InstalledSettingsExe(s);
        const std::filesystem::path expected = program.value_or(s.installRoot / kSettingsExe);
        if (!TaskRunsProgram(s.task, expected)) {
          // Registered again "with the same triggers" only when it has one of the check's own:
          // with neither, that would delete it, and --checks picks the triggers instead.
          const FixKind fix =
              program && !HasNoTriggers(s.task.options) ? FixKind::kAuto : FixKind::kUserAction;
          add(Make(FindingId::kTaskStaleAction, Severity::kWarning, false, fix)).path = expected;
        }
      }
      if (!s.task.enabled) {
        add(Make(FindingId::kTaskDisabled, Severity::kWarning, false, FixKind::kUserAction));
      }
    }
  }

  // ---- Leftovers and settings ----
  if (installed && (s.legacy.runOnce || s.legacy.runOnceOthers)) {
    // While installed, a pending sign-in clean-up (1.x's, or one an uninstall left) would delete
    // a folder of the install at the next sign-in, a per-user install included; the check itself
    // removes what is left over while the program is there.
    add(Make(FindingId::kCleanupPending, Severity::kWarning, false, FixKind::kAuto));
  } else if (installed && !s.pendingTrash.empty()) {
    // Files an install or clean-up for all users could not delete because they were in use wait
    // in trash folders for the next restart. Nothing to repair and nothing broken: information.
    Finding& finding =
        add(Make(FindingId::kCleanupPending, Severity::kInfo, false, FixKind::kNone));
    finding.count = static_cast<int>(s.pendingTrash.size());
  }
  if (HasV1Leftovers(s.legacy)) {
    // Removing them belongs to the 1.x clean-up (--migrate-v1, or installing). Once it has run for
    // this user, with this version installed, the check finishes what it left: files that were
    // in use then. It never starts the clean-up on its own.
    const bool finish = s.migratedFromV1 && !s.legacy.files.empty() && InstalledSettingsExe(s);
    add(Make(FindingId::kV1Leftovers, Severity::kInfo, false,
             finish ? FixKind::kAuto : FixKind::kNone));
  }
  if (installed) {
    std::vector<std::filesystem::path> old = OldUserBuilds(s, registerTarget);
    if (!old.empty()) {
      Finding& finding =
          add(Make(FindingId::kOldBuildFolders, Severity::kInfo, false, FixKind::kAuto));
      finding.count = static_cast<int>(old.size());
      finding.paths = std::move(old);
    }
    if (IsZoomValueInvalid(s.zoom.decision.registry)) {
      // The zoom in effect, so rewriting the value changes nothing the user sees.
      add(Make(FindingId::kZoomInvalid, Severity::kWarning, false, FixKind::kAuto)).zoom =
          s.zoom.decision.zoom;
    }
  }
  const std::wstring pdfcraftNow = s.pdfcraft.installed ? s.pdfcraft.version : std::wstring();
  if (s.last && s.last->pdfcraftVersion != pdfcraftNow) {
    Finding& finding =
        add(Make(FindingId::kPdfCraftChanged, Severity::kInfo, false, FixKind::kNone));
    finding.from = s.last->pdfcraftVersion;
    finding.to = pdfcraftNow;
  }

  // ---- Where this runs ----
  if (s.context.elevated) {
    add(Make(FindingId::kElevated, Severity::kInfo, false, FixKind::kNone));
  }
  if (s.context.differentFromShellUser) {
    add(Make(FindingId::kDifferentUser, Severity::kWarning, false, FixKind::kNone));
  }
  if (s.windowsChecksSkipped) {
    add(Make(FindingId::kVirtualized, Severity::kWarning, false, FixKind::kNone)).checksSkip =
        s.windowsChecksSkip;
  }
  return findings;
}

bool TaskRunsProgram(const TaskState& task, const std::filesystem::path& program) noexcept {
  return IEquals(Unquoted(task.actionPath), program.native()) &&
         task.actionArgs == kCheckTaskArguments;
}

bool HealthyFromFindings(std::span<const Finding> findings) noexcept {
  return std::none_of(findings.begin(), findings.end(),
                      [](const Finding& finding) { return finding.breaksThumbnails; });
}

bool LiveTestUnconfirmed(std::span<const Finding> findings) noexcept {
  return std::any_of(findings.begin(), findings.end(), [](const Finding& finding) {
    return finding.id == FindingId::kLiveTestSkipped && finding.severity != Severity::kInfo;
  });
}

ExitCode ExitCodeFromFindings(std::span<const Finding> findings) noexcept {
  if (HasFinding(findings, FindingId::kNotInstalled)) return ExitCode::kNotInstalled;
  if (!HealthyFromFindings(findings) || LiveTestUnconfirmed(findings)) return ExitCode::kProblems;
  return ExitCode::kOk;
}

bool HasFinding(std::span<const Finding> findings, FindingId id) noexcept {
  return std::any_of(findings.begin(), findings.end(),
                     [id](const Finding& finding) { return finding.id == id; });
}

}  // namespace pdft
