// The settings window's rules (pure): which controls are on, what Close does, which banner shows,
// when there is something to save, and what a child's exit code means. The window only applies
// what these say, so all of it is tested without a window.

#pragma once

#include <windows.h>

#include <array>
#include <optional>
#include <string_view>

namespace pdft {

// What the window knows about where it runs.
enum class WindowContext {
  kStarting,      // the first reading has not come back
  kReady,         // installed, and writes reach the real registry
  kNotInstalled,  // nothing of this product is registered here
  kSandboxed,     // writes may not reach the real registry
};

// The context a finished reading shows (pure): writes that may not reach the registry count first.
WindowContext ClassifyWindowContext(bool refusesWrites, bool installed) noexcept;

// What the window is doing. One thing at a time.
enum class WindowActivity {
  kIdle,
  kGathering,     // reading the state again
  kChecking,      // Check now
  kFixing,        // the Fix link
  kSaving,        // Save, before or without a refresh
  kRefreshing,    // Save's refresh, which can be cancelled
  kStopping,      // ... being cancelled
  kChild,         // a rebuild, uninstall or install in a child process
  kExternalBusy,  // another process holds the operation lock
  kClosing,       // waiting for work to end, then closing
};

struct WindowState {
  WindowContext context = WindowContext::kStarting;
  WindowActivity activity = WindowActivity::kIdle;
  bool otherUser = false;  // this process's user is not the desktop's
  bool dirty = false;      // the fields differ from what is saved
};

struct ControlStates {
  bool zoomAndChecks = false;
  bool checkNowAndFix = false;
  bool rebuild = false;
  bool uninstall = false;
  bool openLogAndDiagnostics = false;
  bool save = false;
  bool cancelLink = false;  // shown, not only enabled
  bool install = false;     // nothing is installed: the install buttons, in developer builds
};

// Which controls may be used now (pure).
ControlStates DecideControls(const WindowState& state) noexcept;

// What pressing Close, Esc or the X does now.
enum class CloseAction {
  kCloseNow,
  kAskSaveChanges,  // [Save] [Don't save] [Cancel]
  kAskStopRefresh,  // "Stop refreshing and close?"
  kWaitForWork,     // say what it waits for, then close
};

// (pure) A child goes on by itself, so it never holds the window open.
CloseAction DecideClose(const WindowState& state) noexcept;

enum class WindowBanner { kNone, kSandboxed, kOtherUser, kExternalBusy };

// The banner, the first that applies in the order of WindowBanner (pure).
WindowBanner DecideBanner(const WindowState& state) noexcept;

// The fields the Save button writes.
struct WindowEdits {
  int zoom = 100;
  bool afterPdfCraft = false;
  bool atSignIn = false;
  friend bool operator==(const WindowEdits&, const WindowEdits&) = default;
};

// What the zoom box means once it loses focus (pure): its number limited to the zoom's range, or
// |current| when it holds no number at all.
int ZoomFromBox(std::wstring_view text, int current) noexcept;

// The slider's tick marks; 142 is the first zoom that fills the square with an A4 page.
inline constexpr std::array<int, 6> kZoomTicks = {100, 120, 142, 160, 180, 200};

// A preview tile's edge in pixels at |dpi|: 96 DIPs, within what the preview child accepts (pure).
int PreviewEdgeForDpi(UINT dpi) noexcept;

enum class ChildOutcome { kInformation, kWarning, kError };

// The icon for a child's exit code (pure): 0 and 3 (cancelled, nothing changed) information, 1
// warning, anything else error.
ChildOutcome OutcomeOfChild(DWORD exitCode) noexcept;

}  // namespace pdft
