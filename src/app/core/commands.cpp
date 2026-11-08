#include "commands.h"

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string_view>
#include <vector>

#include "check.h"
#include "check_schedule.h"
#include "diagnostics.h"
#include "elevation.h"
#include "evaluate.h"
#include "install.h"
#include "instance.h"
#include "json.h"
#include "live_test.h"
#include "log.h"
#include "paths.h"
#include "project_config.h"
#include "rebuild.h"
#include "refresh.h"
#include "registration.h"
#include "registry_roots.h"
#include "settings.h"
#include "setup.h"
#include "snapshot.h"
#include "status_format.h"
#include "strings.h"
#include "system_probes.h"
#include "uninstall.h"
#include "zoom.h"

namespace pdft {
namespace {

constexpr int Code(ExitCode code) { return static_cast<int>(code); }

constexpr wchar_t kBusyMessage[] =
    L"Another check or change is running and did not finish within 30 seconds; try again later.";

// What the messages for kNamedObjectsRefusedHr give as the likely causes.
constexpr wchar_t kNamedObjectsRefusedCauses[] =
    L"This happens, for example, to a program started as another account inside someone else's "
    L"session (run it as the account signed in to that session), or to one running at low "
    L"integrity, in a sandbox or with a restricted token.";

// Says why a command did not get the operation lock, if it did not: another process kept it
// (|busy|, exit 4), or Windows does not let this process make it (|namedObjectsRefused|, exit 5).
// True when it said so.
bool ReportLockNotTaken(bool busy, bool namedObjectsRefused, Output& out) {
  if (busy) {
    out.Error(kBusyMessage);
  } else if (namedObjectsRefused) {
    out.Error(NamedObjectsRefusedMessage());
  } else {
    return false;
  }
  return true;
}

// The log's word for a command: its name without the dashes ("zoom"); "window" without one.
std::wstring Component(const CliArgs& args) {
  if (args.verb == Verb::kNone) return L"window";
  std::wstring_view name = Describe(args.verb).name;
  while (name.starts_with(L'-')) name.remove_prefix(1);
  return std::wstring(name);
}

ContextProbe ProbeFor(const CliArgs& args) {
  if (args.writes) return ContextProbe::kWrite;
  if (Describe(args.verb).reportsState) return ContextProbe::kReadOnly;
  return ContextProbe::kNone;
}

const wchar_t* VirtualizationName(Virtualization virtualization) {
  switch (virtualization) {
    case Virtualization::kNo:
      return L"no";
    case Virtualization::kYes:
      return L"yes";
    case Virtualization::kUnknown:
      return L"unknown";
  }
  return L"unknown";
}

const wchar_t* ViewsName(RegistryViews views) {
  switch (views) {
    case RegistryViews::kNotChecked:
      return L"not-checked";
    case RegistryViews::kAgree:
      return L"agree";
    case RegistryViews::kDiffer:
      return L"differ";
    case RegistryViews::kUnknown:
      return L"unknown";
  }
  return L"unknown";
}

// Starts the settings executable next to this one, by full path (never through a search).
HRESULT StartSettingsExecutable() noexcept try {
  std::filesystem::path exe;
  RETURN_IF_FAILED(GetOwnExe(&exe));
  const std::filesystem::path settings = exe.parent_path() / kSettingsExe;
  std::wstring commandLine = L"\"" + settings.native() + L"\"";
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  RETURN_IF_WIN32_BOOL_FALSE(CreateProcessW(settings.c_str(), commandLine.data(), nullptr, nullptr,
                                            FALSE, 0, nullptr, nullptr, &startup, &process));
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return S_OK;
}
CATCH_RETURN();

// The settings executable without a command: one window per user, and never an elevated one.
int OpenSettingsWindow(const ProcessContext& context, bool relaunched, OpenWindowFn openWindow,
                       Output& out) {
  if (context.elevated) {
    // Someone asked for the window: say on screen why none opens. An elevated process logs nothing.
    const std::wstring message = WindowElevatedRefusalMessage();
    out.Error(message);
    MessageBoxW(nullptr, message.c_str(), kProductName, MB_OK | MB_ICONERROR);
    return Code(ExitCode::kWrongContext);
  }
  wil::unique_handle instance;
  bool first = false;
  if (const HRESULT hr = TakeSingleInstance(context.userSid, std::addressof(instance), &first);
      FAILED(hr)) {
    // WIL logs other failures.
    if (hr == kNamedObjectsRefusedHr) {
      Log(LogLevel::kWarning, L"window",
          L"window result=failed named-objects-refused hr=" + HResultText(kNamedObjectsRefusedHr));
      // Someone asked for the window: say on screen why none opens.
      const std::wstring message = WindowNamedObjectsRefusedMessage();
      out.Error(message);
      MessageBoxW(nullptr, message.c_str(), kProductName, MB_OK | MB_ICONERROR);
    }
    return Code(ExitCode::kFailed);
  }
  // Another window is open. Bringing it to the front comes with the window itself.
  if (!first) return Code(ExitCode::kOk);
  return openWindow ? openWindow(WindowStart{context, relaunched}) : Code(ExitCode::kOk);
}

// ---- The check engine's commands --------------------------------------------------------------

// Someone can answer a question: standard input is a console, and that console has a window.
// A program started without a window (a scheduled task, a script's hidden child) still gets a
// console of its own, with nobody in front of it.
bool CanAskOnConsole() noexcept {
  const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  return input != nullptr && input != INVALID_HANDLE_VALUE &&
         GetFileType(input) == FILE_TYPE_CHAR && GetConsoleMode(input, &mode) &&
         GetConsoleWindow() != nullptr;
}

// Asks |question| on the console and reads one line; no answer is no.
bool AskYesNo(Output& out, const std::wstring& question) {
  out.Line(question + L" [y/N]");
  wchar_t answer[64] = {};
  DWORD read = 0;
  if (!ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), answer, ARRAYSIZE(answer) - 1, &read,
                    nullptr)) {
    return false;
  }
  return IsYesAnswer(std::wstring_view(answer, read));
}

// The report's text with Windows line ends, for the clipboard and files.
std::wstring WithCrLf(std::wstring_view text) {
  std::wstring result;
  result.reserve(text.size() + text.size() / 32);
  for (wchar_t c : text) {
    if (c == L'\n') result += L'\r';
    result += c;
  }
  return result;
}

// Prints |text| (UTF-8 JSON) as one line, exactly as the JSON writer made it.
void PrintJson(Output& out, const std::string& text) {
  std::wstring wide;
  if (SUCCEEDED(Utf8ToWide(text, &wide))) out.Json(wide);
}

// Deletes the probe PDFs a crashed run left in %TEMP%, where SweepsProbes says so; best effort.
void SweepProbesUnlessElevated(const ProcessContext& context) noexcept {
  if (SweepsProbes(context)) (void)SweepStaleProbes();
}

