#include "check_schedule.h"

#include <wil/result_macros.h>

#include <string>
#include <utility>

#include "evaluate.h"
#include "instance.h"
#include "log.h"
#include "strings.h"

namespace pdft {
namespace {

constexpr wchar_t kLogComponent[] = L"checks";

const wchar_t* OnOff(bool on) noexcept { return on ? L"on" : L"off"; }

// The log line: what was asked, what came of it and where the task is now. Never the SID.
void LogResult(const ScheduleOptions& options, const ScheduleResult& result) noexcept try {
  std::wstring message = L"checks after-pdfcraft=";
  message += OnOff(options.afterPdfCraft);
  message += L" at-sign-in=";
  message += OnOff(options.atSignIn);
  message += FAILED(result.error) ? L" result=failed hr=" + HResultText(result.error)
                                  : std::wstring(L" result=done");
  message += L" task=";
  message += !result.after.exists ? L"none" : result.after.atFallback ? L"root-folder" : L"folder";
  Log(FAILED(result.error) ? LogLevel::kWarning : LogLevel::kInfo, kLogComponent, message);
} catch (...) {
  // Logging is best effort.
}

}  // namespace

HRESULT ChangeCheckSchedule(const ScheduleOptions& options, const RegistryRoots& roots,
                            const SystemProbes& probes, ScheduleResult* result) noexcept try {
  *result = ScheduleResult{};
  result->askedAfterPdfCraft = options.afterPdfCraft;
  result->askedAtSignIn = options.atSignIn;
  ProcessContext context;
  if (options.context) {
    context = *options.context;
  } else {
    RETURN_IF_FAILED(probes.GetProcessContext(ContextProbe::kWrite, &context));
  }
  result->context = context;
  if (context.probe != ContextProbe::kWrite || RefusesWrites(context.virtualization)) {
    result->refusal = ScheduleRefusal::kWrongContext;
    result->code = ExitCode::kWrongContext;
    return S_OK;
  }
  if (context.elevated) {
    result->refusal = ScheduleRefusal::kElevated;
    result->code = ExitCode::kWrongContext;
    return S_OK;
  }

  std::optional<OperationLock> lock;
  const HRESULT locked = OperationLock::Acquire(EffectiveLockWaitMs(options.lockWaitMs), &lock);
  if (locked == OperationLock::kBusy || locked == OperationLock::kTakenByLowerIntegrity) {
    result->busy = true;
    result->code = ExitCode::kBusy;
    Log(LogLevel::kWarning, kLogComponent,
        locked == OperationLock::kBusy ? L"checks result=busy"
                                       : L"checks result=busy lock-taken-by-lower-integrity");
    return S_OK;
  }
  RETURN_IF_FAILED(locked);

  GatherOptions gather;
  gather.liveTest = false;
  gather.context = context;
  gather.paths = options.paths;
  RETURN_IF_FAILED(GatherSnapshot(roots, gather, probes, &result->before));
  const Snapshot& s = result->before;
  if (HasFinding(Evaluate(s), FindingId::kNotInstalled)) {
    result->refusal = ScheduleRefusal::kNotInstalled;
    result->code = ExitCode::kNotInstalled;
    Log(LogLevel::kInfo, kLogComponent, L"checks result=not-installed");
    return S_OK;
  }
  // The task's place comes from the user's SID; without it there is nowhere to put it.
  RETURN_HR_IF(E_INVALIDARG, s.taskLocation.name.empty());

  const TaskOptions wanted = OptionsFromCheckboxes(options.afterPdfCraft, options.atSignIn);
  result->remove = HasNoTriggers(wanted);
  if (!result->remove) {
    const std::optional<std::filesystem::path> program = InstalledSettingsExe(s);
    if (!program) {
      result->refusal = ScheduleRefusal::kNoSettingsExe;
      result->code = ExitCode::kNotInstalled;
      Log(LogLevel::kInfo, kLogComponent, L"checks result=no-settings-program");
      return S_OK;
    }
    result->program = *program;
  }

  result->error = probes.ApplyTaskOptions(s.taskLocation, wanted, result->program, context);
  result->readError = probes.ReadTask(s.taskLocation, &result->after);
  if (FAILED(result->readError)) result->after = {};
  result->code = FAILED(result->error) ? ExitCode::kFailed : ExitCode::kOk;
  LogResult(options, *result);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
