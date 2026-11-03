#include "check.h"

#include <wil/result_macros.h>

#include <algorithm>
#include <utility>

#include "instance.h"
#include "log.h"
#include "registration.h"
#include "settings.h"
#include "strings.h"

namespace pdft {
namespace {

constexpr wchar_t kLogComponent[] = L"check";

int CountBreaking(std::span<const Finding> findings) noexcept {
  return static_cast<int>(std::count_if(findings.begin(), findings.end(),
                                        [](const Finding& f) { return f.breaksThumbnails; }));
}

void LogLine(LogLevel level, const std::wstring& message) noexcept {
  Log(level, kLogComponent, message);
}

// The log's word for why the Explorer test was skipped.
const wchar_t* LiveSkipLogName(LiveTestSkip skip) noexcept {
  switch (skip) {
    case LiveTestSkip::kNone:
      return L"none";
    case LiveTestSkip::kNotRequested:
      return L"not-requested";
    case LiveTestSkip::kElevated:
      return L"elevated";
    case LiveTestSkip::kVirtualized:
      return L"virtualized";
    case LiveTestSkip::kNotOurHandler:
      return L"not-our-handler";
    case LiveTestSkip::kNoClass:
      return L"no-class";
  }
  return L"none";
}

// The second gathering. After a slot or class changed, everything is read again, the Explorer
// test included; otherwise the test's answer cannot have changed and is kept, so a check that
// only tidied up does not start a second thumbnail process.
HRESULT GatherAgain(const RegistryRoots& roots, const GatherOptions& first,
                    const SystemProbes& probes, const Snapshot& before, bool slotOrClassChanged,
                    Snapshot* after) noexcept try {
  GatherOptions options = first;
  options.testFile.reset();  // the user's file was tested once; the result is carried over
  if (!slotOrClassChanged) options.liveTest = false;
  RETURN_IF_FAILED(GatherSnapshot(roots, options, probes, after));
  after->testFile = before.testFile;
  if (!slotOrClassChanged) {
    after->live = before.live;
    after->liveSkip = before.liveSkip;
    after->liveError = before.liveError;
    after->blocks = before.blocks;
    after->sacVerdict = before.sacVerdict;
  }
  return S_OK;
}
CATCH_RETURN();

}  // namespace

ExitCode CheckExitCode(std::span<const Finding> findings, bool repairFailed, bool adminStepNeeded,
                       bool quiet) noexcept {
  const ExitCode code = ExitCodeFromFindings(findings);
  if (code == ExitCode::kNotInstalled) return code;
  if (adminStepNeeded && quiet) return ExitCode::kNeedsAdmin;
  if (code == ExitCode::kOk && (repairFailed || adminStepNeeded)) return ExitCode::kProblems;
  return code;
}

std::wstring CheckSummary(ExitCode code, int fixed, int problems) {
  const auto count = [](int n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
  };
  std::wstring text;
  switch (code) {
    case ExitCode::kOk:
      return fixed == 0 ? L"nothing to fix" : L"fixed " + std::to_wstring(fixed);
    case ExitCode::kNeedsAdmin:
      text = L"the machine-wide part needs an administrator";
      break;
    case ExitCode::kNotInstalled:
      return L"not installed";
    default:
      text = problems > 0 ? count(problems, L"problem remains", L"problems remain")
                          : L"a repair did not complete";
      break;
  }
  if (fixed > 0) text += L"; fixed " + std::to_wstring(fixed);
  return text;
}

const wchar_t* CheckResultName(ExitCode code) noexcept {
  switch (code) {
    case ExitCode::kOk:
      return L"healthy";
    case ExitCode::kProblems:
      return L"problems";
    case ExitCode::kUsage:
      return L"usage";
    case ExitCode::kCancelled:
      return L"cancelled";
    case ExitCode::kBusy:
      return L"busy";
    case ExitCode::kFailed:
      return L"failed";
    case ExitCode::kNotInstalled:
      return L"not-installed";
    case ExitCode::kNeedsAdmin:
      return L"needs-admin";
    case ExitCode::kWrongContext:
      return L"refused";
  }
  return L"failed";
}

std::wstring LiveTestLogFields(const Snapshot& s) {
  if (s.live) {
    return L" live=" + HResultText(s.live->hr) + L" live-ms=" + std::to_wstring(s.live->ms);
  }
  if (FAILED(s.liveError)) return L" live=could-not-start live-hr=" + HResultText(s.liveError);
  return std::wstring(L" live=skipped live-skip=") + LiveSkipLogName(s.liveSkip);
}

HRESULT RunCheck(const CheckOptions& options, const RegistryRoots& roots,
                 const SystemProbes& probes, CheckResult* result) noexcept try {
  *result = CheckResult{};
  const ULONGLONG started = GetTickCount64();

  // Only the canary says whether writes reach the real hive; anything else may be a private copy
  // that Explorer never reads, where a "repair" would report success and change nothing.
  ProcessContext context;
  if (options.context) {
    context = *options.context;
  } else {
    RETURN_IF_FAILED(probes.GetProcessContext(ContextProbe::kWrite, &context));
  }
  if (context.probe != ContextProbe::kWrite || RefusesWrites(context.virtualization)) {
    result->refused = true;
    result->code = ExitCode::kWrongContext;
    return S_OK;
  }
  // The repairs write the user's classes and slots and delete folders in the user's profile,
  // which the user's own unelevated programs can change in the meantime: an administrator process
  // never does that. A scheduled check runs unelevated.
  if (context.elevated) {
    result->refused = true;
    result->refusedElevated = true;
    result->code = ExitCode::kWrongContext;
    return S_OK;
  }

  std::optional<OperationLock> lock;
  const HRESULT locked = OperationLock::Acquire(EffectiveLockWaitMs(options.lockWaitMs), &lock);
  if (locked == OperationLock::kBusy || locked == OperationLock::kTakenByLowerIntegrity) {
    result->busy = true;
    result->code = ExitCode::kBusy;
    LogLine(LogLevel::kWarning, locked == OperationLock::kBusy
                                    ? L"check result=busy"
                                    : L"check result=busy lock-taken-by-lower-integrity");
    return S_OK;
  }
  if (locked == kNamedObjectsRefusedHr) {
    result->namedObjectsRefused = true;
    result->code = ExitCode::kFailed;
    LogLine(LogLevel::kWarning,
            L"check result=failed named-objects-refused hr=" + HResultText(kNamedObjectsRefusedHr));
    return S_OK;
  }
  RETURN_IF_FAILED(locked);
  result->lockAbandoned = lock->abandoned();
  // Another account made the global lock's name first: this lock keeps out only this session.
  if (lock->sessionOnly()) LogLine(LogLevel::kWarning, L"check lock=session-only");

  GatherOptions gather;
  gather.liveTest = options.liveTest;
  gather.testFile = options.testFile;
  gather.context = context;
  gather.paths = options.paths;
  Snapshot before;
  RETURN_IF_FAILED(GatherSnapshot(roots, gather, probes, &before));
  result->before = Evaluate(before);

  if (HasFinding(result->before, FindingId::kNotInstalled)) {
    // Nothing to repair and nothing to record: a product that is not there keeps no state.
    result->findings = result->before;
    result->after = std::move(before);
    result->code = ExitCode::kNotInstalled;
    result->summary = CheckSummary(result->code, 0, 0);
    LogLine(LogLevel::kInfo,
            L"check result=not-installed fixed=0" + LiveTestLogFields(result->after));
    return S_OK;
  }

  result->plan = PlanRepairs(before, result->before, options.repair);
  RETURN_IF_FAILED(ApplyUserRepairs(roots, probes, before, result->plan, &result->repairs));
  const int fixed = CountDone(result->repairs);
  bool slotOrClassChanged = ChangedSlotOrClass(result->repairs);

  // --fix: the administrator step, run by the settings executable installed for all users (the
  // only one the elevated side accepts), while this process still holds the lock.
  bool machineRepaired = false;
  if (result->plan.machine && options.launcher && !options.quiet) {
    const std::filesystem::path exe = before.paths.machineRoot / kSettingsExe;
    const DWORD attributes = GetFileAttributesW(exe.c_str());
    if (*result->plan.machine != ElevatedVerb::kMachineRepairClass ||
        attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
      result->adminStepUnavailable = true;
    } else {
      ElevatedRun run;
      RETURN_IF_FAILED(
          RunElevated(*result->plan.machine, exe, options.owner, options.launcher, &run));
      machineRepaired =
          run.outcome == ElevatedOutcome::kDone || run.outcome == ElevatedOutcome::kRestartPending;
      result->elevated = std::move(run);
    }
  } else if (result->plan.machine && !options.quiet) {
    const std::filesystem::path exe = before.paths.machineRoot / kSettingsExe;
    result->adminStepUnavailable = GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES;
  }
  if (machineRepaired) slotOrClassChanged = true;

  if (fixed > 0 || machineRepaired) {
    RETURN_IF_FAILED(
        GatherAgain(roots, gather, probes, before, slotOrClassChanged, &result->after));
    result->findings = Evaluate(result->after);
  } else {
    result->after = std::move(before);
    result->findings = result->before;
  }

  result->adminStepNeeded = result->plan.machine.has_value() && !machineRepaired;
  result->code = CheckExitCode(result->findings, AnyFailed(result->repairs),
                               result->adminStepNeeded, options.quiet);
  if (result->elevated && result->elevated->outcome == ElevatedOutcome::kCancelled) {
    result->code = ExitCode::kCancelled;
  } else if (result->elevated && LackedAdministratorRights(*result->elevated)) {
    result->code = ExitCode::kNeedsAdmin;
  } else if (result->elevated && result->elevated->outcome == ElevatedOutcome::kFailed) {
    result->code = ExitCode::kFailed;
  }
  result->summary = CheckSummary(result->code, fixed, CountBreaking(result->findings));

  CheckRecord record;
  GetSystemTimeAsFileTime(&record.time);
  record.result = result->code;
  record.summary = result->summary;
  const Snapshot& after = result->after;
  record.pdfcraftVersion = after.pdfcraft.installed ? after.pdfcraft.version : std::wstring();
  if (after.live) record.liveTest = after.live->hr;
  result->recordError = RecordCheck(roots, record);
  result->recorded = SUCCEEDED(result->recordError);
  if (!result->recorded) {
    LogLine(LogLevel::kWarning, L"check record failed hr=" + HResultText(result->recordError));
  }

  // Explorer reads associations once and keeps them; it rereads them when told.
  if (slotOrClassChanged) result->notified = NotifyAssocChanged(roots);

  std::wstring line = L"check result=";
  line += CheckResultName(result->code);
  line += L" fixed=" + std::to_wstring(fixed);
  line += L" pdfcraft=" +
          (record.pdfcraftVersion.empty() ? std::wstring(L"none") : record.pdfcraftVersion);
  line += LiveTestLogFields(after);
  line += L" ms=" + std::to_wstring(GetTickCount64() - started);
  // A warning when a step of this program failed; an Explorer test that could not start is one.
  LogLine((AnyFailed(result->repairs) || FAILED(after.liveError)) ? LogLevel::kWarning
                                                                  : LogLevel::kInfo,
          line);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