int RunStatus(const CliArgs& args, const ProcessContext& context, Output& out) {
  SweepProbesUnlessElevated(context);
  GatherOptions options;
  options.liveTest = !args.noLiveTest;
  options.context = context;
  if (args.testFile) {
    std::filesystem::path resolved;
    TestFileProblem problem = TestFileProblem::kNone;
    (void)ValidateTestFile(*args.testFile, &resolved, &problem);
    if (problem != TestFileProblem::kNone) {
      // The path is the user's: it goes to the console, never to the log.
      out.Error(L"--test-file: the file " + TestFileProblemText(problem) + L": " + *args.testFile);
      return Code(ExitCode::kUsage);
    }
    options.testFile = resolved;
  }
  Snapshot snapshot;
  if (const HRESULT hr = GatherSnapshot(RegistryRoots{}, options, RealSystemProbes(), &snapshot);
      FAILED(hr)) {
    out.Error(L"Cannot read the state of the installation: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  const std::vector<Finding> findings = Evaluate(snapshot);
  const ExitCode code = ExitCodeFromFindings(findings);
  if (args.json) {
    PrintJson(out, StatusJson(snapshot, findings, code));
  } else {
    out.Line(RenderStatus(FormatStatus(snapshot, findings)));
  }
  return Code(code);
}

void PrintFailures(const std::vector<std::wstring>& failures, Output& out) {
  for (const std::wstring& failure : failures) out.Error(L"Not done: " + failure);
}

// The step's own report, indented: the elevated process writes no log of its own.
void PrintReport(const ElevatedRun& run, Output& out) {
  for (const std::wstring& line : run.report) out.Error(L"  " + line);
}

// How an administrator step (|what|, "The administrator step") went. On a failure, and when it is
// done but reported parts it did not complete, the step's own report follows.
void PrintElevatedRun(const ElevatedRun& run, std::wstring_view what, Output& out) {
  switch (run.outcome) {
    case ElevatedOutcome::kDone:
    case ElevatedOutcome::kRestartPending:
      if (PartsNotDone(run)) {
        out.Error(ElevatedDoneText(what, run));
        PrintReport(run, out);
      } else {
        out.Line(ElevatedDoneText(what, run));
      }
      if (ReportChannelTaken(run)) out.Error(ReportChannelTakenMessage());
      return;
    case ElevatedOutcome::kCancelled:
      out.Line(L"The administrator prompt was declined.");
      return;
    case ElevatedOutcome::kFailed:
      break;
  }
  if (LackedAdministratorRights(run)) {
    out.Error(NoAdministratorRightsMessage());
  } else if (RefusedBelowPackage(run)) {
    out.Error(BelowPackageRefusalMessage());
  } else {
    out.Error(std::wstring(what) + L" failed: " + HResultText(run.failure));
  }
  if (ReportChannelTaken(run)) out.Error(ReportChannelTakenMessage());
  PrintReport(run, out);
}

// What --check and --fix print after the check ran, and their exit code.
int ReportCheck(HRESULT hr, const CheckResult& result, const ProcessContext& context, bool quiet,
                Output& out) {
  if (FAILED(hr)) {
    out.Error(L"The check failed: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  if (result.refused) {
    out.Error(result.refusedElevated ? ElevatedRefusalMessage() : CannotRunHereMessage(context));
    return Code(result.code);
  }
  if (ReportLockNotTaken(result.busy, result.namedObjectsRefused, out)) return Code(result.code);
  if (quiet) return Code(result.code);
  for (const RepairOutcome& outcome : result.repairs) out.Line(RepairOutcomeText(outcome));
  if (result.elevated) PrintElevatedRun(*result.elevated, L"The administrator step", out);
  PrintFailures(result.machineNotDone, out);
  if (!result.repairs.empty() || result.elevated) out.Line(L"");
  out.Line(RenderStatus(FormatStatus(result.after, result.findings)));
  if (result.adminStepNeeded && result.plan.machine && !result.elevated) {
    out.Line(L"");
    std::wstring text = L"The machine-wide part needs an administrator to " +
                        ElevatedVerbText(*result.plan.machine) + L". ";
    if (result.adminStepUnavailable) {
      text += L"No " + std::wstring(kSettingsExe) +
              L" is installed for all users to do it; reinstall " + kProductName +
              L" for all users to repair it.";
    } else {
      text += L"--fix does it (it asks for administrator rights).";
    }
    out.Line(text);
  }
  return Code(result.code);
}

int RunCheckCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  SweepProbesUnlessElevated(context);
  CheckOptions options;
  options.repair = args.noRepair ? RepairMode::kNone : RepairMode::kSafe;
  options.liveTest = !args.noLiveTest;
  options.quiet = args.quiet;
  options.context = context;
  CheckResult result;
  const HRESULT hr = RunCheck(options, RegistryRoots{}, RealSystemProbes(), &result);
  return ReportCheck(hr, result, context, args.quiet, out);
}

int RunFixCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  if (args.takeBack && !args.yes && !context.elevated) {
    // Only reached with a console to ask on (TakeBackNeedsYes). Ask only when a slot really
    // belongs to another handler, and name it. An elevated process is refused below without
    // asking first.
    GatherOptions options;
    options.liveTest = false;
    options.context = context;
    Snapshot snapshot;
    if (const HRESULT hr = GatherSnapshot(RegistryRoots{}, options, RealSystemProbes(), &snapshot);
        FAILED(hr)) {
      out.Error(L"Cannot read the state of the installation: " + HResultText(hr));
      return Code(ExitCode::kFailed);
    }
    const std::vector<Finding> findings = Evaluate(snapshot);
    std::vector<std::wstring> taken;
    for (const Finding& finding : findings) {
      if (finding.id == FindingId::kSlotOtherHandler) {
        taken.push_back(L"  " + FindingText(snapshot, finding));
      }
    }
    if (!taken.empty()) {
      out.Line(L"These .pdf thumbnail slots belong to another handler:");
      for (const std::wstring& line : taken) out.Line(line);
      if (!AskYesNo(out, L"Point them at " + std::wstring(kProductName) + L"?")) {
        out.Line(L"Nothing changed.");
        return Code(ExitCode::kCancelled);
      }
    }
  }
  SweepProbesUnlessElevated(context);
  CheckOptions options;
  options.repair = args.takeBack ? RepairMode::kSafeAndTakeBack : RepairMode::kSafe;
  options.context = context;
  // The part for all users: the settings executable installed there, through UAC.
  options.launcher = LaunchElevatedWithShell;
  CheckResult result;
  const HRESULT hr = RunCheck(options, RegistryRoots{}, RealSystemProbes(), &result);
  return ReportCheck(hr, result, context, false, out);
}

int RunChecksCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  if (!args.checks) {
    // The parser makes the value required; this only keeps a caller from passing none.
    out.Error(L"--checks needs a value. Run --help");
    return Code(ExitCode::kUsage);
  }
  ScheduleOptions options;
  options.afterPdfCraft = args.checks->afterPdfCraft;
  options.atSignIn = args.checks->atSignIn;
  options.context = context;
  ScheduleResult result;
  if (const HRESULT hr = ChangeCheckSchedule(options, RegistryRoots{}, RealSystemProbes(), &result);
      FAILED(hr)) {
    out.Error(L"Cannot read the state of the installation: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  if (result.refusal == ScheduleRefusal::kWrongContext) {
    out.Error(CannotRunHereMessage(context));
    return Code(result.code);
  }
  if (result.refusal == ScheduleRefusal::kElevated) {
    out.Error(ChecksElevatedRefusalMessage());
    return Code(result.code);
  }
  if (ReportLockNotTaken(result.busy, result.namedObjectsRefused, out)) return Code(result.code);
  const ScheduleReport report = ScheduleResultReport(result);
  for (const std::wstring& line : report.out) out.Line(line);
  if (!report.error.empty()) out.Error(report.error);
  return Code(result.code);
}

// ---- The installer's steps and the 1.x clean-up --------------------------------------------

// What a refused per-user step says, and its exit code.
int ReportSetupRefusal(SetupRefusal refusal, std::wstring_view command,
                       const ProcessContext& context, Output& out) {
  switch (refusal) {
    case SetupRefusal::kWrongContext:
      out.Error(CannotRunHereMessage(context));
      return Code(ExitCode::kWrongContext);
    case SetupRefusal::kElevated:
      out.Error(SetupElevatedRefusalMessage(command));
      return Code(ExitCode::kWrongContext);
    case SetupRefusal::kNotInstalled:
      out.Error(std::wstring(kProductName) + L" is not installed (no " + kSettingsExe +
                L" in the install folder), so there is nothing to set up for; 1.x, if it is "
                L"there, stays as it is.");
      return Code(ExitCode::kNotInstalled);
    case SetupRefusal::kNotElevated:
      out.Error(std::wstring(command) +
                L" sets up this PC for all users and must run as administrator.");
      return Code(ExitCode::kNeedsAdmin);
    case SetupRefusal::kPackaged:
      out.Error(
          L"Cannot run here: started below an app package (or unable to read the processes above "
          L"it), and the test values it wrote under HKEY_LOCAL_MACHINE and in this account's "
          L"registry did not come back through WMI, so its changes might not reach Windows. Run "
          L"the installer from File Explorer.");
      return Code(ExitCode::kWrongContext);
    case SetupRefusal::kNone:
      break;
  }
  return Code(ExitCode::kFailed);
}

int RunSetupUserCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  SetupUserOptions options;
  options.checks = args.checks;
  options.zoom = args.zoom;
  options.context = context;
  SetupUserResult result;
  if (const HRESULT hr = SetupUser(options, RegistryRoots{}, RealSystemProbes(), &result);
      FAILED(hr)) {
    out.Error(L"Cannot read the state of the installation: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  if (result.refusal != SetupRefusal::kNone) {
    return ReportSetupRefusal(result.refusal, Describe(args.verb).name, context, out);
  }
  if (ReportLockNotTaken(result.busy, result.namedObjectsRefused, out)) return Code(result.code);
  // Quiet when everything went: it runs at sign-in.
  PrintFailures(result.failures, out);
  return Code(result.code);
}

int RunSetupMachineCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  SetupMachineResult result;
  if (const HRESULT hr = SetupMachine(SetupMachineOptions{}, RegistryRoots{}, &result);
      FAILED(hr)) {
    out.Error(L"Cannot set up this PC: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  if (result.refusal != SetupRefusal::kNone) {
    const int code = ReportSetupRefusal(result.refusal, Describe(args.verb).name, context, out);
    // What the step found above it and what its test writes showed: elevated, it logs nothing.
    if (result.refusal == SetupRefusal::kPackaged) {
      out.Error(L"  " + MachineWriteCheckText(result.machineWrites));
    }
    return code;
  }
  PrintFailures(result.failures, out);
  if (result.v1.restartPending || result.old.restartPending) {
    out.Line(L"Some old files are in use; Windows deletes them at the next restart.");
  }
  // The step runs elevated and logs nothing, so this is the only trace of why nothing old went.
  if (result.v1.classForeign || result.old.classForeign) {
    out.Line(
        L"The class for all users names its DLL with environment variables (REG_EXPAND_SZ), so "
        L"no old build folder was removed.");
  }
  return Code(result.code);
}

// The lines --migrate-v1 prints about the user part.
void PrintUserMigration(const V1UserResult& user, Output& out) {
  if (user.runOnceDeleted > 0) out.Line(L"Deleted 1.x's pending sign-in clean-up.");
  if (user.zoomMigrated) {
    out.Line(L"Kept 1.x's zoom: " + std::to_wstring(*user.zoomMigrated) + L" %.");
  }
  if (user.taskRegistered) {
    out.Line(L"Registered the check task (after " + std::wstring(kPdfCraftProductName) +
             L" updates, at sign-in).");
  }
  if (user.legacyTaskDeleted) out.Line(L"Deleted 1.x's task " + std::wstring(kLegacyTask) + L".");
  switch (user.userClass) {
    case V1UserResult::ClassChange::kRepointed:
      out.Line(L"Pointed the per-user class at this version.");
      break;
    case V1UserResult::ClassChange::kDeleted:
      out.Line(L"Removed the per-user 1.x class (the class for all users applies).");
      break;
    case V1UserResult::ClassChange::kKept:
      out.Line(L"Kept the per-user 1.x class: nothing of this version could replace it.");
      break;
    case V1UserResult::ClassChange::kNone:
      break;
  }
  const size_t removed = static_cast<size_t>(std::count_if(
      user.files.begin(), user.files.end(),
      [](const V1FileOutcome& outcome) { return outcome.result == V1FileOutcome::Result::kDone; }));
  if (removed > 0) {
    out.Line(L"Removed " + std::to_wstring(removed) + (removed == 1 ? L" item" : L" items") +
             L" 1.x left in its per-user folder.");
  }
  size_t waiting = 0;
  for (const V1FileOutcome& outcome : user.files) {
    if (outcome.result == V1FileOutcome::Result::kKept &&
        (outcome.reason == V1KeepReason::kInUse || outcome.reason == V1KeepReason::kWaiting)) {
      ++waiting;
    }
  }
  for (const V1Kept& kept : user.plan.kept) {
    if (kept.reason == V1KeepReason::kHeld) ++waiting;
  }
  if (waiting > 0) {
    out.Line(std::to_wstring(waiting) + (waiting == 1 ? L" item is" : L" items are") +
             L" still in use; the next check removes " + (waiting == 1 ? L"it." : L"them."));
  }
  for (const V1Kept& kept : user.plan.kept) {
    if (kept.reason == V1KeepReason::kContentDiffers) {
      out.Line(L"Kept " + kept.path + L": its contents are not what 1.x shipped.");
    }
  }
}

int RunMigrateV1Command(const CliArgs& args, const ProcessContext& context, Output& out) {
  MigrateOptions options;
  options.scope = args.migrateScope;
  options.context = context;
  options.launcher = LaunchElevatedWithShell;
  MigrateResult result;
  if (const HRESULT hr = MigrateV1(options, RegistryRoots{}, RealSystemProbes(), &result);
      FAILED(hr)) {
    out.Error(L"Cannot read the state of the installation: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  if (result.refusal != SetupRefusal::kNone) {
    return ReportSetupRefusal(result.refusal, Describe(args.verb).name, context, out);
  }
  if (ReportLockNotTaken(result.busy, result.namedObjectsRefused, out)) return Code(result.code);
  if (result.userPart) PrintUserMigration(result.user, out);
  if (result.machineNoInstall) {
    out.Line(L"1.x's files for all users stay until " + std::wstring(kProductName) +
             L" is installed for all users, which removes them.");
  }
  if (result.run) {
    switch (result.run->outcome) {
      case ElevatedOutcome::kDone:
        out.Line(L"Removed 1.x's build folders for all users.");
        break;
      case ElevatedOutcome::kRestartPending:
        out.Line(
            L"Removed 1.x's build folders for all users; files in use go at the next "
            L"restart.");
        break;
      case ElevatedOutcome::kCancelled:
        out.Line(L"The administrator prompt was declined; 1.x's files for all users stay.");
        break;
      case ElevatedOutcome::kFailed:
        // Other failures are listed with the rest (PrintFailures); the step's report names the
        // folders.
        if (LackedAdministratorRights(*result.run)) {
          out.Error(NoAdministratorRightsMessage());
          PrintReport(*result.run, out);
          out.Line(L"1.x's files for all users stay.");
        } else if (RefusedBelowPackage(*result.run)) {
          out.Error(BelowPackageRefusalMessage());
          PrintReport(*result.run, out);
          out.Line(L"1.x's files for all users stay.");
        } else {
          out.Error(L"The administrator step failed: " + HResultText(result.run->failure));
          PrintReport(*result.run, out);
        }
        break;
    }
    if (result.run->outcome != ElevatedOutcome::kCancelled && ReportChannelTaken(*result.run)) {
      out.Error(ReportChannelTakenMessage());
    }
  }
  PrintFailures(result.failures, out);
  if (result.code == ExitCode::kOk) out.Line(L"Nothing of 1.x is left to clean up.");
  return Code(result.code);
}

// ---- Ctrl+C and closing the console ---------------------------------------------------------

// The stop source of the running --refresh (or --uninstall --rebuild-thumbnails) and the event set
// once it has finished, for the console's control handler; null while none runs.
// Constant-initialised, as startup.h requires.
constinit std::atomic<std::stop_source*> g_consoleStop{nullptr};
constinit std::atomic<HANDLE> g_workDone{nullptr};
// Handlers that may still be using the two: ~ConsoleStop waits until there are none.
constinit std::atomic<int> g_handlersInside{0};
// Whether StopOnConsoleControl is installed; once it is, it stays for the rest of the process.
constinit std::atomic<bool> g_handlerInstalled{false};

// How long closing the console waits for a running command.
constexpr DWORD kCloseWaitMs = 4000;

// Ctrl+C and Ctrl+Break ask the running command to stop and keep the process alive. Closing the
// console ends the process when this returns, so a rebuild past its point of no return gets a few
// seconds to finish deleting. Signing out and shutting down never get here: Windows sends them
// only to console processes that load neither user32.dll nor gdi32.dll, and both executables
// import user32.dll, so those end the process without notice.
BOOL WINAPI StopOnConsoleControl(DWORD type) noexcept {
  const bool interrupt = type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT;
  if (!interrupt && type != CTRL_CLOSE_EVENT) return FALSE;
  ++g_handlersInside;
  if (std::stop_source* stop = g_consoleStop.load()) stop->request_stop();
  if (type == CTRL_CLOSE_EVENT) {
    if (const HANDLE done = g_workDone.load()) WaitForSingleObject(done, kCloseWaitMs);
  }
  --g_handlersInside;
  return TRUE;
}

// Sends the console's control signals to |stop| while it lives (StopOnConsoleControl; without a
// console there are none). The handler is installed the first time and never removed: a Ctrl+C
// that ends the question's read reaches it on another thread, possibly only after the command has
// returned, and without it Windows would then end the process with STATUS_CONTROL_C_EXIT instead
// of the command's exit code. Going away, it sets |done|, which a handler for the console closing
// waits on, and then waits for any handler still running. |stop| and |done| must outlive it.
class ConsoleStop {
 public:
  ConsoleStop(std::stop_source* stop, HANDLE done) noexcept : done_(done) {
    g_workDone.store(done);
    g_consoleStop.store(stop);
    if (!g_handlerInstalled.load()) {
      g_handlerInstalled.store(SetConsoleCtrlHandler(StopOnConsoleControl, TRUE) != FALSE);
    }
  }
  ConsoleStop(const ConsoleStop&) = delete;
  ConsoleStop& operator=(const ConsoleStop&) = delete;
  ~ConsoleStop() {
    if (done_) SetEvent(done_);
    g_consoleStop.store(nullptr);
    g_workDone.store(nullptr);
    // A handler that read them before they were cleared may still be using them.
    while (g_handlersInside.load() != 0) Sleep(1);
  }

 private:
  HANDLE done_ = nullptr;
};

// ---- --install, --uninstall, --uninstall-cleanup, --zoom ------------------------------------

int RunInstallCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  InstallOptions options;
  options.scope = args.scope;
  options.checks = args.checks;
  options.zoom = args.zoom;
  options.force = args.force;
  options.context = context;
  options.launcher = LaunchElevatedWithShell;
  InstallResult result;
  if (const HRESULT hr =
          InstallFromOwnFolder(options, RegistryRoots{}, RealSystemProbes(), &result);
      FAILED(hr)) {
    out.Error(L"Cannot install: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  if (result.refusal == InstallRefusal::kWrongContext) {
    out.Error(CannotRunHereMessage(context));
    return Code(result.code);
  }
  if (result.refusal != InstallRefusal::kNone) {
    out.Error(InstallRefusalMessage(result));
    if (result.run) {
      for (const std::wstring& line : result.run->report) out.Error(L"  " + line);
      if (result.run->outcome != ElevatedOutcome::kCancelled && ReportChannelTaken(*result.run)) {
        out.Error(ReportChannelTakenMessage());
      }
    }
    return Code(result.code);
  }
  if (ReportLockNotTaken(result.busy, result.namedObjectsRefused, out)) return Code(result.code);
  if (result.switchedToMachine) {
    out.Line(std::wstring(kProductName) +
             L" is installed for all users, so this updates the install for all users (one "
             L"administrator prompt). To switch to a per-user install, run --uninstall first.");
  }
  if (result.holdersEnded > 0) {
    out.Line(L"Ended " + std::to_wstring(result.holdersEnded) +
             (result.holdersEnded == 1 ? L" thumbnail process" : L" thumbnail processes") +
             L" of this session that held an old build.");
  }
  if (result.run) PrintElevatedRun(*result.run, L"The part for all users", out);
  if (result.run && (result.run->outcome == ElevatedOutcome::kCancelled ||
                     LackedAdministratorRights(*result.run) || RefusedBelowPackage(*result.run))) {
    out.Line(L"Nothing was installed.");
    return Code(result.code);
  }
  const size_t v1Removed = static_cast<size_t>(std::count_if(
      result.v1Files.begin(), result.v1Files.end(),
      [](const V1FileOutcome& outcome) { return outcome.result == V1FileOutcome::Result::kDone; }));
  if (v1Removed > 0) {
    out.Line(L"Removed " + std::to_wstring(v1Removed) + (v1Removed == 1 ? L" item" : L" items") +
             L" 1.x left in the per-user folder.");
  }
  if (result.setup.migration.legacyTaskDeleted) {
    out.Line(L"Deleted 1.x's task " + std::wstring(kLegacyTask) + L".");
  }
  if (result.setup.zoomWritten) {
    out.Line(L"Zoom: " + std::to_wstring(*result.setup.zoomWritten) + L" %.");
  }
  PrintFailures(result.failures, out);
  if (result.code == ExitCode::kFailed && result.after.scope == Scope::kNone) {
    return Code(result.code);
  }
  out.Line(result.scope == Scope::kMachine ? L"Installed for all users."
                                           : L"Installed for this user.");
  out.Line(L"");
  out.Line(RenderStatus(FormatStatus(result.after, result.findings)));
  return Code(result.code);
}

int RunUninstallCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  // Refused before the question: an elevated process could not do the per-user part.
  if (context.elevated) {
    out.Error(UninstallElevatedRefusalMessage());
    return Code(ExitCode::kWrongContext);
  }
  // With --rebuild-thumbnails, from here and for the whole command, Ctrl+C only asks to stop, so a
  // start of File Explorer the check below tries is always ended. It stops the uninstall before
  // it removes anything. Later it only keeps the rebuild from starting, and the uninstall goes
  // on: once the rebuild has ended File Explorer, it finishes and brings File Explorer back.
  std::stop_source stop;
  wil::unique_event_nothrow done;
  std::optional<ConsoleStop> consoleStop;
  // The rebuild's check, once for the whole command: the question names the restart of File
  // Explorer only where the rebuild may run, and a refusal it finds is what the report says
  // about the rebuild.
  std::optional<RebuildPrecheck> check;
  if (args.rebuildThumbnails) {
    (void)done.create(wil::EventOptions::ManualReset);
    consoleStop.emplace(&stop, done.get());
    check.emplace();
    if (const HRESULT hr = PrecheckRebuild(context, RealRebuildSystem(), &*check); FAILED(hr)) {
      check->failure = hr;
    }
  }
  const bool rebuild =
      check && SUCCEEDED(check->failure) && check->refusal == RebuildRefusal::kNone;
  // Before the question, whose read would otherwise wait for an answer.
  if (stop.stop_requested()) {
    out.Line(L"Nothing changed.");
    return Code(ExitCode::kCancelled);
  }
  if (!args.yes && !AskYesNo(out, UninstallQuestion(rebuild))) {
    out.Line(L"Nothing changed.");
    return Code(ExitCode::kCancelled);
  }
  if (stop.stop_requested()) {
    out.Line(L"Nothing changed.");
    return Code(ExitCode::kCancelled);
  }
  UninstallOptions options;
  options.yes = args.yes;
  options.rebuildThumbnails = args.rebuildThumbnails;
  options.rebuildPrecheck = check;
  options.rebuildProgress = [&out](RebuildPhase phase) {
    const std::wstring line = RebuildProgressText(phase);
    if (!line.empty()) out.Line(line);
  };
  options.stop = stop.get_token();
  options.context = context;
  options.launcher = LaunchElevatedWithShell;
  options.runner = RunProgramAndWait;
  UninstallResult result;
  if (const HRESULT hr = Uninstall(options, RegistryRoots{}, RealSystemProbes(), &result);
      FAILED(hr)) {
    out.Error(L"Cannot uninstall: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  switch (result.refusal) {
    case UninstallRefusal::kWrongContext:
      out.Error(CannotRunHereMessage(context));
      return Code(result.code);
    case UninstallRefusal::kElevated:
      out.Error(UninstallElevatedRefusalMessage());
      return Code(result.code);
    case UninstallRefusal::kNotInstalled:
      out.Error(std::wstring(kProductName) +
                L" is not installed here; there is nothing to remove.");
      return Code(result.code);
    case UninstallRefusal::kCancelled:
      out.Line(L"Nothing changed.");
      return Code(result.code);
    case UninstallRefusal::kNone:
      break;
  }
  if (ReportLockNotTaken(result.busy, result.namedObjectsRefused, out)) return Code(result.code);
  if (result.inno) {
    if (SUCCEEDED(result.innoRun)) {
      out.Line(std::wstring(kProductName) +
               L" was installed by its installer, so its uninstaller ran (exit code " +
               std::to_wstring(result.innoExit) + L").");
    } else if (result.code == ExitCode::kCancelled) {
      out.Line(L"The installer's uninstaller did not start: it was cancelled.");
    }
    if (args.rebuildThumbnails) {
      out.Line(
          L"The thumbnail cache was not rebuilt: the installer's uninstaller does not "
          L"rebuild it.");
    }
    PrintFailures(result.failures, out);
    return Code(result.code);
  }
  for (const std::wstring& line : UninstallReport(result)) out.Line(line);
  PrintFailures(result.failures, out);
  return Code(result.code);
}

int RunUninstallCleanupCommand(Output& out) {
  // The uninstaller's step: whatever it can, never a question, never a failure.
  UninstallCleanupResult result;
  const HRESULT hr =
      UninstallCleanup(UninstallCleanupOptions{}, RegistryRoots{}, RealSystemProbes(), &result);
  Log(FAILED(hr) || !result.failures.empty() ? LogLevel::kWarning : LogLevel::kInfo,
      L"uninstall-cleanup",
      L"uninstall-cleanup hr=" + HResultText(hr) + L" machine=" +
          (result.machinePart ? L"yes" : L"no") + L" user=" + (result.userPart ? L"yes" : L"no") +
          L" failures=" + std::to_wstring(result.failures.size()));
  // Elevated, nothing is logged: what it left out goes to its error output, which an installer
  // can keep in its own log.
  for (const std::wstring& line : UninstallCleanupErrors(hr, result)) out.Error(line);
  return Code(ExitCode::kOk);
}

// How often a refresh prints where it is, at most.
constexpr ULONGLONG kRefreshProgressEveryMs = 5000;

// A progress callback that prints RefreshProgressText at most every kRefreshProgressEveryMs.
RefreshProgressFn RefreshProgressPrinter(Output& out) {
  return [&out, lastLine = std::optional<ULONGLONG>()](const RefreshProgress& p) mutable {
    const ULONGLONG now = GetTickCount64();
    if (lastLine && now - *lastLine < kRefreshProgressEveryMs) return;
    const std::wstring line = RefreshProgressText(p);
    if (line.empty()) return;
    out.Line(line);
    lastLine = now;
  };
}

// --zoom <n> once the zoom is saved, where kPdfRefreshDefault is on: the PDF thumbnails File
// Explorer already made are refreshed in the default places, under the lock the caller holds.
int RefreshAfterZoom(const CliArgs& args, const ProcessContext& context, bool changed,
                     Output& out) {
  const int zoom = *args.zoom;
  if (args.noRefresh) {
    out.Line(ZoomSavedText(zoom, true));
    return Code(ExitCode::kOk);
  }
  if (!changed) {
    out.Line(ZoomUnchangedText(zoom));
    return Code(ExitCode::kOk);
  }
  RefreshConditions conditions;
  const HRESULT read =
      ReadRefreshConditions(context, RegistryRoots{}, SystemAssociationQueries(), &conditions);
  const RefreshRefusal refusal =
      FAILED(read) ? RefreshRefusal::kVirtualized : DecideRefreshRefusal(conditions);
  if (refusal != RefreshRefusal::kNone) {
    Log(LogLevel::kInfo, L"zoom",
        std::wstring(L"zoom refresh=skipped refused=") + RefreshRefusalToken(refusal));
    out.Line(ZoomSavedText(zoom, true));
    return Code(ExitCode::kOk);
  }

  std::stop_source stop;
  wil::unique_event_nothrow done;
  (void)done.create(wil::EventOptions::ManualReset);
  ConsoleStop consoleStop(&stop, done.get());
  RefreshCounts counts;
  const HRESULT hr = RefreshPdfThumbnails(PdfRefreshOptions{}, RefreshProgressPrinter(out),
                                          stop.get_token(), &counts);
  const ExitCode code = RefreshExitCode(hr, counts);
  switch (code) {
    case ExitCode::kOk:
      out.Line(ZoomRefreshedText(zoom, counts.refreshed, counts.filesRefreshed));
      break;
    case ExitCode::kCancelled:
      out.Line(ZoomRefreshStoppedText(zoom, counts.refreshed));
      break;
    case ExitCode::kFailed:
      out.Line(ZoomSavedText(zoom, true));
      out.Error(RefreshFailureText(hr));
      break;
    default:
      out.Line(L"Zoom set to " + std::to_wstring(ClampZoom(zoom)) + L" %.");
      for (const std::wstring& line : RefreshReport(hr, counts, counts.files, true)) {
        out.Line(line);
      }
      break;
  }
  return Code(code);
}

int RunZoomCommandWith(const CliArgs& args, const ProcessContext& context, bool refreshDefault,
                       Output& out) {
  if (!args.zoom) {
    Settings settings;
    ZoomDetails details;
    if (const HRESULT hr = LoadSettings(RegistryRoots{}, &settings, &details); FAILED(hr)) {
      out.Error(L"Cannot read the zoom: " + HResultText(hr));
      return Code(ExitCode::kFailed);
    }
    out.Line(ZoomText(details.decision));
    return Code(ExitCode::kOk);
  }
  // The zoom is the signed-in user's own setting.
  if (context.elevated) {
    out.Error(
        L"Cannot run here: running as administrator. The zoom is the signed-in user's own "
        L"setting, which an administrator process does not write; run --zoom from a normal (not "
        L"administrator) prompt.");
    return Code(ExitCode::kWrongContext);
  }
  std::optional<OperationLock> lock;
  const HRESULT locked = OperationLock::Acquire(kCheckLockWaitMs, &lock);
  if (locked == OperationLock::kBusy || locked == OperationLock::kTakenByLowerIntegrity) {
    out.Error(kBusyMessage);
    return Code(ExitCode::kBusy);
  }
  if (locked == kNamedObjectsRefusedHr) {
    Log(LogLevel::kWarning, L"zoom",
        L"zoom result=failed named-objects-refused hr=" + HResultText(kNamedObjectsRefusedHr));
    out.Error(NamedObjectsRefusedMessage());
    return Code(ExitCode::kFailed);
  }
  if (FAILED(locked)) {
    out.Error(L"Cannot save the zoom: " + HResultText(locked));
    return Code(ExitCode::kFailed);
  }
  if (lock->sessionOnly()) Log(LogLevel::kWarning, L"zoom", L"zoom lock=session-only");
  bool changed = true;
  if (Settings before; SUCCEEDED(LoadSettings(RegistryRoots{}, &before))) {
    changed = before.zoom != ClampZoom(*args.zoom);
  }
  if (const HRESULT hr = SaveZoom(RegistryRoots{}, *args.zoom); FAILED(hr)) {
    out.Error(L"Cannot save the zoom: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  (void)NotifyAssocChanged(RegistryRoots{});
  Log(LogLevel::kInfo, L"zoom", L"zoom saved=" + std::to_wstring(*args.zoom));
  if (refreshDefault) return RefreshAfterZoom(args, context, changed, out);
  out.Line(ZoomSavedText(*args.zoom, false));
  return Code(ExitCode::kOk);
}

int RunZoomCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  return RunZoomCommandWith(args, context, kPdfRefreshDefault, out);
}

// ---- --refresh ------------------------------------------------------------------------------

// --refresh --full: the refusals first (PrecheckRebuild: they read the context, AutoRestartShell
// and, where Windows does not restart File Explorer, whether this program could), then the
// question, the operation lock and the rebuild (rebuild.h).
int RunRefreshFullCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  // From here Ctrl+C only asks to stop, so a start of File Explorer the check below tries is
  // always ended; it stops the command until the rebuild ends the first process or deletes the
  // first file.
  std::stop_source stop;
  wil::unique_event_nothrow done;
  (void)done.create(wil::EventOptions::ManualReset);
  ConsoleStop consoleStop(&stop, done.get());
  // Stopped by Ctrl+C before the rebuild started, so nothing changed.
  const auto stopped = [&out] {
    Log(LogLevel::kWarning, L"refresh",
        std::wstring(L"refresh full refused=") + RebuildRefusalToken(RebuildRefusal::kCancelled));
    out.Line(L"Nothing changed.");
    return Code(ExitCode::kCancelled);
  };

  // Refused before the question: what the context decides, then whether Windows restarts File
  // Explorer by itself, which a package above this process needs, and where it does not, whether
  // this program could start File Explorer outside every job.
  RebuildPrecheck check;
  if (const HRESULT hr = PrecheckRebuild(context, RealRebuildSystem(), &check); FAILED(hr)) {
    out.Error(RebuildFailureText(hr));
    return Code(ExitCode::kFailed);
  }
  if (FAILED(check.failure)) {
    Log(LogLevel::kWarning, L"refresh", RebuildPrecheckLogText(check));
    out.Error(RebuildFailureText(check.failure));
    return Code(ExitCode::kFailed);
  }
  if (check.refusal != RebuildRefusal::kNone) {
    Log(LogLevel::kWarning, L"refresh", RebuildPrecheckLogText(check));
    out.Error(RefreshFullRefusalMessage(check.refusal, context, check.startError));
    return Code(RebuildExitCode(RebuildNotStarted(check)));
  }
  // Before the question, whose read would otherwise wait for an answer.
  if (stop.stop_requested()) return stopped();

  if (!args.yes && !AskYesNo(out, RebuildQuestion())) {
    out.Line(L"Nothing changed.");
    return Code(ExitCode::kCancelled);
  }
  if (stop.stop_requested()) return stopped();

  std::optional<OperationLock> lock;
  const HRESULT locked = OperationLock::Acquire(kCheckLockWaitMs, &lock);
  // The wait cannot be interrupted, so a Ctrl+C during it is answered once it ends, whatever
  // it gave.
  if (stop.stop_requested()) return stopped();
  if (locked == OperationLock::kBusy || locked == OperationLock::kTakenByLowerIntegrity) {
    out.Error(kBusyMessage);
    return Code(ExitCode::kBusy);
  }
  if (locked == kNamedObjectsRefusedHr) {
    Log(LogLevel::kWarning, L"refresh",
        L"refresh result=failed named-objects-refused hr=" + HResultText(kNamedObjectsRefusedHr));
    out.Error(NamedObjectsRefusedMessage());
    return Code(ExitCode::kFailed);
  }
  if (FAILED(locked)) {
    out.Error(RebuildFailureText(locked));
    return Code(ExitCode::kFailed);
  }
  if (lock->sessionOnly()) Log(LogLevel::kWarning, L"refresh", L"refresh lock=session-only");
  if (lock->abandoned()) Log(LogLevel::kWarning, L"refresh", L"refresh lock=abandoned");

  RebuildOptions options;
  options.context = context;
  const auto progress = [&out](RebuildPhase phase) {
    const std::wstring line = RebuildProgressText(phase);
    if (!line.empty()) out.Line(line);
  };
  RebuildResult result;
  if (const HRESULT hr =
          RebuildThumbnailCache(options, RealRebuildSystem(), progress, stop.get_token(), &result);
      FAILED(hr)) {
    out.Error(RebuildFailureText(hr));
    return Code(ExitCode::kFailed);
  }
  if (result.refusal == RebuildRefusal::kCancelled) {
    out.Line(L"Nothing changed.");
  } else if (result.refusal != RebuildRefusal::kNone) {
    out.Error(RefreshFullRefusalMessage(result.refusal, context, result.startError));
  } else if (FAILED(result.failure)) {
    out.Error(RebuildFailureText(result.failure));
  } else {
    for (const std::wstring& line : RebuildReport(result)) out.Line(line);
  }
  return Code(RebuildExitCode(result));
}

// --refresh, with or without --folder: the refusals first (ReadRefreshConditions), then each folder
// checked, the operation lock, the refusals again, and the refresh of the PDF thumbnails in those
// folders or, with none, in the default places (refresh.h), with a progress line at most every
// kRefreshProgressEveryMs.
int RunRefreshFoldersCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  // From here Ctrl+C only asks to stop: before the refresh starts it ends the command, nothing
  // changed; during the refresh, the request under way finishes first.
  std::stop_source stop;
  wil::unique_event_nothrow done;
  (void)done.create(wil::EventOptions::ManualReset);
  ConsoleStop consoleStop(&stop, done.get());
  // Stopped by Ctrl+C before the refresh started, so nothing changed.
  const auto stopped = [&out] {
    Log(LogLevel::kWarning, L"refresh", L"refresh refused=cancelled");
    out.Line(L"Nothing changed.");
    return Code(ExitCode::kCancelled);
  };

  // The exit code when the refresh may not start; std::nullopt when it may.
  const auto refused = [&context, &out]() -> std::optional<int> {
    RefreshConditions conditions;
    if (const HRESULT hr = ReadRefreshConditions(context, RegistryRoots{},
                                                 SystemAssociationQueries(), &conditions);
        FAILED(hr)) {
      out.Error(RefreshFailureText(hr));
      return Code(ExitCode::kFailed);
    }
    const RefreshRefusal refusal = DecideRefreshRefusal(conditions);
    if (refusal == RefreshRefusal::kNone) return std::nullopt;
    const ExitCode code = RefreshRefusalExitCode(refusal);
    Log(code == ExitCode::kOk ? LogLevel::kInfo : LogLevel::kWarning, L"refresh",
        std::wstring(L"refresh refused=") + RefreshRefusalToken(refusal));
    // Icons only is no failure: there is simply nothing to refresh.
    if (code == ExitCode::kOk) {
      out.Line(RefreshFolderRefusalMessage(refusal, context));
    } else {
      out.Error(RefreshFolderRefusalMessage(refusal, context));
    }
    return Code(code);
  };
  if (const std::optional<int> code = refused()) return *code;

  PdfRefreshOptions options;
  for (const std::wstring& text : args.folders) {
    std::filesystem::path folder;
    FolderProblem problem = FolderProblem::kNone;
    const HRESULT hr = ValidateRefreshFolder(text, &folder, &problem);
    // The path is the user's: it goes to the console, never to the log.
    if (problem != FolderProblem::kNone) {
      out.Error(FolderProblemText(text, problem));
      return Code(ExitCode::kUsage);
    }
    if (FAILED(hr)) {
      out.Error(RefreshFailureText(hr));
      return Code(ExitCode::kFailed);
    }
    options.folders.push_back(std::move(folder));
  }
  if (stop.stop_requested()) return stopped();

  std::optional<OperationLock> lock;
  const HRESULT locked = OperationLock::Acquire(kCheckLockWaitMs, &lock);
  // The wait cannot be interrupted, so a Ctrl+C during it is answered once it ends, whatever
  // it gave.
  if (stop.stop_requested()) return stopped();
  if (locked == OperationLock::kBusy || locked == OperationLock::kTakenByLowerIntegrity) {
    out.Error(kBusyMessage);
    return Code(ExitCode::kBusy);
  }
  if (locked == kNamedObjectsRefusedHr) {
    Log(LogLevel::kWarning, L"refresh",
        L"refresh result=failed named-objects-refused hr=" + HResultText(kNamedObjectsRefusedHr));
    out.Error(NamedObjectsRefusedMessage());
    return Code(ExitCode::kFailed);
  }
  if (FAILED(locked)) {
    out.Error(RefreshFailureText(locked));
    return Code(ExitCode::kFailed);
  }
  if (lock->sessionOnly()) Log(LogLevel::kWarning, L"refresh", L"refresh lock=session-only");
  if (lock->abandoned()) Log(LogLevel::kWarning, L"refresh", L"refresh lock=abandoned");
  // Decided again under the lock: a command that held it meanwhile, an uninstall or
  // --fix --take-back, may have changed the registration or the handler Windows uses for .pdf.
  if (const std::optional<int> code = refused()) return *code;

  RefreshCounts counts;
  const HRESULT hr =
      RefreshPdfThumbnails(options, RefreshProgressPrinter(out), stop.get_token(), &counts);
  const ExitCode code = RefreshExitCode(hr, counts);
  if (code == ExitCode::kFailed) {
    out.Error(RefreshFailureText(hr));
    return Code(code);
  }
  for (const std::wstring& line :
       RefreshReport(hr, counts, options.folders.empty() ? counts.files : options.limits.maxFiles,
                     kPdfRefreshDefault)) {
    out.Line(line);
  }
  return Code(code);
}

int RunRefreshCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  if (args.full) return RunRefreshFullCommand(args, context, out);
  return RunRefreshFoldersCommand(args, context, out);
}

