#include "status_format.h"

#include <oleauto.h>

#include <algorithm>
#include <cwchar>
#include <optional>
#include <utility>

#include "file_ops.h"
#include "pe_info.h"
#include "project_config.h"
#include "registration.h"
#include "strings.h"

namespace pdft {
namespace {

// Separates the parts of one line: a middle dot between spaces.
constexpr wchar_t kDot[] = L" \u00B7 ";

// The width of the label column; longer labels get two spaces after them.
constexpr size_t kLabelWidth = 15;
// Where the text (and a hint below it) starts: "  OK " plus the label column.
constexpr size_t kTextColumn = 5 + kLabelWidth;

constexpr wchar_t kLabelInstall[] = L"Install";
constexpr wchar_t kLabelHandler[] = L"Handler";
constexpr wchar_t kLabelPdfium[] = L"PDFium";
constexpr wchar_t kLabelSlot[] = L".pdf slot";
constexpr wchar_t kLabelEffective[] = L"Effective";
constexpr wchar_t kLabelDefaultApp[] = L"Default app";
constexpr wchar_t kLabelExplorerTest[] = L"Explorer test";
constexpr wchar_t kLabelTestFile[] = L"Test file";
constexpr wchar_t kLabelZoom[] = L"Zoom";
constexpr wchar_t kLabelChecks[] = L"Checks";
constexpr wchar_t kLabelSac[] = L"Smart App Control";
constexpr wchar_t kLabelUserClass[] = L"Per-user class";
constexpr wchar_t kLabelExplorer[] = L"Explorer";
constexpr wchar_t kLabelPolicy[] = L"Policy";
constexpr wchar_t kLabelCleanup[] = L"1.x clean-up";
constexpr wchar_t kLabelRestart[] = L"Restart";
constexpr wchar_t kLabelLeftovers[] = L"1.x leftovers";
constexpr wchar_t kLabelOldBuilds[] = L"Old builds";
constexpr wchar_t kLabelAccount[] = L"Account";
constexpr wchar_t kLabelNote[] = L"Note";

const wchar_t* const kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                  L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};

std::wstring Hex(HRESULT hr) { return HResultText(hr); }

std::wstring Plural(int n, const wchar_t* one, const wchar_t* many) {
  return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

const Finding* Find(std::span<const Finding> findings, FindingId id) {
  for (const Finding& finding : findings) {
    if (finding.id == id) return &finding;
  }
  return nullptr;
}

std::wstring ScopeWord(Scope scope) {
  return scope == Scope::kMachine ? L"machine-wide" : L"per-user";
}

const ClassReg* ClassOf(const Snapshot& s, Scope scope) {
  if (scope == Scope::kMachine) return s.machineClass ? &*s.machineClass : nullptr;
  if (scope == Scope::kUser) return s.userClass ? &*s.userClass : nullptr;
  return nullptr;
}

// What a CLSID is called: the product for ours, else the registry's name for it, as 1.x did.
std::wstring HandlerName(std::wstring_view clsid, const ClsidDescription& description) {
  if (IsOurClsid(clsid)) return kProductName;
  if (description.name && !description.name->empty()) return *description.name;
  return L"unknown handler";
}

// "{...} (Some viewer)"
std::wstring Described(std::wstring_view clsid, const ClsidDescription& description) {
  return std::wstring(clsid) + L" (" + HandlerName(clsid, description) + L")";
}

std::wstring OsArchName(const Snapshot& s) { return ArchName(s.context.osArch); }

std::wstring DllArchName(const ClassReg& reg) {
  return reg.peMachine ? ArchName(MachineToArch(*reg.peMachine)) : L"architecture unknown";
}

std::wstring SkipReason(LiveTestSkip skip) {
  switch (skip) {
    case LiveTestSkip::kNone:
      return L"";
    case LiveTestSkip::kNotRequested:
      return L"not requested (--no-live-test)";
    case LiveTestSkip::kElevated:
      return L"this process is elevated, File Explorer is not; run --status from a normal prompt";
    case LiveTestSkip::kVirtualized:
      return L"this process may not see the registry File Explorer sees";
    case LiveTestSkip::kNotOurHandler:
      return L"Windows resolves .pdf thumbnails to another handler";
    case LiveTestSkip::kNoClass:
      return L"no class is registered";
  }
  return L"";
}

std::wstring ChecksSkipReason(WindowsChecksSkip skip) {
  switch (skip) {
    case WindowsChecksSkip::kNone:
      return L"";
    case WindowsChecksSkip::kViewsDiffer:
      return L"this process sees a private copy of the registry (it runs inside an app package)";
    case WindowsChecksSkip::kViewsUnknown:
      return L"it could not be confirmed that this process sees the real registry";
    case WindowsChecksSkip::kVirtualized:
      return L"this process's registry is virtualized";
    case WindowsChecksSkip::kNotChecked:
      return L"this process runs inside an app package, and its registry view was not checked";
  }
  return L"";
}

// The Explorer test's error in words (1.x's texts where 1.x had one).
std::wstring LiveFailureText(HRESULT hr) {
  if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
    return L"no answer from File Explorer's thumbnail process within " +
           std::to_wstring(kLiveTestTimeoutMs / 1000) + L" seconds";
  }
  if (IsLoadBlockedHresult(hr)) return L"Windows refused to load the DLL, " + Hex(hr);
  if (hr == REGDB_E_CLASSNOTREG) return Hex(hr) + L": the COM class is not registered";
  return L"extraction failed: " + Hex(hr);
}

std::wstring SacText(const Snapshot& s) {
  const SacVerdict& verdict = s.sacVerdict;
  switch (verdict.kind) {
    case SacVerdictKind::kNone:
      return L"";
    case SacVerdictKind::kEval:
      return L"in evaluation mode; if Windows switches it ON, it may block these unsigned DLLs";
    case SacVerdictKind::kUnknown:
      return L"state unknown (" + SacValueText(s.sac) + L")";
    case SacVerdictKind::kOk:
      return std::wstring(L"On; not blocking ") + kProductName + L" (the Explorer test passed)";
    case SacVerdictKind::kBlocked: {
      std::wstring text = std::wstring(L"On and blocking ") + kProductName + L" (";
      if (verdict.blocks > 0) {
        text += L"Code Integrity blocked the installed DLLs " + std::to_wstring(verdict.blocks) +
                L" time(s) in the last " + std::to_wstring(kSacBlockDays) + L" days)";
      } else {
        text +=
            L"Windows refused to load the DLL, " + Hex(verdict.evidence.value_or(E_FAIL)) + L")";
      }
      return text;
    }
    case SacVerdictKind::kOn:
      // Blocks are counted only when a class is registered and its DLL is there: without a count
      // there is nothing to say about them.
      if (s.ActiveClass() == nullptr || !s.blocks) return L"On";
      return L"On; Code Integrity logged no blocks of the installed DLLs in the last " +
             std::to_wstring(kSacBlockDays) + L" days";
    case SacVerdictKind::kOnUnknown:
      return L"On; the Code Integrity log could not be read";
  }
  return L"";
}

std::wstring PdfCraftText(const Snapshot& s) {
  const std::wstring name = kPdfCraftProductName;
  if (!s.pdfcraft.installed) return name + L" not found";
  return name + L" " + (s.pdfcraft.version.empty() ? L"(version unknown)" : s.pdfcraft.version);
}

std::wstring VersionOrNone(const std::wstring& version) {
  return version.empty() ? std::wstring(L"none") : version;
}

std::wstring ZoomOriginText(ZoomOrigin origin) {
  switch (origin) {
    case ZoomOrigin::kRegistry:
      return L"registry";
    case ZoomOrigin::kLegacyIni:
      return L"1.x settings.ini";
    case ZoomOrigin::kDefault:
      return L"default";
  }
  return L"default";
}

std::wstring RegistryTypeName(DWORD type) {
  switch (type) {
    case REG_SZ:
      return L"REG_SZ";
    case REG_EXPAND_SZ:
      return L"REG_EXPAND_SZ";
    case REG_BINARY:
      return L"REG_BINARY";
    case REG_DWORD:
      return L"REG_DWORD";
    case REG_DWORD_BIG_ENDIAN:
      return L"REG_DWORD_BIG_ENDIAN";
    case REG_MULTI_SZ:
      return L"REG_MULTI_SZ";
    case REG_QWORD:
      return L"REG_QWORD";
    default:
      return L"type " + std::to_wstring(type);
  }
}

// 1.x's handler reads only its settings.ini, so a zoom saved in the registry does not reach it.
bool ActiveHandlerIs1x(const Snapshot& s) {
  const ClassReg* active = s.ActiveClass();
  return active && (active->fileVersion == L"1" || active->fileVersion.starts_with(L"1."));
}

std::wstring Joined(const std::vector<std::wstring>& parts, std::wstring_view separator) {
  std::wstring text;
  for (const std::wstring& part : parts) {
    if (part.empty()) continue;
    if (!text.empty()) text += separator;
    text += part;
  }
  return text;
}

// How to install for all users. Developer builds also install from a build's own folder.
std::wstring InstallCommandHint() {
  return kDevInstall ? L"the installer, choosing all users (or, in a developer build, "
                       L"--install --machine from the build's folder)"
                     : L"the installer, choosing all users";
}

// How "--checks <value>" does |what|, where it can: only with the installed settings executable
// there for the task to run (a 1.x build has none, and --checks then refuses), and from a normal
// prompt (it refuses an elevated one). Empty where --checks cannot help.
std::wstring ChecksCommandHint(const Snapshot& s, std::wstring_view value, std::wstring_view what) {
  if (!InstalledSettingsExe(s)) return L"";
  std::wstring hint = L"--checks " + std::wstring(value) + L" " + std::wstring(what);
  if (s.context.elevated) hint += L" (from a normal, not administrator, prompt)";
  return hint;
}

// Whether another account's tasks hold every name the user's check task may have: the gathering
// keeps ReadTask's answer, kTaskTakenHr, among its issues.
bool TaskNamesTaken(const Snapshot& s) {
  return std::any_of(s.issues.begin(), s.issues.end(),
                     [](const GatherIssue& issue) { return issue.hr == kTaskTakenHr; });
}

// What the Checks line suggests while another account's tasks hold the names: an administrator
// removes them, then --checks turns the checks on where it can.
std::wstring TaskTakenChecksHint(const Snapshot& s) {
  const std::wstring then = ChecksCommandHint(s, L"default", L"turns them on");
  return TaskTakenHint(s.taskLocation) + (then.empty() ? std::wstring() : L"; then " + then);
}

// A task's path as Task Scheduler shows it: "\folder\name", or "\name" in the root folder.
std::wstring TaskPathText(std::wstring_view folder, std::wstring_view name) {
  std::wstring path(folder);
  if (!path.ends_with(L'\\')) path += L'\\';
  return path + std::wstring(name);
}

// A new install for all users: what a machine-wide part needs that --fix cannot repair.
std::wstring ReinstallForAllUsersHint() {
  return L"Reinstall " + std::wstring(kProductName) +
         L" for all users (needs administrator rights)";
}

// What a damaged machine-wide class needs: --fix runs the administrator step that points it at
// the newest complete build for all users, where one is there together with the settings
// executable that runs the step; otherwise a new install for all users.
std::wstring MachineRepairHint(const Snapshot& s) {
  if (s.scope != Scope::kMachine || !s.settingsExeFound ||
      !ChooseBuildDll(s.machineBuilds, s.context.osArch)) {
    return ReinstallForAllUsersHint();
  }
  std::wstring hint =
      L"--fix points it at the newest complete build for all users (asks for administrator rights)";
  if (s.context.elevated) hint += L" from a normal, not administrator, prompt";
  return hint;
}

// The folder a machine-wide class's DLL belongs in: <machine root>\app.
std::wstring MachineBuildsText(const Snapshot& s) {
  return (s.paths.machineRoot / kBuildsFolderName).native();
}

}  // namespace

