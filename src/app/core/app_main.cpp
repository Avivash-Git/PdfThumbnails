#include "app_main.h"

#include <objbase.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <string_view>
#include <variant>
#include <vector>

#include "cli_args.h"
#include "com_security.h"
#include "log.h"
#include "output.h"
#include "project_config.h"
#include "strings.h"

namespace pdft {

int AppMain(Mode mode, HRESULT hardened, std::span<const wchar_t* const> args,
            OpenWindowFn openWindow) {
  // WIL is compiled without its static initializers (cmake/Dependencies.cmake), so its error
  // reporting is set up here instead. This one only sets function pointers (the module name for
  // failure reports, fail-fast through kernelbase); never wil::WilInitialize_Result, which would
  // bring back the failure cache that other processes could plant.
  wil::WilInitialize_ResultMacros_DesktopOrSystem_SuppressPrivateApiUse();
  Output out = Output::ForMode(mode);
  if (FAILED(hardened)) {
    out.Error(L"Cannot restrict where DLLs are loaded from: " + HResultText(hardened));
    return static_cast<int>(ExitCode::kFailed);
  }
  const HRESULT com = InitializeComSecurity();
  if (FAILED(com)) {
    out.Error(L"Cannot initialize COM: " + HResultText(com));
    return static_cast<int>(ExitCode::kFailed);
  }
  const auto uninitialize = wil::scope_exit([]() noexcept { CoUninitialize(); });
  LogWilFailures();

  std::vector<std::wstring_view> words;
  for (size_t i = 1; i < args.size(); ++i) words.emplace_back(args[i] ? args[i] : L"");
  const auto parsed = ParseCommandLine(words, ParseOptions{mode, kDevInstall});
  if (const ParseError* error = std::get_if<ParseError>(&parsed)) {
    // Not logged: the arguments are the user's own text.
    out.Error(error->message);
    out.Error(L"Run --help for usage.");
    return static_cast<int>(ExitCode::kUsage);
  }
  return RunCommand(std::get<CliArgs>(parsed), mode, out, openWindow);
}

}  // namespace pdft