HRESULT CopyToClipboard(std::wstring_view text) noexcept {
  // Another program may hold the clipboard for a moment.
  bool opened = false;
  for (int attempt = 0; attempt < 10 && !opened; ++attempt) {
    opened = OpenClipboard(nullptr) != FALSE;
    if (!opened) Sleep(100);
  }
  RETURN_LAST_ERROR_IF(!opened);
  auto close = wil::scope_exit([] { CloseClipboard(); });
  RETURN_IF_WIN32_BOOL_FALSE(EmptyClipboard());
  const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
  wil::unique_hglobal memory(GlobalAlloc(GMEM_MOVEABLE, bytes));
  RETURN_LAST_ERROR_IF_NULL(memory);
  auto* buffer = static_cast<wchar_t*>(GlobalLock(memory.get()));
  RETURN_LAST_ERROR_IF_NULL(buffer);
  memcpy(buffer, text.data(), text.size() * sizeof(wchar_t));
  buffer[text.size()] = L'\0';
  GlobalUnlock(memory.get());
  RETURN_LAST_ERROR_IF_NULL(SetClipboardData(CF_UNICODETEXT, memory.get()));
  memory.release();  // the clipboard owns it now
  return S_OK;
}

// Writes |text| to |path| as UTF-8 with a byte order mark. *refused when the path is not a file
// the report may go to (a folder, a link, a device): a usage error rather than a failure.
HRESULT WriteReportFile(const std::wstring& path, std::wstring_view text, bool* refused) noexcept
    try {
  *refused = false;
  if (TestFilePathProblem(path) != TestFileProblem::kNone) {
    *refused = true;
    return E_INVALIDARG;
  }
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    *refused = true;
    return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
  }
  // A link made in the meantime is replaced, never followed. A path that names a pipe gives its
  // server an identification token at most.
  wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                     CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                                         SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                                     nullptr));
  RETURN_LAST_ERROR_IF(!file);
  if (GetFileType(file.get()) != FILE_TYPE_DISK) {
    // "CON", "NUL" and the like open a device; nothing was truncated.
    *refused = true;
    return HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE);
  }
  const std::string bytes = "\xEF\xBB\xBF" + WideToUtf8Lossy(text);
  size_t written = 0;
  while (written < bytes.size()) {
    DWORD chunk = 0;
    RETURN_IF_WIN32_BOOL_FALSE(WriteFile(file.get(), bytes.data() + written,
                                         static_cast<DWORD>(bytes.size() - written), &chunk,
                                         nullptr));
    written += chunk;
  }
  return S_OK;
}
CATCH_RETURN();

