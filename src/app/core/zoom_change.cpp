#include "zoom_change.h"

#include <wil/result_macros.h>

#include <optional>
#include <string>

#include "instance.h"
#include "log.h"
#include "registration.h"
#include "settings.h"
#include "strings.h"
#include "zoom.h"

namespace pdft {

ZoomRefreshSteps RealZoomRefreshSteps() {
  ZoomRefreshSteps steps;
  steps.readConditions = [](const ProcessContext& context, const RegistryRoots& roots,
                            RefreshConditions* conditions) {
    return ReadRefreshConditions(context, roots, SystemAssociationQueries(), conditions);
  };
  steps.refresh = [](const RefreshProgressFn& progress, std::stop_token stop,
                     RefreshCounts* counts) {
    return RefreshPdfThumbnails(PdfRefreshOptions{}, progress, std::move(stop), counts);
  };
  return steps;
}

HRESULT ChangeZoom(const ZoomChangeOptions& options, const RegistryRoots& roots,
                   const ZoomRefreshSteps& steps, ZoomChangeResult* result) noexcept try {
  std::optional<OperationLock> lock;
  const HRESULT locked = OperationLock::Acquire(EffectiveLockWaitMs(options.lockWaitMs), &lock);
  if (locked == OperationLock::kBusy || locked == OperationLock::kTakenByLowerIntegrity) {
    result->busy = true;
    return S_OK;
  }
  if (locked == kNamedObjectsRefusedHr) {
    Log(LogLevel::kWarning, L"zoom",
        L"zoom result=failed named-objects-refused hr=" + HResultText(kNamedObjectsRefusedHr));
    result->namedObjectsRefused = true;
    return S_OK;
  }
  if (FAILED(locked)) {
    result->lockError = locked;
    return S_OK;
  }
  if (lock->sessionOnly()) Log(LogLevel::kWarning, L"zoom", L"zoom lock=session-only");

  if (Settings before; SUCCEEDED(LoadSettings(roots, &before))) {
    result->changed = before.zoom != ClampZoom(options.zoom);
  }
  result->saveError = SaveZoom(roots, options.zoom);
  if (FAILED(result->saveError)) return S_OK;
  result->saved = true;
  (void)NotifyAssocChanged(roots);
  Log(LogLevel::kInfo, L"zoom", L"zoom saved=" + std::to_wstring(options.zoom));
  if (!options.refresh || !result->changed) return S_OK;

  RefreshConditions conditions;
  const HRESULT read = steps.readConditions(options.context, roots, &conditions);
  result->refusal = FAILED(read) ? RefreshRefusal::kVirtualized : DecideRefreshRefusal(conditions);
  if (result->refusal != RefreshRefusal::kNone) {
    Log(LogLevel::kInfo, L"zoom",
        std::wstring(L"zoom refresh=skipped refused=") + RefreshRefusalToken(result->refusal));
    return S_OK;
  }
  if (options.beforeRefresh) options.beforeRefresh();
  result->refreshRan = true;
  result->refreshResult = steps.refresh(
      options.progress ? options.progress : [](const auto&) {}, options.stop, &result->counts);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
