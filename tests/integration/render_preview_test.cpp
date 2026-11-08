// The settings executable's preview child, for real: it renders the embedded samples through the
// handler and writes pixels to its standard output. It reads the registered class and writes
// nothing. An administrator process does not render, so where this test runs elevated the child
// starts through pdft-run-medium and only the refusal is asked of the elevated start.

#include <windows.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "child_process.h"
#include "context.h"
#include "exit_code.h"
#include "medium_il.h"
#include "preview_protocol.h"
#include "project_config.h"

namespace pdft {
namespace {

constexpr DWORD kTimeoutMs = 120000;

bool Elevated() {
  ProcessContext context;
  return SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context)) && context.elevated;
}

std::wstring SettingsExe() { return test::SelfFolder() + L"\\" + kSettingsExe; }

// Runs the child below administrator rights, however this test was started.
test::CapturedRun Render(const std::wstring& value) {
  if (!Elevated())
    return test::RunCaptured(SettingsExe(), L"--render-preview " + value, kTimeoutMs);
  return test::RunCaptured(test::SelfFolder() + L"\\pdft-run-medium.exe",
                           L"-- \"" + SettingsExe() + L"\" --render-preview " + value, kTimeoutMs);
}

#define RENDER_OR_SKIP(value, run)                                                           \
  test::CapturedRun run = Render(value);                                                     \
  if (test::IsApplicationControlBlock(run.hr)) GTEST_SKIP() << "Application Control";        \
  ASSERT_HRESULT_SUCCEEDED(run.hr);                                                          \
  if (run.exitCode == static_cast<DWORD>(ExitCode::kNotInstalled)) {                         \
    GTEST_SKIP() << "no handler this build may load (not a developer build, not installed)"; \
  }

struct Expected {
  const wchar_t* value;
  uint32_t edge;
  uint32_t documentW, documentH;
  uint32_t slideW, slideH;
};

class RenderPreview : public ::testing::TestWithParam<Expected> {};

TEST_P(RenderPreview, GivesEachSampleAtTheZoomAskedFor) {
  const Expected expected = GetParam();
  RENDER_OR_SKIP(expected.value, run);
  ASSERT_EQ(run.exitCode, 0u) << run.err;
  std::vector<PreviewFrame> frames;
  ASSERT_TRUE(DecodePreview(run.out, expected.edge, &frames)) << run.out.size() << " bytes";
  ASSERT_EQ(frames[0].hr, S_OK);
  EXPECT_EQ(frames[0].width, expected.documentW);
  EXPECT_EQ(frames[0].height, expected.documentH);
  ASSERT_EQ(frames[1].hr, S_OK);
  EXPECT_EQ(frames[1].width, expected.slideW);
  EXPECT_EQ(frames[1].height, expected.slideH);
}

INSTANTIATE_TEST_SUITE_P(Zooms, RenderPreview,
                         ::testing::Values(Expected{L"100,256", 256, 181, 256, 256, 144},
                                           Expected{L"142,256", 256, 256, 256, 256, 204},
                                           Expected{L"200,256", 256, 256, 256, 256, 256}));

TEST(RenderPreviewChild, RefusesAValueOutOfRange) {
  const test::CapturedRun run =
      test::RunCaptured(SettingsExe(), L"--render-preview 99,256", kTimeoutMs);
  if (test::IsApplicationControlBlock(run.hr)) GTEST_SKIP() << "Application Control";
  ASSERT_HRESULT_SUCCEEDED(run.hr);
  EXPECT_EQ(run.exitCode, static_cast<DWORD>(ExitCode::kUsage));
}

TEST(RenderPreviewChild, DoesNotRenderElevated) {
  if (!Elevated()) GTEST_SKIP() << "this test program is not elevated";
  const test::CapturedRun run =
      test::RunCaptured(SettingsExe(), L"--render-preview 100,256", kTimeoutMs);
  ASSERT_HRESULT_SUCCEEDED(run.hr);
  EXPECT_EQ(run.exitCode, static_cast<DWORD>(ExitCode::kWrongContext));
  EXPECT_EQ(run.out, "");
}

}  // namespace
}  // namespace pdft