int RunDiagnostics(const CliArgs& args, const ProcessContext& context, Output& out) {
  // An administrator process writes no file where the user's unelevated programs could redirect
  // it; refused before anything is read. Standard output and --copy remain.
  if (context.elevated && args.out) {
    out.Error(DiagnosticsOutElevatedRefusalMessage());
    return Code(ExitCode::kWrongContext);
  }
  // The report never starts a thumbnail process; it shows the last recorded Explorer test.
  GatherOptions options;
  options.liveTest = false;
  options.context = context;
  Snapshot snapshot;
  if (const HRESULT hr = GatherSnapshot(RegistryRoots{}, options, RealSystemProbes(), &snapshot);
      FAILED(hr)) {
    out.Error(L"Cannot read the state of the installation: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  const std::vector<Finding> findings = Evaluate(snapshot);
  const std::vector<std::wstring> tail = DiagnosticsLogTail(context);
  RedactContext redact;
  (void)GetRedactContext(context, &redact);
  const std::wstring report = BuildDiagnostics(snapshot, findings, tail, redact);

  if (args.copy) {
    if (const HRESULT hr = CopyToClipboard(WithCrLf(report)); FAILED(hr)) {
      out.Error(L"Cannot put the report on the clipboard: " + HResultText(hr));
      return Code(ExitCode::kFailed);
    }
    out.Line(L"Diagnostics copied to the clipboard; paste them into a GitHub issue.");
    return Code(ExitCode::kOk);
  }
  if (args.out) {
    bool refused = false;
    if (const HRESULT hr = WriteReportFile(*args.out, WithCrLf(report), &refused); FAILED(hr)) {
      if (refused) {
        out.Error(L"--out must name a file, not a folder, a link or a device: " + *args.out);
        return Code(ExitCode::kUsage);
      }
      out.Error(L"Cannot write " + *args.out + L": " + HResultText(hr));
      return Code(ExitCode::kFailed);
    }
    out.Line(L"Diagnostics written to " + *args.out);
    return Code(ExitCode::kOk);
  }
  out.Line(report);
  return Code(ExitCode::kOk);
}

}  // namespace

std::wstring VersionText() {
  return std::wstring(kProductName) + L" " + kProductVersion + L" (" + kArch + L")";
}

std::wstring CannotRunHereMessage(const ProcessContext& context) {
  std::wstring message = L"Cannot run here: ";
  if (!context.packagedAncestor && context.virtualization == Virtualization::kUnknown) {
    // A normal PC where the check itself failed (WMI not answering, say): there is no
    // container to leave, so no advice about where else to run it.
    message += L"the registry check (a test value written here and read back through WMI) failed";
    if (FAILED(context.probeError)) message += L" with " + HResultText(context.probeError);
    message += L", so it cannot be confirmed that changes made from here would reach Windows.";
    return message;
  }
  if (context.packagedAncestor) {
    message += L"started from the packaged app " + *context.packagedAncestor + L"; ";
  }
  message += context.virtualization == Virtualization::kYes
                 ? L"registry virtualized"
                 : L"registry virtualization could not be ruled out";
  message +=
      L". Changes made from here would not reach Windows. Run it from File Explorer or from a "
      L"terminal that is not part of an app package.";
  return message;
}

std::wstring ElevatedRefusalMessage() {
  return L"Cannot run here: running as administrator. The repairs change only this user's own "
         L"settings and files, which an administrator process does not write; run it from a "
         L"normal (not administrator) prompt.";
}

std::wstring ChecksElevatedRefusalMessage() {
  return L"Cannot run here: running as administrator. The check task belongs to the signed-in "
         L"user and runs with their own rights; run --checks from a normal (not administrator) "
         L"prompt.";
}

std::wstring DiagnosticsOutElevatedRefusalMessage() {
  return L"Cannot run here: running as administrator. --diagnostics --out writes a file into "
         L"folders the signed-in user's own programs can change and redirect, which an "
         L"administrator process does not do; run it from a normal (not administrator) prompt, or "
         L"use --copy.";
}

std::wstring SetupElevatedRefusalMessage(std::wstring_view command) {
  return L"Cannot run here: running as administrator. " + std::wstring(command) +
         L" changes the signed-in user's own settings, which an administrator process does not "
         L"write; run it from a normal (not administrator) prompt. Where a part for all users "
         L"needs an administrator, it asks for one itself.";
}

std::wstring InstallRefusalMessage(const InstallResult& result) {
  const std::wstring product = kProductName;
  const std::wstring folder = result.source.dir.native();
  switch (result.refusal) {
    case InstallRefusal::kElevated:
      return L"Cannot run here: running as administrator. --install also sets up the signed-in "
             L"user, which an administrator process does not do; run it from a normal (not "
             L"administrator) prompt. It asks for administrator rights itself for the part for "
             L"all users.";
    case InstallRefusal::kInnoInstall:
      return L"Cannot install here: " + product + L" is installed by its installer (" +
             (result.inno && result.inno->scope == Scope::kUser ? L"for this user"
                                                                : L"for all users") +
             L"). Update it with a newer installer, or remove it in Settings > Apps first.";
    case InstallRefusal::kNotABuild: {
      const SourceCheck& check = result.sourceCheck;
      std::wstring text = L"Cannot install from " + folder + L": ";
      switch (check.problem) {
        case SourceProblem::kMissingFile:
          text += check.file + L" is not there. Run --install from the folder of a build.";
          break;
        case SourceProblem::kWrongArch:
          text += check.file + L" is built for " + ArchName(check.arch) +
                  L", which this Windows cannot use; install the build for this Windows.";
          break;
        case SourceProblem::kNot2x:
          text += check.file + L" is not a build of version 2 or later.";
          break;
        case SourceProblem::kUnreadable:
          text += check.file + L" cannot be read (" + HResultText(check.hr) + L").";
          break;
        case SourceProblem::kNone:
          break;
      }
      return text;
    }
    case InstallRefusal::kInsideRoot:
      return L"Cannot install from " + folder +
             L": it is the install folder itself, or inside it. Run --install from the folder of "
             L"a build.";
    case InstallRefusal::kUnsafeFolder: {
      std::wstring text = L"Cannot install from " + folder + L": ";
      if (result.unsafe) {
        text += UnsafeAccessText(*result.unsafe);
      } else {
        text += L"the step for all users found that other accounts can change it";
      }
      text += result.scope == Scope::kUser
                  ? L", so File Explorer's thumbnail process and your sign-in task would run "
                    L"whatever another account put there."
                  : L", so an administrator would copy and register for every user whatever was "
                    L"put there.";
      return text +
             L" Put the build in a folder only you and administrators can change (inside your "
             L"user profile, for example) and run --install from there.";
    }
    case InstallRefusal::kV1Machine:
      return L"Version 1.x is installed for all users, and File Explorer's thumbnail process would "
             L"go on loading it instead of a per-user install. --install --machine replaces it "
             L"(one administrator prompt); --install --user --force installs for this user "
             L"anyway.";
    case InstallRefusal::kWrongContext:
    case InstallRefusal::kNone:
      break;
  }
  return L"Cannot install.";
}

std::wstring UninstallElevatedRefusalMessage() {
  return L"Cannot run here: running as administrator. --uninstall also removes the signed-in "
         L"user's own settings and files, which an administrator process does not do; run it "
         L"from a normal (not administrator) prompt. It asks for administrator rights itself for "
         L"the part for all users.";
}

std::wstring NoAdministratorRightsMessage() {
  return L"The step for all users did not get administrator rights (Windows started it without "
         L"them), so it changed nothing.";
}

std::wstring BelowPackageRefusalMessage() {
  // True of a step of either build: an earlier one refuses below any package, this one where its
  // test writes did not come back.
  return L"The step for all users changed nothing: started below an app package (or unable to "
         L"read the processes above it), it could not show that its changes would reach Windows. "
         L"Run the command from a Command Prompt opened with Win+R (type cmd), which no app "
         L"package starts.";
}

std::wstring ElevatedDoneText(std::wstring_view what, const ElevatedRun& run) {
  // Without the restart's clause: a part not done may be a file in use that could not be queued
  // for deletion at the restart.
  if (PartsNotDone(run)) {
    return std::wstring(what) + L" is done, but some of its parts did not complete; its report:";
  }
  if (run.outcome == ElevatedOutcome::kRestartPending) {
    return std::wstring(what) +
           L" is done; files that were in use are deleted at the next restart.";
  }
  return std::wstring(what) + L" is done.";
}

std::wstring ReportChannelTakenMessage() {
  return L"The administrator step's report channel was taken by another process, so its own "
         L"report could not come back; the step ran all the same, and its result above comes "
         L"from its exit code.";
}

std::wstring NamedObjectsRefusedMessage() {
  return std::wstring(
             L"Windows does not let this program create named objects here, so it "
             L"cannot take the lock that lets only one check or change run at a time; "
             L"it changed nothing. ") +
         kNamedObjectsRefusedCauses;
}

std::wstring WindowElevatedRefusalMessage() {
  return std::wstring(L"Cannot run here: running as administrator. ") + kProductName +
         L" Settings shows and changes the signed-in user's own settings, which an administrator "
         L"process does not write. Open it normally (not as administrator).";
}

std::wstring WindowNamedObjectsRefusedMessage() {
  return std::wstring(
             L"Windows does not let this program create named objects here, so it "
             L"cannot tell whether its window is open already and does not open it. ") +
         kNamedObjectsRefusedCauses;
}

std::wstring UninstallQuestion(bool rebuildThumbnails) {
  std::wstring question = L"Remove " + std::wstring(kProductName) + L" for all users of this PC";
  if (rebuildThumbnails) question += L", and rebuild the thumbnail cache (restarts File Explorer)";
  return question + L"?";
}

namespace {

// Why the uninstall's rebuild changed nothing, for "The thumbnail cache was not rebuilt: <reason>."
// (the uninstall itself went on, so not "Nothing changed."); empty when it ran.
std::wstring RebuildNotDoneReason(const RebuildResult& rebuild) {
  switch (rebuild.refusal) {
    case RebuildRefusal::kElevated:
      return L"running as administrator, and File Explorer must not run as administrator";
    case RebuildRefusal::kVirtualized:
      return L"it could not be confirmed that changes made from here reach Windows";
    case RebuildRefusal::kOtherShellUser:
      return L"File Explorer on this desktop belongs to another account, and the thumbnail cache "
             L"is each account's own";
    case RebuildRefusal::kNoRestartBelowPackage:
      return L"Windows is set not to restart File Explorer by itself, and this program does not "
             L"start it from inside an app package (the terminal it runs in)";
    case RebuildRefusal::kNoRestartInJob:
      return L"Windows is set not to restart File Explorer by itself, and a File Explorer started "
             L"from here would stay inside a job of the program this runs in";
    case RebuildRefusal::kNoRestartStartFails:
      return L"Windows is set not to restart File Explorer by itself, and this program cannot "
             L"start File Explorer from here either (" +
             HResultText(rebuild.startError) + L")";
    case RebuildRefusal::kOwnProcessHoldsCache:
      return L"this process has the thumbnail cache open itself";
    case RebuildRefusal::kFileOperation:
      return L"File Explorer is copying, moving or deleting files";
    case RebuildRefusal::kCancelled:
      return L"it was stopped before it started";
    case RebuildRefusal::kNone:
      break;
  }
  if (rebuild.failure == HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK)) {
    return L"its folder is reached through a link or junction, which this program does not follow";
  }
  if (FAILED(rebuild.failure)) return HResultText(rebuild.failure);
  return {};
}

}  // namespace

