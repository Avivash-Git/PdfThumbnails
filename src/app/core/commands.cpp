#include "commands.h"

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "check.h"
#include "diagnostics.h"
#include "evaluate.h"
#include "instance.h"
#include "json.h"
#include "live_test.h"
#include "log.h"
#include "paths.h"
#include "project_config.h"
#include "registry_roots.h"
#include "snapshot.h"
#include "status_format.h"
#include "strings.h"
#include "system_probes.h"

namespace pdft {
namespace {

constexpr int Code(ExitCode code) { return static_cast<int>(code); }

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

// The settings executable without a command: one window per user.
int OpenSettingsWindow(const ProcessContext& context, OpenWindowFn openWindow) {
  wil::unique_handle instance;
  bool first = false;
  if (FAILED(TakeSingleInstance(context.userSid, std::addressof(instance), &first))) {
    return Code(ExitCode::kFailed);
  }
  // Another window is open. Bringing it to the front comes with the window itself.
  if (!first) return Code(ExitCode::kOk);
  return openWindow ? openWindow() : Code(ExitCode::kOk);
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

// Prints |text| (UTF-8 JSON) as one line.
void PrintUtf8(Output& out, const std::string& text) {
  std::wstring wide;
  if (SUCCEEDED(Utf8ToWide(text, &wide))) out.Line(wide);
}

int RunStatus(const CliArgs& args, const ProcessContext& context, Output& out) {
  // Probe PDFs a crashed run left in %TEMP%; best effort.
  (void)SweepStaleProbes();
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
    PrintUtf8(out, StatusJson(snapshot, findings, code));
  } else {
    out.Line(RenderStatus(FormatStatus(snapshot, findings)));
  }
  return Code(code);
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
  if (result.busy) {
    out.Error(
        L"Another check or change is running and did not finish within 30 seconds; try again "
        L"later.");
    return Code(result.code);
  }
  if (quiet) return Code(result.code);
  for (const RepairOutcome& outcome : result.repairs) out.Line(RepairOutcomeText(outcome));
  if (!result.repairs.empty()) out.Line(L"");
  out.Line(RenderStatus(FormatStatus(result.after, result.findings)));
  if (result.adminStepNeeded && result.plan.machine) {
    // --fix will run the administrator step once this build has it; until then both say the same.
    out.Line(L"");
    out.Line(L"The machine-wide part needs an administrator to " +
             ElevatedVerbText(*result.plan.machine) +
             L". This build cannot run the administrator step yet; reinstall " + kProductName +
             L" for all users to repair it.");
  }
  return Code(result.code);
}

int RunCheckCommand(const CliArgs& args, const ProcessContext& context, Output& out) {
  (void)SweepStaleProbes();
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
  (void)SweepStaleProbes();
  CheckOptions options;
  options.repair = args.takeBack ? RepairMode::kSafeAndTakeBack : RepairMode::kSafe;
  options.context = context;
  CheckResult result;
  const HRESULT hr = RunCheck(options, RegistryRoots{}, RealSystemProbes(), &result);
  return ReportCheck(hr, result, context, false, out);
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
  // A link made in the meantime is replaced, never followed.
  wil::unique_hfile file(
      CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
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
  // A log that cannot be read costs its lines, not the report.
  std::vector<std::wstring> tail;
  if (const HRESULT hr = ReadLogTail(kDiagnosticsLogLines, &tail); FAILED(hr)) {
    tail = {L"(the log could not be read: " + HResultText(hr) + L")"};
  }
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

bool ShouldStartSettingsWindow(DWORD consoleProcessCount, bool hasConsoleWindow) noexcept {
  return consoleProcessCount == 1 && hasConsoleWindow;
}

bool ShouldLog(const ProcessContext& context) noexcept { return !context.elevated; }

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
      // The elevated half of a change shows nothing and logs nothing: its exit code is its
      // report. The steps themselves come with installing.
      return static_cast<int>(E_NOTIMPL);
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
  (void)appMutexes.Hold(context.userSid);  // best effort; WIL logs a failure
  Log(LogLevel::kInfo, component, StartMessage(mode, context));
  int code = Code(ExitCode::kFailed);
  switch (args.verb) {
    case Verb::kNone:
      code = OpenSettingsWindow(context, openWindow);
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
    case Verb::kDiagnostics:
      code = RunDiagnostics(args, context, out);
      break;
    default:
      out.Error(std::wstring(Describe(args.verb).name) + L" is not available in this build yet");
      break;
  }
  Log(LogLevel::kInfo, component, L"end exit=" + std::to_wstring(code));
  return code;
}

}  // namespace pdft
