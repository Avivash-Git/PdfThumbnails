// pdft_gui_tests: starts the real settings window and drives it from outside (window_test.cpp).
// The window reads the real registry and, in later steps, changes the signed-in user's settings,
// so this runs only on a throw-away PC: PDFT_GUI_TESTS=1 on a runner GitHub hosts. Elsewhere it
// starts nothing and exits 77 (CTest's "skipped"); the test presets leave its label out as well.
//
// The window refuses an administrator process, and runners are elevated: an elevated start checks
// that refusal first, then relaunches this program at Medium integrity for the tests themselves.

#include <windows.h>

#include <gtest/gtest.h>
#include <objbase.h>

#include <cstdio>
#include <string>

#include "context.h"
#include "exit_code.h"
#include "github_runner.h"
#include "gui_driver.h"
#include "medium_il.h"
#include "project_config.h"

namespace {

// Starts the settings executable from this elevated process: it must say why on screen and exit 8.
bool ElevatedStartIsRefused() {
  pdft::test::Program program;
  if (!program.Start(L"")) {
    std::printf("[ gui ] the settings executable did not start\n");
    return false;
  }
  HWND box = nullptr;
  const bool found = pdft::test::WaitFor(
      [&] {
        box = pdft::test::TopLevelWindowOf(program.id(), L"#32770");
        return box != nullptr;
      },
      60000);
  if (!found) {
    std::printf("[ gui ] an elevated start showed no message\n");
    program.Kill();
    return false;
  }
  // Asked again until it ends: a box that is still being made does not act on the first request.
  DWORD code = 0xFFFFFFFFu;
  for (int attempt = 0; attempt < 30 && code == 0xFFFFFFFFu; ++attempt) {
    PostMessageW(box, WM_COMMAND, IDOK, 0);
    code = program.WaitForExit(1000);
  }
  std::printf("[ gui ] an elevated start: message shown, exit %lu, message still there: %s\n", code,
              IsWindow(box) ? "yes" : "no");
  return code == static_cast<DWORD>(pdft::ExitCode::kWrongContext);
}

}  // namespace

int main(int argc, char** argv) {
  if (pdft::test::EnvironmentValue(L"PDFT_GUI_TESTS") != L"1" ||
      !pdft::test::OnGitHubHostedRunner()) {
    std::printf("[ gui ] not run: only with PDFT_GUI_TESTS=1 on a runner GitHub hosts\n");
    return 77;
  }
  if (!GetShellWindow()) {
    std::printf("[ gui ] not run: this session has no desktop shell\n");
    return 77;
  }
  pdft::ProcessContext context;
  if (FAILED(pdft::GetProcessContext(pdft::ContextProbe::kNone, &context))) return 2;
  const bool refusalChecked = context.elevated;
  const bool refused = !refusalChecked || ElevatedStartIsRefused();

  int exitCode = 0;
  if (pdft::test::RelaunchAtMediumIfElevated(&exitCode)) return refused ? exitCode : 1;

  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  CoUninitialize();
  return result;
}