std::vector<std::wstring> UninstallReport(const UninstallResult& result) {
  std::vector<std::wstring> lines;
  const std::wstring product = kProductName;
  if (result.rebuild) {
    if (const std::wstring reason = RebuildNotDoneReason(*result.rebuild); !reason.empty()) {
      lines.push_back(L"The thumbnail cache was not rebuilt: " + reason + L".");
    } else {
      for (std::wstring& line : RebuildReport(*result.rebuild)) lines.push_back(std::move(line));
    }
  }
  if (result.run) {
    switch (result.run->outcome) {
      case ElevatedOutcome::kDone:
        lines.push_back(L"Removed the part for all users.");
        break;
      case ElevatedOutcome::kRestartPending:
        lines.push_back(
            L"Removed the part for all users; files that were in use are deleted at the next "
            L"restart.");
        break;
      case ElevatedOutcome::kCancelled:
        lines.push_back(
            L"The administrator prompt was declined: the part for all users (its class and its "
            L"install folder) stays. Run --uninstall again and approve the prompt to remove it.");
        break;
      case ElevatedOutcome::kFailed:
        if (LackedAdministratorRights(*result.run)) {
          lines.push_back(NoAdministratorRightsMessage() +
                          L" The part for all users (its class and its install folder) stays.");
        } else if (RefusedBelowPackage(*result.run)) {
          lines.push_back(BelowPackageRefusalMessage() +
                          L" The part for all users (its class and its install folder) stays; run "
                          L"--uninstall again from such a prompt to remove it.");
        } else if (result.run->failure == kRegistryTreeLeftHr && !result.machineLeft) {
          // Only keys in other users' hives stayed: the class and the install folders are gone.
          lines.push_back(
              L"Removed the part for all users, but some keys in other signed-in users' "
              L"registry hives were left as they were (a registry link, which is never "
              L"followed, or more keys than one deletion removes).");
        } else {
          lines.push_back(L"The part for all users failed: " + HResultText(result.run->failure));
        }
        for (const std::wstring& line : result.run->report) lines.push_back(L"  " + line);
        break;
    }
    if (result.run->outcome != ElevatedOutcome::kCancelled && ReportChannelTaken(*result.run)) {
      lines.push_back(ReportChannelTakenMessage());
    }
  }
  if (result.machineNoProgram) {
    std::wstring line = L"The part for all users stays: no " + std::wstring(kSettingsExe) +
                        L" is installed for all users to remove it. If version 1.x is installed "
                        L"for all users, its own uninstall removes it";
    line += kDevInstall
                ? L"; otherwise install this version for all users (--install --machine) and "
                  L"run --uninstall again."
                : L"; otherwise reinstall this version for all users and run --uninstall again.";
    lines.push_back(std::move(line));
  }
  if (!result.files.scheduled.empty()) {
    lines.push_back(L"Some files are in use; they are removed when you next sign in.");
  }
  for (const std::filesystem::path& dir : result.files.notScheduled) {
    lines.push_back(L"Some files in " + dir.native() +
                    L" are in use; delete them once they are "
                    L"closed.");
  }
  if (!result.machineLeft && result.failures.empty()) {
    lines.push_back(product + L" removed. PDF files show their app icon again.");
  } else if (result.machineLeft) {
    lines.push_back(
        L"PDF thumbnails are off for you, but the part for all users is still "
        L"installed.");
  }
  return lines;
}