const wchar_t* ArchName(Arch arch) noexcept {
  switch (arch) {
    case Arch::kX64:
      return L"x64";
    case Arch::kArm64:
      return L"arm64";
    case Arch::kOther:
      return L"other";
  }
  return L"other";
}

std::wstring ShortDateTimeText(const SYSTEMTIME& time) {
  if (time.wMonth < 1 || time.wMonth > 12) return L"";
  wchar_t text[48] = {};
  swprintf_s(text, L"%u %ls %u %02u:%02u", time.wDay, kMonths[time.wMonth - 1], time.wYear,
             time.wHour, time.wMinute);
  return text;
}

std::wstring LocalTimeText(const FILETIME& utc) {
  SYSTEMTIME universal = {};
  SYSTEMTIME local = {};
  if (!FileTimeToSystemTime(&utc, &universal) ||
      !SystemTimeToTzSpecificLocalTime(nullptr, &universal, &local)) {
    return L"";
  }
  return ShortDateTimeText(local);
}

std::wstring TaskTimeText(DATE local) {
  SYSTEMTIME time = {};
  if (local <= 0 || !VariantTimeToSystemTime(local, &time)) return L"";
  return ShortDateTimeText(time);
}

std::wstring FindingText(const Snapshot& s, const Finding& f) {
  switch (f.id) {
    case FindingId::kNotInstalled:
      return std::wstring(kProductName) + L" is not installed";
    case FindingId::kClassMissingUser:
      return L"the per-user class is not registered; a complete per-user build is in " +
             f.path.parent_path().native();
    case FindingId::kClassDamagedUser:
    case FindingId::kClassDamagedMachine: {
      const Scope scope = f.id == FindingId::kClassDamagedUser ? Scope::kUser : Scope::kMachine;
      const ClassReg* reg = ClassOf(s, scope);
      const std::wstring who = L"the " + ScopeWord(scope) + L" class";
      if (!reg || reg->dll.empty()) return who + L" names no DLL";
      if (!reg->dllExists) return who + L" points at " + reg->dll + L", which is missing";
      if (!reg->pdfiumExists) return std::wstring(kPdfiumDll) + L" is missing next to " + reg->dll;
      return who + L" points at " + reg->dll + L", outside " + MachineBuildsText(s);
    }
    case FindingId::kUserClassShadowsMachine:
      return L"a per-user class (" + (s.userClass ? s.userClass->dll : std::wstring()) +
             L") hides the machine-wide one";
    case FindingId::kThreadingModel: {
      const ClassReg* reg = ClassOf(s, f.scope);
      const std::wstring threading = reg && !reg->threading.empty() ? L"\"" + reg->threading + L"\""
                                                                    : std::wstring(L"missing");
      return L"the " + ScopeWord(f.scope) + L" class's ThreadingModel is " + threading + L", not " +
             kApartmentThreading;
    }
    case FindingId::kDllArchMismatch: {
      const ClassReg* reg = ClassOf(s, f.scope);
      return L"the handler DLL is built for " + (reg ? DllArchName(*reg) : std::wstring()) +
             L", but Windows runs " + OsArchName(s);
    }
    case FindingId::kSlotEmpty:
      return std::wstring(SlotName(static_cast<size_t>(f.slot))) + L" is empty";
    case FindingId::kSlotOtherHandler: {
      const size_t slot = static_cast<size_t>(f.slot);
      const ClsidDescription& owner = slot < kSlotCount ? s.slotOwners[slot] : ClsidDescription{};
      return std::wstring(SlotName(slot)) + L" -> " + Described(f.clsid, owner) +
             L", another handler";
    }
    case FindingId::kEffectiveOther:
      return L"Windows resolves .pdf thumbnails to " +
             (f.clsid.empty() ? std::wstring(L"nothing") : Described(f.clsid, s.effectiveOwner));
    case FindingId::kDefaultAppHandler:
      return L"the default app " + s.defaultApp.progId.value_or(L"") +
             L" brings its own thumbnail handler " + Described(f.clsid, s.defaultAppHandlerOwner) +
             L", which takes precedence over the .pdf slots";
    case FindingId::kLiveTestFailed:
      return LiveFailureText(f.hr.value_or(E_FAIL));
    case FindingId::kPerUserClassIgnored:
      return Hex(f.hr.value_or(REGDB_E_CLASSNOTREG)) +
             L": File Explorer's thumbnail process cannot load the per-user COM class";
    case FindingId::kLiveTestSkipped:
      if (f.hr) return L"the Explorer test could not run: " + Hex(*f.hr);
      return L"the Explorer test was skipped: " + SkipReason(f.liveSkip);
    case FindingId::kTestFileFailed:
      if (s.testFile && !s.testFile->result) {
        return L"the test file could not be tried: " + Hex(f.hr.value_or(E_FAIL));
      }
      return L"no thumbnail for the test file: " + Hex(f.hr.value_or(E_FAIL));
    case FindingId::kTestFileSkipped:
      return L"the test file was not tried: " + SkipReason(f.liveSkip);
    case FindingId::kSacBlocked:
    case FindingId::kSacEvaluation:
    case FindingId::kSacOn:
    case FindingId::kSacUnknown:
      return std::wstring(kLabelSac) + L": " + SacText(s);
    case FindingId::kIconsOnly:
      return L"\"Always show icons, never thumbnails\" is on (File Explorer: Options > View)";
    case FindingId::kThumbnailsDisabledByPolicy: {
      const ExplorerSettings& e = s.explorer;
      const std::wstring whom = e.thumbnailsOffByUserPolicy && e.thumbnailsOffByMachinePolicy
                                    ? L"for this user and this PC"
                                : e.thumbnailsOffByUserPolicy ? L"for this user"
                                                              : L"for this PC";
      return L"the policy \"Turn off the display of thumbnails and only display icons\" is on " +
             whom;
    }
    case FindingId::kChecksOff:
      if (TaskNamesTaken(s)) return L"no scheduled check: " + TaskTakenText();
      if (s.legacy.legacyTask) {
        return L"no scheduled check; the 1.x task " + std::wstring(kLegacyTask) +
               L" is still there";
      }
      return L"no scheduled check";
    case FindingId::kTaskStaleAction:
      return L"the check task runs " +
             (s.task.actionPath.empty() ? std::wstring(L"no program") : s.task.actionPath) +
             (s.task.actionArgs.empty() ? std::wstring() : L" " + s.task.actionArgs) +
             L" instead of " + f.path.native() + L" " + kCheckTaskArguments;
    case FindingId::kTaskDisabled:
      return L"the check task is disabled";
    case FindingId::kCleanupPending:
      if (f.severity == Severity::kInfo) {
        return L"files of an old build that were in use wait in " +
               std::wstring(f.count == 1 ? L"a trash folder" : L"trash folders") + L" next to " +
               s.paths.machineRoot.native() + L"; Windows deletes them at the next restart";
      }
      if (s.legacy.runOnce) {
        return L"1.x's pending clean-up (RunOnce value " + std::wstring(kLegacyRunOnce) +
               L") would delete " + s.paths.legacyUserRoot.native() + L" at the next sign-in";
      }
      return L"a pending clean-up (RunOnce values " + RunOnceValueName(0) +
             L"...) would delete folders at the next sign-in";
    case FindingId::kV1Leftovers: {
      const LegacyState& l = s.legacy;
      std::vector<std::wstring> parts;
      if (l.legacyTask) parts.push_back(L"the task " + std::wstring(kLegacyTask));
      if (l.ini) {
        parts.push_back(l.iniZoom ? L"settings.ini (Zoom=" + std::to_wstring(*l.iniZoom) + L")"
                                  : std::wstring(L"settings.ini"));
      }
      if (l.runOnce) parts.push_back(L"the RunOnce clean-up value");
      // settings.ini is in the file list too; it is named above.
      const size_t others = static_cast<size_t>(
          std::count_if(l.files.begin(), l.files.end(), [](const std::filesystem::path& file) {
            return !IEquals(file.filename().native(), L"settings.ini");
          }));
      if (others > 0) {
        parts.push_back(Plural(static_cast<int>(others), L"file or folder", L"files and folders") +
                        L" in " + s.paths.legacyUserRoot.native());
      }
      // A machine-wide class in a 1.x build is the install itself: the Handler line says so.
      if (l.userClassInV1) parts.push_back(L"the per-user class (a 1.x build)");
      return L"1.x left " + Joined(parts, L", ");
    }
    case FindingId::kOldBuildFolders: {
      std::vector<std::wstring> names;
      for (const std::filesystem::path& folder : f.paths)
        names.push_back(folder.filename().native());
      return Plural(f.count, L"build folder", L"build folders") + L" that nothing uses: " +
             Joined(names, L", ");
    }
    case FindingId::kZoomInvalid: {
      const ZoomDetails& z = s.zoom;
      std::wstring what;
      if (z.decision.registry == ZoomValueState::kOutOfRange && z.registryRaw) {
        what = L"out of range (" + std::to_wstring(*z.registryRaw) + L")";
      } else {
        what = L"of the wrong type (" + RegistryTypeName(z.registryType) + L")";
      }
      return L"the Zoom value in the registry is " + what + L"; thumbnails use " +
             std::to_wstring(z.decision.zoom) + L" %";
    }
    case FindingId::kPdfCraftChanged:
      return std::wstring(kPdfCraftProductName) + L" " + VersionOrNone(f.from) + L" -> " +
             VersionOrNone(f.to) + L" since the last check";
    case FindingId::kElevated:
      return L"running as administrator: the Explorer test is skipped, and per-user settings are "
             L"those of " +
             s.context.userAccount;
    case FindingId::kDifferentUser:
      return L"running as " + s.context.userAccount + L", not as the user signed in to Windows";
    case FindingId::kVirtualized:
      return L"the checks through Windows' association code were skipped: " +
             ChecksSkipReason(f.checksSkip);
  }
  return FindingName(f.id);
}

