// Saving a new zoom and, where asked, refreshing the PDF thumbnails File Explorer already made:
// what --zoom <n> and the settings window's Save both do. It says what happened; the words are
// the caller's.
//
// Under the operation lock, held until it returns: the zoom is saved, File Explorer is told, and
// the refresh runs only when the zoom really changed and the refusals (refresh.h), decided again
// under the lock, allow it. A refresh that is refused or stopped leaves the zoom saved.

#pragma once

#include <windows.h>

#include <functional>
#include <stop_token>

#include "check.h"
#include "context.h"
#include "refresh.h"
#include "registry_roots.h"

namespace pdft {

struct ZoomChangeOptions {
  int zoom = 100;
  ProcessContext context;               // the canary's (ContextProbe::kWrite)
  DWORD lockWaitMs = kCheckLockWaitMs;  // never more than kCheckLockWaitMs
  bool refresh = false;                 // refresh the thumbnails once the zoom is saved
  // Called once, just before a refresh starts (the command line starts listening for Ctrl+C).
  std::function<void()> beforeRefresh;
  RefreshProgressFn progress;  // what a refresh reports (may be empty)
  std::stop_token stop;        // stops a refresh
};

struct ZoomChangeResult {
  bool busy = false;                 // the lock stayed taken: nothing changed
  bool namedObjectsRefused = false;  // Windows does not let this process make the lock
  HRESULT lockError = S_OK;          // the lock failed otherwise
  HRESULT saveError = S_OK;          // the zoom could not be saved
  bool saved = false;
  bool changed = true;                             // it differs from the zoom before
  RefreshRefusal refusal = RefreshRefusal::kNone;  // why no refresh ran after all
  bool refreshRan = false;
  HRESULT refreshResult = S_OK;  // RefreshPdfThumbnails' answer
  RefreshCounts counts;
};

// What the change asks of Windows: the real steps (RealZoomRefreshSteps) and the tests' fakes.
struct ZoomRefreshSteps {
  std::function<HRESULT(const ProcessContext&, const RegistryRoots&, RefreshConditions*)>
      readConditions;
  std::function<HRESULT(const RefreshProgressFn&, std::stop_token, RefreshCounts*)> refresh;
};

// ReadRefreshConditions with the system's association queries; RefreshPdfThumbnails in the
// default places, on the calling thread.
ZoomRefreshSteps RealZoomRefreshSteps();

// Changes the zoom (see the top of this file). S_OK when it came to a result (*result: also a
// lock it did not get, or a zoom that could not be saved); a failure only when out of memory.
[[nodiscard]] HRESULT ChangeZoom(const ZoomChangeOptions& options, const RegistryRoots& roots,
                                 const ZoomRefreshSteps& steps, ZoomChangeResult* result) noexcept;

}  // namespace pdft