std::vector<std::wstring> UninstallCleanupErrors(HRESULT hr, const UninstallCleanupResult& result) {
  std::vector<std::wstring> lines;
  if (FAILED(hr)) lines.push_back(L"The clean-up stopped early: " + HResultText(hr));
  if (result.machineNoRights) {
    lines.push_back(
        L"The part for all users was left out: this process is elevated, but without full "
        L"administrator rights (the Administrators group enabled and an integrity level of High "
        L"or more).");
  }
  if (result.machineRefused) {
    lines.push_back(
        L"The part for all users was left out: started below an app package (or unable to read "
        L"the processes above it), its test writes did not come back through WMI (" +
        MachineWriteCheckText(result.machineWrites) + L").");
  }
  // Only from a clean-up that went to its end: one that stopped early may not have reached the
  // per-user part at all.
  if (SUCCEEDED(hr) && !result.userPart) {
    std::wstring line =
        L"The part for this account was left out: the registry check (a test value written here "
        L"and read back through WMI) did not show that changes made from here would reach "
        L"Windows (virtualized=";
    line += VirtualizationName(result.userWrites);
    if (FAILED(result.userProbeError)) line += L", probe-hr=" + HResultText(result.userProbeError);
    lines.push_back(line + L").");
  }
  for (const std::wstring& failure : result.failures) lines.push_back(L"Not done: " + failure);
  return lines;
}

