// A rebuild, uninstall or install run as a child of the settings window: a thread reads what it
// says. It has no Cancel; closing the window leaves it running, and it is never ended from here.

#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace pdft {

inline constexpr UINT kChildLineMessage = WM_APP + 6;
inline constexpr UINT kChildDoneMessage = WM_APP + 7;

struct ChildResult {
  HRESULT hr = E_PENDING;  // starting it failed when this failed
  DWORD exitCode = 0;
  std::wstring output;  // what it said, lines separated by "\n"
};

class ChildOperation {
 public:
  explicit ChildOperation(HWND window) noexcept : window_(window) {}
  ~ChildOperation();
  ChildOperation(const ChildOperation&) = delete;
  ChildOperation& operator=(const ChildOperation&) = delete;

  bool running() const noexcept { return thread_.joinable(); }

  // Starts this program with |arguments|, in System32 so that it pins no folder of ours.
  void Start(std::wstring arguments);
  // The line it said last; nothing when none came since the last call.
  std::optional<std::wstring> TakeLine();
  // Its result. Call once kChildDoneMessage arrived: it ends the thread.
  std::optional<ChildResult> TakeResult();
  // Stops reading without touching the child, which goes on by itself (the window is closing).
  void Abandon() noexcept;

 private:
  void Run(std::wstring arguments);

  HWND window_;
  std::thread thread_;
  std::mutex mutex_;
  std::optional<std::wstring> line_;
  std::optional<ChildResult> result_;
  std::atomic<bool> linePosted_{false};
  std::atomic<bool> abandoned_{false};
};

}  // namespace pdft
