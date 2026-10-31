#include "commands.h"

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <cwchar>
#include <filesystem>
#include <memory>
#include <string_view>

#include "instance.h"
#include "log.h"
#include "paths.h"
#include "project_config.h"
#include "strings.h"

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

bool ShouldStartSettingsWindow(DWORD consoleProcessCount, bool hasConsoleWindow) noexcept {
  return consoleProcessCount == 1 && hasConsoleWindow;
}

bool ShouldLog(const ProcessContext& context) noexcept { return !context.elevated; }

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
  if (args.verb == Verb::kNone) {
    code = OpenSettingsWindow(context, openWindow);
  } else {
    out.Error(std::wstring(Describe(args.verb).name) + L" is not available in this build yet");
  }
  Log(LogLevel::kInfo, component, L"end exit=" + std::to_wstring(code));
  return code;
}

}  // namespace pdft
