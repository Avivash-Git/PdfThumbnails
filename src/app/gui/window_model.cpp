#include "window_model.h"

#include <algorithm>

#include "cli_args.h"
#include "zoom.h"

namespace pdft {

ControlStates DecideControls(const WindowState& state) noexcept {
  ControlStates controls;
  const bool ready = state.context == WindowContext::kReady;
  const bool idle = state.activity == WindowActivity::kIdle;
  const bool mayEdit = idle || state.activity == WindowActivity::kGathering ||
                       state.activity == WindowActivity::kExternalBusy;
  controls.zoomAndChecks = ready && mayEdit;
  controls.checkNowAndFix = ready && idle;
  controls.rebuild = ready && idle && !state.otherUser;
  controls.uninstall = ready && idle && !state.otherUser;
  controls.openLogAndDiagnostics = state.context != WindowContext::kStarting;
  controls.save = ready && idle && state.dirty;
  controls.cancelLink = state.activity == WindowActivity::kRefreshing;
  return controls;
}

CloseAction DecideClose(const WindowState& state) noexcept {
  switch (state.activity) {
    case WindowActivity::kIdle:
    case WindowActivity::kExternalBusy:
      return state.dirty ? CloseAction::kAskSaveChanges : CloseAction::kCloseNow;
    case WindowActivity::kGathering:
      return state.dirty ? CloseAction::kAskSaveChanges : CloseAction::kWaitForWork;
    case WindowActivity::kRefreshing:
      return CloseAction::kAskStopRefresh;
    case WindowActivity::kChild:
      return CloseAction::kCloseNow;
    case WindowActivity::kChecking:
    case WindowActivity::kFixing:
    case WindowActivity::kSaving:
    case WindowActivity::kStopping:
    case WindowActivity::kClosing:
      return CloseAction::kWaitForWork;
  }
  return CloseAction::kWaitForWork;
}

WindowBanner DecideBanner(const WindowState& state) noexcept {
  if (state.context == WindowContext::kSandboxed) return WindowBanner::kSandboxed;
  if (state.otherUser) return WindowBanner::kOtherUser;
  if (state.activity == WindowActivity::kExternalBusy) return WindowBanner::kExternalBusy;
  if (state.context == WindowContext::kNotInstalled) return WindowBanner::kNotInstalled;
  return WindowBanner::kNone;
}

int ZoomFromBox(std::wstring_view text, int current) noexcept {
  if (text.empty()) return current;
  long long value = 0;
  for (wchar_t c : text) {
    if (c < L'0' || c > L'9') return current;
    value = std::min(value * 10 + (c - L'0'), 1'000'000LL);  // stays out of range, never wraps
  }
  return ClampZoom(value);
}

int PreviewEdgeForDpi(UINT dpi) noexcept {
  return std::clamp(MulDiv(96, static_cast<int>(dpi), 96), kMinPreviewEdge, kMaxPreviewEdge);
}

ChildOutcome OutcomeOfChild(DWORD exitCode) noexcept {
  switch (exitCode) {
    case 0:
    case 3:
      return ChildOutcome::kInformation;
    case 1:
      return ChildOutcome::kWarning;
    default:
      return ChildOutcome::kError;
  }
}

}  // namespace pdft
