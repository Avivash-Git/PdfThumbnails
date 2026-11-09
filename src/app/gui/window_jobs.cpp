#include "window_jobs.h"

#include <objbase.h>

#include <utility>

#include "check_schedule.h"
#include "diagnostics.h"
#include "elevation.h"
#include "project_config.h"
#include "registry_roots.h"
#include "settings.h"
#include "system_probes.h"
#include "zoom_change.h"

namespace pdft {
namespace {

// Gathers the state as the window shows it: never with the Explorer test, which Check now runs.
void ReadState(const ProcessContext& context, StateReading* reading) {
  GatherOptions options;
  options.liveTest = false;
  options.context = context;
  reading->hr = GatherSnapshot(RegistryRoots{}, options, RealSystemProbes(), &reading->snapshot);
  if (SUCCEEDED(reading->hr)) {
    reading->findings = Evaluate(reading->snapshot);
    reading->snapshotRead = true;
  }
}

}  // namespace

template <typename Body>
void WindowJobs::Start(Body body) {
  thread_ = std::jthread([this, body = std::move(body)]() noexcept {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    StateReading reading;
    if (FAILED(com)) {
      reading.hr = com;
    } else {
      try {
        body(&reading);
      } catch (...) {
        reading.hr = E_OUTOFMEMORY;
      }
      CoUninitialize();
    }
    Store(std::move(reading));
    PostMessageW(window_, doneMessage_, 0, 0);
  });
}

void WindowJobs::Store(StateReading reading) noexcept {
  const std::lock_guard lock(mutex_);
  reading_ = std::move(reading);
}

void WindowJobs::StartFirstReading(bool relaunched) {
  Start([relaunched](StateReading* reading) {
    reading->hr = GetProcessContext(ContextProbe::kWrite, &reading->context);
    if (FAILED(reading->hr)) return;
    reading->contextRead = true;
    if (RefusesWrites(reading->context.virtualization) && reading->context.packagedAncestor &&
        !relaunched) {
      reading->leaveContainer = true;
      return;
    }
    ReadState(reading->context, reading);
  });
}

void WindowJobs::StartReading(const ProcessContext& context) {
  Start([context](StateReading* reading) {
    reading->context = context;
    reading->contextRead = true;
    ReadState(context, reading);
  });
}

void WindowJobs::StartCheck(const ProcessContext& context, HWND owner,
                            std::optional<RepairMode> fix) {
  Start([context, owner, fix](StateReading* reading) {
    reading->context = context;
    reading->contextRead = true;
    CheckOptions options;
    options.context = context;
    options.lockWaitMs = 0;  // the window says it is busy instead of hanging
    options.owner = owner;
    if (fix) {
      options.repair = *fix;
      options.launcher = LaunchElevatedWithShell;
    }
    CheckResult result;
    reading->hr = RunCheck(options, RegistryRoots{}, RealSystemProbes(), &result);
    if (SUCCEEDED(reading->hr) && !result.busy && !result.refused && !result.namedObjectsRefused) {
      reading->snapshot = result.after;
      reading->findings = result.findings;
      // The record of this very check, which |after| was gathered before.
      (void)ReadCheck(RegistryRoots{}, &reading->snapshot.last);
      reading->snapshotRead = true;
    }
    reading->check = std::move(result);
  });
}

void WindowJobs::StartSave(const ProcessContext& context, const SaveRequest& request) {
  stop_ = std::stop_source{};
  Start([this, context, request, stop = stop_.get_token()](StateReading* reading) {
    SaveOutcome outcome;
    if (request.changeChecks) {
      ScheduleOptions options;
      options.afterPdfCraft = request.afterPdfCraft;
      options.atSignIn = request.atSignIn;
      options.context = context;
      options.lockWaitMs = 0;
      outcome.checksAsked = true;
      outcome.checksHr =
          ChangeCheckSchedule(options, RegistryRoots{}, RealSystemProbes(), &outcome.checks);
    }
    if (request.zoom) {
      ZoomChangeOptions options;
      options.zoom = *request.zoom;
      options.context = context;
      options.lockWaitMs = 0;
      options.refresh = kPdfRefreshDefault;
      options.stop = stop;
      options.progress = [this](const RefreshProgress& progress) {
        {
          const std::lock_guard lock(mutex_);
          progress_ = progress;
        }
        if (!progressPosted_.exchange(true)) {
          PostMessageW(window_, kRefreshProgressMessage, 0, 0);
        }
      };
      outcome.zoomAsked = true;
      outcome.zoom = *request.zoom;
      outcome.zoomHr =
          ChangeZoom(options, RegistryRoots{}, RealZoomRefreshSteps(), &outcome.zoomResult);
    }
    reading->context = context;
    reading->contextRead = true;
    ReadState(context, reading);
    reading->save = std::move(outcome);
  });
}

void WindowJobs::WaitForEnd(DWORD timeoutMs) noexcept {
  if (thread_.joinable()) WaitForSingleObject(thread_.native_handle(), timeoutMs);
}

std::optional<RefreshProgress> WindowJobs::TakeProgress() {
  progressPosted_ = false;
  const std::lock_guard lock(mutex_);
  return std::exchange(progress_, std::nullopt);
}

void WindowJobs::StartDiagnostics(const ProcessContext& context) {
  Start([context](StateReading* reading) {
    reading->hr = MakeDiagnosticsReport(context, &reading->diagnostics);
  });
}

std::optional<StateReading> WindowJobs::TakeReading() {
  if (thread_.joinable()) thread_.join();
  const std::lock_guard lock(mutex_);
  return std::exchange(reading_, std::nullopt);
}

}  // namespace pdft
