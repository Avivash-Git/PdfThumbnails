// IPdfThumbnailsPreviewOptions: a private interface of the handler's thumbnail object, for a
// preview that shows a zoom the user has not saved yet (the settings app). Ask an object from the
// handler's class factory for it with QueryInterface(pdft::kPreviewIid); its IID comes from
// cmake/ProjectInfo.cmake through project_config.h. Explorer never asks for it.

#pragma once

#include <windows.h>

#include <unknwn.h>

#include "project_config.h"

namespace pdft {

struct IPdfThumbnailsPreviewOptions : public IUnknown {
  // From now on this object renders at |percent|, clamped to kMinZoom..kMaxZoom (zoom.h),
  // whatever the saved settings say. Only this object: others keep reading the settings.
  // Returns S_OK.
  virtual HRESULT STDMETHODCALLTYPE SetZoom(int percent) = 0;
};

}  // namespace pdft