namespace {

// The hint, with the other findings for the cases where they change the advice.
std::wstring HintFor(const Snapshot& s, std::span<const Finding> findings, const Finding& f) {
  const std::wstring product = kProductName;
  switch (f.id) {
    case FindingId::kNotInstalled:
      return L"Install it with " +
             std::wstring(kDevInstall
                              ? L"the installer (or, in a developer build, --install from the "
                                L"build's folder)"
                              : L"the installer");
    case FindingId::kClassMissingUser:
    case FindingId::kClassDamagedUser:
      if (f.fix == FixKind::kAuto) return L"--check registers the newest per-user build again";
      return L"Reinstall " + product;
    case FindingId::kClassDamagedMachine:
      return MachineRepairHint(s);
    case FindingId::kUserClassShadowsMachine:
      if (f.fix == FixKind::kAuto) return L"--check removes the per-user class";
      return L"Repair the machine-wide part first: " + MachineRepairHint(s);
    case FindingId::kThreadingModel:
      if (f.fix == FixKind::kAuto) return L"--check registers the class again";
      if (f.fix == FixKind::kNeedsAdmin) return MachineRepairHint(s);
      return L"Reinstall " + product;
    case FindingId::kDllArchMismatch:
      return L"Reinstall the " + OsArchName(s) + L" build of " + product;
    case FindingId::kSlotEmpty:
      return L"--check fills it";
    case FindingId::kSlotOtherHandler:
      return L"--fix --take-back points it back at " + product;
    case FindingId::kEffectiveOther:
      if (Find(findings, FindingId::kSlotEmpty) || Find(findings, FindingId::kSlotOtherHandler)) {
        return L"Fix the .pdf slots first (above)";
      }
      if (Find(findings, FindingId::kDefaultAppHandler)) {
        return L"The default app's own thumbnail handler wins; choose another default app for "
               L".pdf files";
      }
      return L"File Explorer may still use an older association: sign out and in again, then "
             L"run --status";
    case FindingId::kDefaultAppHandler:
      return L"It matters only when the Effective line names it";
    case FindingId::kLiveTestFailed: {
      const HRESULT hr = f.hr.value_or(E_FAIL);
      if (IsLoadBlockedHresult(hr)) {
        // Smart App Control is one source of these errors; an App Control policy or a damaged
        // file give the same ones.
        if (s.sacVerdict.kind == SacVerdictKind::kBlocked) return L"See the Smart App Control line";
        return L"An application control policy (or a damaged file) blocked the DLL; reinstall " +
               product + L", or ask whoever manages this PC";
      }
      if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) return L"Run --status again";
      return L"Try --status --test-file <pdf> with a PDF of your own; --diagnostics makes a "
             L"report for a bug report";
    }
    case FindingId::kPerUserClassIgnored:
      return L"Install " + product + L" for all users: " + InstallCommandHint();
    case FindingId::kTestFileFailed:
      return L"The handler could not make a thumbnail of this file; --diagnostics makes a report "
             L"for a bug report";
    case FindingId::kSacBlocked:
      return L"Turn off Smart App Control, or use a signed release";
    case FindingId::kIconsOnly:
      return L"Turn it off in File Explorer: Options > View";
    case FindingId::kThumbnailsDisabledByPolicy:
      return L"Ask whoever manages this PC, or turn the policy off in the Local Group Policy "
             L"Editor";
    case FindingId::kChecksOff:
      // --checks alone would fail: only an administrator can free the names.
      if (TaskNamesTaken(s)) return TaskTakenChecksHint(s);
      // Nothing to say where --checks cannot turn them on (a 1.x build is installed): the
      // Checks line shows them off, and installing this version brings them.
      return ChecksCommandHint(s, L"default", L"turns them on");
    case FindingId::kTaskDisabled: {
      // The checkboxes as they show now; none at all asks for both.
      const bool any = s.task.afterPdfCraft || s.task.atSignIn;
      std::wstring hint = ChecksCommandHint(
          s, any ? ChecksValue(s.task.afterPdfCraft, s.task.atSignIn) : L"default",
          L"turns it on again");
      return hint.empty() ? L"Turn it on again in Task Scheduler" : hint;
    }
    case FindingId::kTaskStaleAction:
      if (f.fix == FixKind::kAuto) return L"--check registers it again with the same triggers";
      // The program is there, but the task has neither of the check's triggers to keep.
      if (std::wstring hint =
              ChecksCommandHint(s, L"default", L"registers it again with both triggers");
          !hint.empty()) {
        return hint;
      }
      // The program it should run is not there: registering it again would point at nothing.
      return L"Reinstall " + product + L"; until then --checks none removes the task";
    case FindingId::kCleanupPending:
      if (f.severity == Severity::kInfo) return L"Restart Windows to finish";
      return L"--check deletes the pending clean-up";
    case FindingId::kV1Leftovers:
      if (f.fix == FixKind::kAuto) return L"--check removes the files once nothing uses them";
      if (InstalledSettingsExe(s)) return L"--migrate-v1 removes them and keeps the zoom";
      return L"--migrate-v1 removes them, once this version is installed";
    case FindingId::kOldBuildFolders:
      return L"--check removes them (a folder still in use stays until the next check)";
    case FindingId::kZoomInvalid:
      return L"--check rewrites it";
    case FindingId::kElevated:
      return L"Run it from a normal (not administrator) prompt";
    case FindingId::kDifferentUser:
      return L"Run it as the user signed in to Windows";
    case FindingId::kVirtualized:
      return L"Run it from File Explorer or from a terminal that is not part of an app package";
    case FindingId::kLiveTestSkipped:
    case FindingId::kTestFileSkipped:
    case FindingId::kSacEvaluation:
    case FindingId::kSacOn:
    case FindingId::kSacUnknown:
    case FindingId::kPdfCraftChanged:
      return L"";
  }
  return L"";
}

}  // namespace