std::wstring ZoomText(const ZoomDecision& decision) {
  std::wstring text = L"Zoom: " + std::to_wstring(decision.zoom) + L" % (";
  switch (decision.origin) {
    case ZoomOrigin::kRegistry:
      text += L"saved setting";
      break;
    case ZoomOrigin::kLegacyIni:
      text += L"1.x's settings.ini";
      break;
    case ZoomOrigin::kDefault:
      text += L"default";
      break;
  }
  return text + L")";
}

std::wstring ZoomSavedText(int zoom, bool refreshDefault) {
  if (refreshDefault) {
    return L"Zoom set to " + std::to_wstring(ClampZoom(zoom)) +
           L" %. Thumbnails File Explorer already made keep the old zoom; run --refresh to update "
           L"them.";
  }
  return L"Zoom set to " + std::to_wstring(ClampZoom(zoom)) +
         L" %. Thumbnails made from now on use it. To redo the ones File Explorer already made, "
         L"run --refresh --full (restarts File Explorer), or --refresh for PDF thumbnails only "
         L"(folder windows open now may keep the old ones).";
}

std::wstring ZoomRefreshedText(int zoom, size_t refreshed, size_t files) {
  return L"Zoom set to " + std::to_wstring(ClampZoom(zoom)) + L" %. Refreshed " +
         std::to_wstring(refreshed) + L" PDF thumbnails in " + std::to_wstring(files) + L" files.";
}

