// The settings window's work that must not run on its thread: one job at a time on a thread of
// its own, in a single-threaded apartment. A job leaves its result here and posts one bare
// kJobDoneMessage, so a message that is lost leaks nothing; the window then takes the result,
// which ends the thread.

#pragma once

#include <windows.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include "check.h"
#include "context.h"
#include "evaluate.h"
#include "refresh.h"
#include "repair.h"
#include "snapshot.h"
#include "status_panel.h"

namespace pdft {

inline constexpr UINT kJobDoneMessage = WM_APP + 2;
inline constexpr UINT kDiagnosticsDoneMessage = WM_APP + 4;
inline constexpr UINT kRefreshProgressMessage = WM_APP + 5;

// What the window reads about this PC.
struct StateReading {
  HRESULT hr = E_PENDING;  // the state could not be read when it failed
  ProcessContext context;  // from the registry canary
  bool contextRead = false;
  // The registry is virtualized below an app package and this start has not been through File
  // Explorer yet: start again outside it. Nothing else was read.
  bool leaveContainer = false;
  bool snapshotRead = false;  // |snapshot| and |findings| hold what was read
  Snapshot snapshot;
  std::vector<Finding> findings;
  std::optional<CheckResult> check;  // when the job was a check or fix
  std::wstring diagnostics;          // when the job was the diagnostics report
  std::optional<SaveOutcome> save;   // when the job was Save
};

// What Save has to change.
struct SaveRequest {
  std::optional<int> zoom;
  bool changeChecks = false;
  bool afterPdfCraft = false;
  bool atSignIn = false;
};

class WindowJobs {
 public:
  // |doneMessage| says that a job has finished.
  explicit WindowJobs(HWND window, UINT doneMessage = kJobDoneMessage) noexcept
      : window_(window), doneMessage_(doneMessage) {}
  WindowJobs(const WindowJobs&) = delete;
  WindowJobs& operator=(const WindowJobs&) = delete;

  bool running() const noexcept { return thread_.joinable(); }

  // The first reading: |context| again with the registry canary, then the state (without the
  // Explorer test). |relaunched|: this start already came through File Explorer.
  void StartFirstReading(bool relaunched);
  // The state again, with the context read before.
  void StartReading(const ProcessContext& context);
  // Check now, with the Explorer test. With |fix| also the repairs that mode allows and the
  // administrator step, whose prompt belongs to |owner|.
  void StartCheck(const ProcessContext& context, HWND owner, std::optional<RepairMode> fix);
  // Save: the check task first, so that a refresh that is stopped cannot lose it, then the zoom
  // and, where the build says so, the refresh; then the state again.
  void StartSave(const ProcessContext& context, const SaveRequest& request);
  // Stops a refresh: the request under way finishes first.
  void RequestStop() noexcept { stop_.request_stop(); }
  // Waits at most |timeoutMs| for the job to end (the session is ending). Leaves it running then.
  void WaitForEnd(DWORD timeoutMs) noexcept;
  // The newest progress of a refresh; nothing when none came since the last call.
  std::optional<RefreshProgress> TakeProgress();
  // The masked diagnostics report.
  void StartDiagnostics(const ProcessContext& context);

  // The finished job's reading. Call once kJobDoneMessage arrived: it ends the thread.
  std::optional<StateReading> TakeReading();

 private:
  template <typename Body>
  void Start(Body body);
  void Store(StateReading reading) noexcept;

  HWND window_;
  UINT doneMessage_;
  std::jthread thread_;
  std::mutex mutex_;
  std::optional<StateReading> reading_;
  std::stop_source stop_;
  std::optional<RefreshProgress> progress_;
  std::atomic<bool> progressPosted_{false};  // one message on the way at most
};

}  // namespace pdft