std::wstring FindingHint(const Snapshot& s, const Finding& f) {
  return HintFor(s, std::span<const Finding>(), f);
}

std::wstring TestFileProblemText(TestFileProblem problem) {
  switch (problem) {
    case TestFileProblem::kNone:
      return L"is fine";
    case TestFileProblem::kEmpty:
      return L"is empty";
    case TestFileProblem::kBadName:
      return L"is not a usable path";
    case TestFileProblem::kDevicePath:
      return L"names a device, not a file";
    case TestFileProblem::kNotFound:
      return L"does not exist";
    case TestFileProblem::kDirectory:
      return L"is a folder, not a file";
    case TestFileProblem::kNotDiskFile:
      return L"is not a file on a disk";
    case TestFileProblem::kUnreadable:
      return L"cannot be opened";
  }
  return L"cannot be used";
}

std::wstring RepairOutcomeText(const RepairOutcome& outcome) {
  const RepairStep& step = outcome.step;
  const std::wstring product = kProductName;
  const std::wstring slot = step.slot >= 0 && static_cast<size_t>(step.slot) < kSlotCount
                                ? std::wstring(SlotName(static_cast<size_t>(step.slot)))
                                : std::wstring(L"a slot");
  std::wstring what;
  switch (step.kind) {
    case RepairStep::Kind::kDeleteRunOnce:
      what = L"delete the pending sign-in clean-up";
      break;
    case RepairStep::Kind::kRegisterUserClass:
      what = L"register the per-user class for " + step.path.native();
      break;
    case RepairStep::Kind::kDeleteUserClass:
      what = L"remove the per-user class that hides the machine-wide one";
      break;
    case RepairStep::Kind::kFillSlot:
      what = L"point the empty " + slot + L" slot at " + product;
      break;
    case RepairStep::Kind::kTakeBackSlot:
      what = L"take the " + slot + L" slot back from another handler";
      break;
    case RepairStep::Kind::kRewriteZoom:
      what = L"rewrite the zoom value as " + std::to_wstring(step.zoom) + L" %";
      break;
    case RepairStep::Kind::kReRegisterTask:
      what = L"register the check task again for " + step.path.native();
      break;
    case RepairStep::Kind::kRemoveUserBuild:
      what = L"remove the old build folder " + step.path.filename().native();
      break;
    case RepairStep::Kind::kRemoveV1Files:
      what = L"remove the files 1.x left in its per-user folder";
      break;
  }
  switch (outcome.result) {
    case RepairResult::kDone:
      return L"Done: " + what;
    case RepairResult::kUnchanged:
      return L"Nothing left to do: " + what;
    case RepairResult::kKept:
      return L"Left for the next check (still in use): " + what;
    case RepairResult::kNotApplied:
      switch (outcome.notApplied) {
        case NotApplied::kElevated:
          return step.kind == RepairStep::Kind::kRemoveUserBuild
                     ? L"Not done, running as administrator (a normal check removes it): " + what
                     : L"Not done, running as administrator (a normal check does it): " + what;
        case NotApplied::kNoProgram:
          return L"Not done, the installed program is not there: " + what;
        case NotApplied::kNoTrigger:
          return L"Not done, the task has neither of the check's triggers (choose them with "
                 L"--checks): " +
                 what;
        case NotApplied::kNone:
          break;
      }
      return L"Not done: " + what;
    case RepairResult::kFailed:
      if (outcome.hr == kTaskTakenHr) {
        return L"Failed, " + TaskTakenText() + L" (" + Hex(outcome.hr) + L"): " + what;
      }
      return L"Failed (" + Hex(outcome.hr) + L"): " + what;
  }
  return what;
}

