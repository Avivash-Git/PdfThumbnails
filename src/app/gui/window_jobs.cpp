#include "window_jobs.h"

#include <objbase.h>

#include <utility>

#include "registry_roots.h"
#include "system_probes.h"

namespace pdft {
namespace {

// Gathers the state as the window shows it: never with the Explorer test, which Check now runs.
void ReadState(const ProcessContext& context, StateReading* reading) {
  GatherOptions options;
  options.liveTest = false;
  options.context = context;
  reading->hr = GatherSnapshot(RegistryRoots{}, options, RealSystemProbes(), &reading->snapshot);
  if (SUCCEEDED(reading->hr)) reading->findings = Evaluate(reading->snapshot);
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
    PostMessageW(window_, kJobDoneMessage, 0, 0);
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

std::optional<StateReading> WindowJobs::TakeReading() {
  if (thread_.joinable()) thread_.join();
  const std::lock_guard lock(mutex_);
  return std::exchange(reading_, std::nullopt);
}

}  // namespace pdft