std::wstring ZoomUnchangedText(int zoom) {
  return L"Zoom is already " + std::to_wstring(ClampZoom(zoom)) +
         L" %; nothing to refresh (run --refresh to redo the thumbnails).";
}

std::wstring ZoomRefreshStoppedText(int zoom, size_t refreshed) {
  return L"Zoom set to " + std::to_wstring(ClampZoom(zoom)) + L" %. The refresh stopped: " +
         std::to_wstring(refreshed) +
         L" thumbnails refreshed, the rest keep the old zoom; run --refresh to finish.";
}

bool UninstallNeedsYes(const CliArgs& args, bool canAskOnConsole) noexcept {
  return args.verb == Verb::kUninstall && !args.yes && !canAskOnConsole;
}

bool RefreshNeedsYes(const CliArgs& args, bool canAskOnConsole) noexcept {
  return args.verb == Verb::kRefresh && args.full && !args.yes && !canAskOnConsole;
}

std::wstring RefreshFullRefusalMessage(RebuildRefusal refusal, const ProcessContext& context,
                                       HRESULT detail) {
  if (refusal == RebuildRefusal::kVirtualized) return CannotRunHereMessage(context);
  return RebuildRefusalText(refusal, context, detail);
}

std::wstring RefreshFolderRefusalMessage(RefreshRefusal refusal, const ProcessContext& context) {
  if (refusal == RefreshRefusal::kVirtualized) return CannotRunHereMessage(context);
  return RefreshRefusalText(refusal, context);
}

bool ShouldStartSettingsWindow(DWORD consoleProcessCount, bool hasConsoleWindow) noexcept {
  return consoleProcessCount == 1 && hasConsoleWindow;
}

bool ShouldLog(const ProcessContext& context) noexcept { return !context.elevated; }

bool SweepsProbes(const ProcessContext& context) noexcept { return !context.elevated; }

bool TakeBackNeedsYes(const CliArgs& args, bool canAskOnConsole) noexcept {
  return args.verb == Verb::kFix && args.takeBack && !args.yes && !canAskOnConsole;
}

bool IsYesAnswer(std::wstring_view answer) noexcept {
  while (!answer.empty() && iswspace(answer.front())) answer.remove_prefix(1);
  while (!answer.empty() && iswspace(answer.back())) answer.remove_suffix(1);
  return IEquals(answer, L"y") || IEquals(answer, L"yes");
}

std::wstring StartMessage(Mode mode, const ProcessContext& context) {
  wchar_t integrity[16] = {};
  swprintf_s(integrity, L"0x%04lX", context.integrityRid);
  std::wstring message = L"start ";
  message += mode == Mode::kCli ? L"cli" : L"gui";
  message += L" il=";
  message += integrity;
  message += context.elevated ? L" elevated=yes" : L" elevated=no";
  message += context.packagedAncestor ? L" packaged=yes" : L" packaged=no";
  // Each check's own result: agreeing views are no verdict about writes (context.h).
  if (context.probe == ContextProbe::kWrite) {
    message += L" virtualized=";
    message += VirtualizationName(context.virtualization);
  } else if (context.probe == ContextProbe::kReadOnly) {
    message += L" views=";
    message += ViewsName(context.views);
  }
  if (FAILED(context.probeError)) message += L" probe-hr=" + HResultText(context.probeError);
  return message;
}

int RunCommand(const CliArgs& args, Mode mode, Output& out, OpenWindowFn openWindow) {
  const ParseOptions parseOptions{mode, kDevInstall};
  switch (args.verb) {
    case Verb::kVersion:
      out.Line(VersionText());
      return Code(ExitCode::kOk);
    case Verb::kHelp:
      out.Line(UsageText(parseOptions));
      return Code(ExitCode::kOk);
    case Verb::kElevated:
      // The elevated half of a change shows nothing and logs nothing: its exit code and the
      // lines it sends its parent are its report. It reads its own command line again and
      // checks it, and where it runs from, before anything else (elevation.h).
      return ElevatedMain();
    case Verb::kNone:
      if (mode == Mode::kCli) {
        DWORD processes[2] = {};
        if (ShouldStartSettingsWindow(GetConsoleProcessList(processes, 2),
                                      GetConsoleWindow() != nullptr)) {
          const HRESULT hr = StartSettingsExecutable();
          if (FAILED(hr)) {
            out.Error(L"Cannot start " + std::wstring(kSettingsExe) + L": " + HResultText(hr));
            return Code(ExitCode::kFailed);
          }
          return Code(ExitCode::kOk);
        }
        out.Line(UsageText(parseOptions));
        return Code(ExitCode::kOk);
      }
      break;
    default:
      break;
  }

  // Before anything is read: a script cannot answer the question, so it must say --yes.
  if (TakeBackNeedsYes(args, CanAskOnConsole())) {
    out.Error(
        L"--fix --take-back takes .pdf slots from another handler and needs --yes when there is "
        L"no console to ask on. Run --help");
    return Code(ExitCode::kUsage);
  }
  if (UninstallNeedsYes(args, CanAskOnConsole())) {
    out.Error(L"--uninstall removes " + std::wstring(kProductName) +
              L" for all users and needs --yes when there is no console to ask on. Run --help");
    return Code(ExitCode::kUsage);
  }
  if (RefreshNeedsYes(args, CanAskOnConsole())) {
    out.Error(
        L"--refresh --full restarts File Explorer and needs --yes when there is no console to ask "
        L"on. Run --help");
    return Code(ExitCode::kUsage);
  }

  ProcessContext context;
  const HRESULT hr = GetProcessContext(ProbeFor(args), &context);
  if (FAILED(hr)) {
    out.Error(L"Cannot read this process's user and token: " + HResultText(hr));
    return Code(ExitCode::kFailed);
  }
  const std::wstring component = Component(args);
  if (args.writes && RefusesWrites(context.virtualization)) {
    out.Error(CannotRunHereMessage(context));
    // Under a packaged app this is the expected answer, and the log file there may be
    // redirected into the app's private storage too: not logged. Without one, the canary
    // failed or disagreed on a normal PC, and a scheduled run without a console has no other
    // way to leave a trace of that.
    if (!context.packagedAncestor && ShouldLog(context)) {
      EnableLog();
      Log(LogLevel::kInfo, component, StartMessage(mode, context));
      Log(LogLevel::kWarning, component,
          L"end exit=" + std::to_wstring(Code(ExitCode::kWrongContext)) + L" refused");
    }
    return Code(ExitCode::kWrongContext);
  }

  // An elevated process logs nothing (ShouldLog); its Log() calls then write nothing.
  if (ShouldLog(context)) EnableLog();
  AppMutexes appMutexes;
  // Best effort: WIL logs a failure, and the line below a namespace closed to this process.
  const HRESULT held = appMutexes.Hold(context.userSid);
  Log(LogLevel::kInfo, component, StartMessage(mode, context));
  if (held == kNamedObjectsRefusedHr) {
    Log(LogLevel::kWarning, component,
        L"installer mutexes not held: named-objects-refused hr=" + HResultText(held));
  }
  int code = Code(ExitCode::kFailed);
  switch (args.verb) {
    case Verb::kNone:
      code = OpenSettingsWindow(context, args.relaunched, openWindow, out);
      break;
    case Verb::kStatus:
      code = RunStatus(args, context, out);
      break;
    case Verb::kCheck:
      code = RunCheckCommand(args, context, out);
      break;
    case Verb::kFix:
      code = RunFixCommand(args, context, out);
      break;
    case Verb::kChecks:
      code = RunChecksCommand(args, context, out);
      break;
    case Verb::kDiagnostics:
      code = RunDiagnostics(args, context, out);
      break;
    case Verb::kSetupUser:
      code = RunSetupUserCommand(args, context, out);
      break;
    case Verb::kSetupMachine:
      code = RunSetupMachineCommand(args, context, out);
      break;
    case Verb::kMigrateV1:
      code = RunMigrateV1Command(args, context, out);
      break;
    case Verb::kInstall:
      code = RunInstallCommand(args, context, out);
      break;
    case Verb::kUninstall:
      code = RunUninstallCommand(args, context, out);
      break;
    case Verb::kUninstallCleanup:
      code = RunUninstallCleanupCommand(out);
      break;
    case Verb::kZoom:
      code = RunZoomCommand(args, context, out);
      break;
    case Verb::kRefresh:
      code = RunRefreshCommand(args, context, out);
      break;
    default:
      out.Error(std::wstring(Describe(args.verb).name) + L" is not available in this build yet");
      break;
  }
  Log(LogLevel::kInfo, component, L"end exit=" + std::to_wstring(code));
  return code;
}

}  // namespace pdft