std::wstring ChecksStateText(bool afterPdfCraft, bool atSignIn) {
  const auto onOff = [](bool on) { return on ? L"on" : L"off"; };
  return L"after " + std::wstring(kPdfCraftProductName) + L" updates: " + onOff(afterPdfCraft) +
         kDot + L"at sign-in: " + onOff(atSignIn);
}

const wchar_t* ChecksValue(bool afterPdfCraft, bool atSignIn) noexcept {
  if (afterPdfCraft && atSignIn) return L"default";
  if (afterPdfCraft) return L"pdfcraft";
  if (atSignIn) return L"signin";
  return L"none";
}

ScheduleReport ScheduleResultReport(const ScheduleResult& result) {
  ScheduleReport report;
  const Snapshot& s = result.before;
  const std::wstring product = kProductName;
  switch (result.refusal) {
    case ScheduleRefusal::kNotInstalled:
      report.error = product +
                     L" is not installed, so there is nothing for scheduled checks to "
                     L"check. Install it first.";
      return report;
    case ScheduleRefusal::kNoSettingsExe: {
      if (s.installRoot.empty()) {
        // Only a per-user build that is not registered yet: the check registers it.
        report.error = L"No install with " + std::wstring(kSettingsExe) +
                       L" for the scheduled checks to run was found. Run --check first, then "
                       L"try again.";
      } else if (ActiveHandlerIs1x(s)) {
        report.error = L"The installed " + product + L" is a 1.x build, which has no " +
                       (s.installRoot / kSettingsExe).native() +
                       L" for the scheduled checks to run. Install this version first.";
      } else {
        report.error = (s.installRoot / kSettingsExe).native() +
                       L" is not there, so a scheduled check would have nothing to run. "
                       L"Reinstall " +
                       product + L".";
      }
      return report;
    }
    case ScheduleRefusal::kWrongContext:
    case ScheduleRefusal::kElevated:
    case ScheduleRefusal::kNone:
      break;
  }
  if (result.error == kTaskTakenHr) {
    report.error = L"Could not register the check task: " + TaskTakenText() + L" (" +
                   Hex(result.error) + L"). " + TaskTakenHint(s.taskLocation) +
                   L"; then run --checks again.";
    return report;
  }
  if (FAILED(result.error)) {
    report.error =
        result.error == kSomeTriggersFailed
            ? L"Task Scheduler registered the check task but could not set up all of its "
              L"triggers (" +
                  Hex(result.error) + L"); run --status to see what it has."
            : L"Could not change the check task: " + Hex(result.error);
    return report;
  }

  if (result.remove) {
    report.out.push_back(s.task.exists ? L"Checks: off (the check task was removed)"
                                       : L"Checks: off (there was no check task)");
  } else if (FAILED(result.readError) || !result.after.exists) {
    // Registered, but it cannot be shown as it is now: what was asked for.
    report.out.push_back(L"Checks: " +
                         ChecksStateText(result.askedAfterPdfCraft, result.askedAtSignIn));
  } else {
    report.out.push_back(L"Checks: " +
                         ChecksStateText(result.after.afterPdfCraft, result.after.atSignIn));
  }
  if (result.readError == kTaskTakenHr) {
    // Nothing of this user's was there to change or to read back (checks turned off where there
    // was no task): another account's tasks hold the names, which the line says in words, with
    // the way out.
    report.out.push_back(L"The check task cannot be registered while " + TaskTakenText() + L" (" +
                         Hex(result.readError) + L"). " + TaskTakenHint(s.taskLocation) + L".");
  } else if (FAILED(result.readError)) {
    report.out.push_back(L"The check task was changed but could not be read back: " +
                         Hex(result.readError));
  } else if (result.after.exists) {
    report.out.push_back(L"Check task: " + result.after.path + L", runs " +
                         result.after.actionPath + L" " + result.after.actionArgs);
  }
  if (s.legacy.legacyTask) {
    report.out.push_back(L"The 1.x task " + std::wstring(kLegacyTask) +
                         L" is still there; it was left as it is.");
  }
  return report;
}

