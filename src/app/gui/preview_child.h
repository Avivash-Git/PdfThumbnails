// The body of the settings executable's internal --render-preview: a short-lived child that loads
// the handler, renders the two embedded sample PDFs at a zoom that is not saved yet and writes the
// pixels to its standard output (preview_protocol.h). The handler pins itself and PDFium and may
// end its process on a fatal error, so it never loads into the settings window itself.

#pragma once

#include <windows.h>

namespace pdft {

// Renders both samples at |zoom| percent into |edge| x |edge| pixels. Exit 0 once the frames are
// written (a sample that did not render is a frame with its failure); 8 elevated, where the
// handler does not render; 6 when no handler may be loaded; 5 when loading it or writing failed.
// It finds the DLL itself and takes no path from anyone: the class registered for this user's
// install, inside that install's build folders and of version 2 or later; in developer builds
// only, otherwise the one next to this executable.
int RenderPreviewChild(int zoom, int edge) noexcept;

}  // namespace pdft