std::wstring TaskTakenText() {
  return L"another account's scheduled tasks hold the names of this user's check task";
}

std::wstring TaskTakenHint(const TaskLocation& location) {
  std::vector<std::wstring> names;
  if (!location.name.empty()) names.push_back(TaskPathText(location.folder, location.name));
  if (!location.fallbackName.empty()) {
    names.push_back(TaskPathText(location.fallbackFolder, location.fallbackName));
  }
  const std::wstring which = names.empty() ? std::wstring(L"them") : Joined(names, L" and ");
  return L"An administrator can delete " + which + L" in Task Scheduler (uninstalling " +
         kProductName + L" for all users deletes them too)";
}

std::wstring ElevatedVerbText(ElevatedVerb verb) {
  switch (verb) {
    case ElevatedVerb::kMachineInstall:
      return L"install " + std::wstring(kProductName) + L" for all users";
    case ElevatedVerb::kMachineRepairClass:
      return L"point the machine-wide class at a complete build again";
    case ElevatedVerb::kMachineUninstall:
      return L"remove the machine-wide part";
    case ElevatedVerb::kMachineMigrateV1:
      return L"remove the machine-wide part of 1.x";
  }
  return L"repair the machine-wide part";
}

std::wstring ResultText(std::span<const Finding> findings) {
  if (Find(findings, FindingId::kNotInstalled)) return L"Result: not installed";
  const int problems = static_cast<int>(std::count_if(
      findings.begin(), findings.end(), [](const Finding& f) { return f.breaksThumbnails; }));
  if (problems == 0) return L"Result: healthy";
  return L"Result: " + Plural(problems, L"problem keeps", L"problems keep") +
         L" PDF thumbnails from working";
}

std::vector<StatusLine> FormatStatus(const Snapshot& s, std::span<const Finding> findings) {
  std::vector<StatusLine> lines;
  const auto line = [&](char mark, std::wstring label, std::wstring text,
                        std::wstring hint = {}) -> StatusLine& {
    lines.push_back(StatusLine{mark, std::move(label), std::move(text), std::move(hint)});
    return lines.back();
  };
  const auto has = [&](FindingId id) { return Find(findings, id); };
  const auto hint = [&](const Finding& f) { return HintFor(s, findings, f); };
  const bool installed = !has(FindingId::kNotInstalled);

  line(' ', L"",
       std::wstring(kProductName) + L" " + kProductVersion + L" (" + kArch + L"), Windows build " +
           (s.osBuild.empty() ? std::wstring(L"unknown") : s.osBuild));

  // ---- The install and its class ----
  if (const Finding* f = has(FindingId::kNotInstalled)) {
    line('!', kLabelInstall, L"not installed", hint(*f));
  } else if (s.scope == Scope::kMachine) {
    line(' ', kLabelInstall, L"machine-wide (" + s.installRoot.native() + L"); settings per user");
  } else {
    line(' ', kLabelInstall, L"per user (" + s.paths.userRoot.native() + L")");
  }

  const ClassReg* active = s.ActiveClass();
  if (!active) {
    if (const Finding* f = has(FindingId::kClassMissingUser)) {
      line('!', kLabelHandler,
           L"not registered; a complete per-user build is ready: " + f->path.native(), hint(*f));
    } else {
      line(' ', kLabelHandler, L"not registered");
    }
  } else {
    // The DLL registered for the install's scope: the one File Explorer's thumbnail process
    // loads, so tests and bug reports can tell which build is in use.
    const Finding* damaged = has(s.scope == Scope::kMachine ? FindingId::kClassDamagedMachine
                                                            : FindingId::kClassDamagedUser);
    std::wstring text = active->dll.empty() ? std::wstring(L"(no DLL path)") : active->dll;
    if (!active->dllExists) {
      text += L" (missing)";
    } else {
      text +=
          L" (" +
          (active->fileVersion.empty() ? std::wstring(L"version unknown") : active->fileVersion) +
          L", " + DllArchName(*active) + L", " +
          (active->threading.empty() ? std::wstring(L"no ThreadingModel") : active->threading) +
          L")";
    }
    // A machine-wide class pointing outside the install's build folders (ClassDamagedMachine
    // names the DLL only then).
    const bool misplaced =
        s.scope == Scope::kMachine && damaged && !damaged->path.empty() && active->dllExists;
    if (misplaced) {
      text += L"; outside " + MachineBuildsText(s);
    } else if (s.scope == Scope::kMachine && s.legacy.machineClassInV1) {
      // 1.x's own install, still in use: not a leftover until something replaces it.
      text += L"; a 1.x build (installing this version for all users replaces it)";
    }
    std::vector<std::wstring> hints;
    if (damaged && (!active->dllExists || misplaced)) hints.push_back(hint(*damaged));
    for (const Finding& f : findings) {
      if ((f.id == FindingId::kThreadingModel || f.id == FindingId::kDllArchMismatch) &&
          f.scope == s.scope) {
        hints.push_back(hint(f));
      }
    }
    std::sort(hints.begin(), hints.end());
    hints.erase(std::unique(hints.begin(), hints.end()), hints.end());
    line(hints.empty() ? 'K' : '!', kLabelHandler, std::move(text), Joined(hints, L"; "));

    // Without the DLL there is nothing to look for PDFium next to.
    if (active->dllExists && active->pdfiumExists) {
      line('K', kLabelPdfium, std::wstring(kPdfiumDll) + L" next to the handler");
    } else if (active->dllExists) {
      line('!', kLabelPdfium, std::wstring(kPdfiumDll) + L" missing next to the handler",
           damaged ? hint(*damaged) : L"Reinstall " + std::wstring(kProductName));
    }
  }

  // ---- The slots and what Windows makes of them ----
  for (size_t slot = 0; slot < kSlotCount; ++slot) {
    const std::wstring name(SlotName(slot));
    const std::optional<std::wstring>& value = s.slots[slot];
    const Finding* problem = nullptr;
    for (const Finding& f : findings) {
      if ((f.id == FindingId::kSlotEmpty || f.id == FindingId::kSlotOtherHandler) &&
          f.slot == static_cast<int>(slot)) {
        problem = &f;
      }
    }
    std::wstring text;
    if (SlotIsEmpty(value)) {
      text = name + L" -> empty";
    } else if (IsOurClsid(*value)) {
      text = name + L" -> " + kProductName;
    } else {
      text = name + L" -> " + Described(*value, s.slotOwners[slot]) + L", another handler";
    }
    if (problem) {
      line('!', kLabelSlot, std::move(text), hint(*problem));
    } else if (s.windowsChecksSkipped) {
      // This process may read a private copy of the slot, not the one File Explorer reads.
      line(' ', kLabelSlot, text + L" (as this process sees it; not checked)");
    } else {
      // Not judged when nothing is installed.
      line(installed ? 'K' : ' ', kLabelSlot, std::move(text));
    }
  }

  if (s.windowsChecksSkipped) {
    const Finding* f = has(FindingId::kVirtualized);
    line(' ', kLabelEffective, L"not checked: " + ChecksSkipReason(s.windowsChecksSkip),
         f ? hint(*f) : std::wstring());
  } else if (const Finding* f = has(FindingId::kEffectiveOther)) {
    line('!', kLabelEffective, FindingText(s, *f), hint(*f));
  } else {
    const std::wstring target = !s.effective ? std::wstring(L"nothing")
                                : IsOurClsid(*s.effective)
                                    ? std::wstring(kProductName)
                                    : Described(*s.effective, s.effectiveOwner);
    line(installed ? 'K' : ' ', kLabelEffective, L"Windows resolves .pdf thumbnails to " + target);
  }

  {
    std::wstring app;
    if (s.windowsChecksSkipped) {
      app = L"not checked";
    } else if (!s.defaultApp.progId) {
      app = L"none";
    } else {
      const std::optional<std::wstring>& overlay = s.defaultApp.overlay;
      app = *s.defaultApp.progId + L" (" +
            (!overlay           ? std::wstring(L"its icon is drawn on thumbnails")
             : overlay->empty() ? std::wstring(L"no icon overlay")
                                : L"overlay " + *overlay) +
            L")";
    }
    std::wstring craft = PdfCraftText(s);
    if (const Finding* changed = has(FindingId::kPdfCraftChanged)) {
      craft += L" (" + VersionOrNone(changed->from) + L" at the last check)";
    }
    if (const Finding* f = has(FindingId::kDefaultAppHandler)) {
      line('!', kLabelDefaultApp, app + kDot + craft + L"; " + FindingText(s, *f), hint(*f));
    } else {
      line(' ', kLabelDefaultApp, app + kDot + craft);
    }
  }

  // ---- The Explorer test ----
  if (s.live) {
    const Finding* failed = has(FindingId::kPerUserClassIgnored);
    if (!failed) failed = has(FindingId::kLiveTestFailed);
    if (failed) {
      line('!', kLabelExplorerTest, FindingText(s, *failed), hint(*failed));
    } else if (s.live->hr == S_OK) {
      line('K', kLabelExplorerTest,
           L"thumbnail extracted in a fresh thumbnail process (" + std::to_wstring(s.live->ms) +
               L" ms)");
    } else {
      line(' ', kLabelExplorerTest, LiveFailureText(s.live->hr));
    }
  } else if (FAILED(s.liveError)) {
    line(' ', kLabelExplorerTest, L"could not run: " + Hex(s.liveError));
  } else {
    line(' ', kLabelExplorerTest, L"skipped: " + SkipReason(s.liveSkip));
  }

  if (s.testFile) {
    const TestFileRun& run = *s.testFile;
    if (run.problem != TestFileProblem::kNone) {
      line('!', kLabelTestFile, L"the file " + TestFileProblemText(run.problem),
           L"Name an existing PDF file: --status --test-file <pdf>");
    } else if (run.result && run.result->hr == S_OK) {
      line('K', kLabelTestFile,
           std::to_wstring(run.result->size.cx) + L"x" + std::to_wstring(run.result->size.cy) +
               L" thumbnail extracted in " + std::to_wstring(run.result->ms) + L" ms");
    } else if (const Finding* f = has(FindingId::kTestFileFailed); f && !run.result) {
      line('!', kLabelTestFile, L"could not run: " + Hex(f->hr.value_or(E_FAIL)),
           L"Run --status --test-file <pdf> again");
    } else if (f) {
      line('!', kLabelTestFile, L"no thumbnail: " + LiveFailureText(f->hr.value_or(E_FAIL)),
           hint(*f));
    } else if (run.skip != LiveTestSkip::kNone) {
      line(' ', kLabelTestFile, L"skipped: " + SkipReason(run.skip));
    } else {
      line('!', kLabelTestFile, L"could not run", L"Run --status --test-file <pdf> again");
    }
  }

  // ---- Settings, checks, Smart App Control ----
  {
    std::wstring text = std::to_wstring(s.zoom.decision.zoom) + L" % (" +
                        ZoomOriginText(s.zoom.decision.origin) + L")";
    if (ActiveHandlerIs1x(s) && s.zoom.decision.origin == ZoomOrigin::kRegistry) {
      text += L"; the registered 1.x handler reads only settings.ini (" +
              std::to_wstring(s.zoom.legacyIniZoom.value_or(100)) + L" %)";
    }
    if (const Finding* f = has(FindingId::kZoomInvalid)) {
      line('!', kLabelZoom, text + L"; " + FindingText(s, *f), hint(*f));
    } else {
      line(' ', kLabelZoom, std::move(text));
    }
  }

  {
    std::wstring last;
    if (s.last) {
      const std::wstring when = LocalTimeText(s.last->time);
      last = L"last check " + (when.empty() ? std::wstring(L"(time unknown)") : when) +
             (s.last->summary.empty() ? std::wstring() : L" (" + s.last->summary + L")");
    } else if (s.task.exists && s.task.lastRun) {
      last = L"last run " + TaskTimeText(*s.task.lastRun);
    }
    if (s.task.exists) {
      std::wstring text = ChecksStateText(s.task.afterPdfCraft, s.task.atSignIn);
      if (!last.empty()) text += kDot + last;
      std::vector<std::wstring> problems;
      std::vector<std::wstring> hints;
      for (FindingId id : {FindingId::kTaskDisabled, FindingId::kTaskStaleAction}) {
        if (const Finding* f = has(id)) {
          problems.push_back(FindingText(s, *f));
          hints.push_back(hint(*f));
        }
      }
      if (problems.empty()) {
        line('K', kLabelChecks, std::move(text));
      } else {
        line('!', kLabelChecks, text + L"; " + Joined(problems, L"; "), Joined(hints, L"; "));
      }
    } else {
      std::wstring text = L"off";
      if (s.legacy.legacyTask) {
        text += L" (the 1.x task " + std::wstring(kLegacyTask) + L" is still there)";
      }
      const Finding* off = has(FindingId::kChecksOff);
      // Not the user's choice: another account's tasks keep theirs from being registered.
      const bool taken = off != nullptr && TaskNamesTaken(s);
      if (taken) text += L"; " + TaskTakenText();
      if (!last.empty()) text += kDot + last;
      line(taken ? '!' : ' ', kLabelChecks, std::move(text), off ? hint(*off) : std::wstring());
    }
  }

  if (s.sacVerdict.kind != SacVerdictKind::kNone) {
    if (const Finding* f = has(FindingId::kSacBlocked)) {
      line('!', kLabelSac, SacText(s), hint(*f));
    } else {
      line(' ', kLabelSac, SacText(s));
    }
  }

  // ---- Findings without a line of their own ----
  for (const Finding& f : findings) {
    switch (f.id) {
      case FindingId::kUserClassShadowsMachine:
        line('!', kLabelUserClass,
             (s.userClass ? s.userClass->dll : std::wstring()) + L" hides the machine-wide class",
             hint(f));
        break;
      case FindingId::kIconsOnly:
        line('!', kLabelExplorer, FindingText(s, f), hint(f));
        break;
      case FindingId::kThumbnailsDisabledByPolicy:
        line('!', kLabelPolicy, FindingText(s, f), hint(f));
        break;
      case FindingId::kCleanupPending:
        if (f.severity == Severity::kInfo) {
          line(' ', kLabelRestart, FindingText(s, f), hint(f));
        } else {
          line('!', kLabelCleanup, FindingText(s, f), hint(f));
        }
        break;
      case FindingId::kV1Leftovers:
        line(' ', kLabelLeftovers, FindingText(s, f), hint(f));
        break;
      case FindingId::kOldBuildFolders:
        line(' ', kLabelOldBuilds, FindingText(s, f), hint(f));
        break;
      case FindingId::kDifferentUser:
        line('!', kLabelAccount, FindingText(s, f), hint(f));
        break;
      case FindingId::kElevated:
        line(' ', kLabelNote, FindingText(s, f), hint(f));
        break;
      default:
        break;
    }
  }

  line(' ', L"", ResultText(findings));
  return lines;
}

std::wstring RenderStatus(std::span<const StatusLine> lines) {
  std::wstring text;
  for (const StatusLine& line : lines) {
    if (!text.empty()) text += L"\n";
    if (line.label.empty()) {
      text += line.text;
    } else {
      text += line.mark == 'K' ? L"  OK " : line.mark == '!' ? L"  !! " : L"     ";
      text += line.label;
      text.append(line.label.size() < kLabelWidth ? kLabelWidth - line.label.size() : 2, L' ');
      text += line.text;
    }
    if (!line.hint.empty()) {
      text += L"\n";
      text.append(kTextColumn, L' ');
      text += line.hint;
    }
  }
  return text;
}

}  // namespace pdft
