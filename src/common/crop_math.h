// Where a thumbnail's page goes: the size of the bitmap, and the rectangle (in bitmap pixels)
// that the whole page is drawn into. The handler renders with these numbers, and the tests check
// them without PDFium.
//
// The bitmap fits an |edge| x |edge| square and keeps the page's aspect ratio. Above 100 % the
// zoom crops the page's longer side evenly from both ends (top and bottom of a portrait page), so
// the rest fills more of the square, but never crops past a square. The formula is 1.1's,
// operation for operation, so thumbnails stay pixel-identical: keep it that way.

#pragma once

#include "zoom.h"

namespace pdft {

// Explorer does not ask for more than 2560 px; ComputeCrop caps the edge there anyway.
inline constexpr unsigned kMaxEdge = 2560;

struct CropGeometry {
  int bmpW, bmpH;         // the bitmap: at most edge x edge, at least 1 x 1 for edge >= 1
  int pageLeft, pageTop;  // where the whole page starts: <= 0, centring the part that shows
  int pageW, pageH;       // the whole page at the chosen scale (at most 2x edge), at least 1 x 1
};

// The geometry of a page of |pageWpt| x |pageHpt| points (positive and finite; the handler only
// renders pages from 1 to 1e6 points) drawn to fit |edge| x |edge| pixels at |zoom| percent.
// |edge| is capped at kMaxEdge and |zoom| clamped to kMinZoom..kMaxZoom first, so the handler,
// which already does both, gets the same numbers as 1.1. The offsets halve with C++ integer
// division, which truncates toward zero: an odd overhang leaves the extra pixel at the far end.
// Edge 0 gives a 0 x 0 bitmap (and a 1 x 1 page): no image, which is why GetThumbnail refuses
// cx == 0 with E_INVALIDARG before it renders anything.
constexpr CropGeometry ComputeCrop(double pageWpt, double pageHpt, unsigned edge,
                                   int zoom) noexcept {
  if (edge > kMaxEdge) edge = kMaxEdge;
  zoom = ClampZoom(zoom);
  const double longSide = pageWpt >= pageHpt ? pageWpt : pageHpt;
  const double shortSide = pageWpt >= pageHpt ? pageHpt : pageWpt;
  double visible = longSide * 100.0 / zoom;  // points of the longer side that stay
  if (visible < shortSide) visible = shortSide;
  const double scale = edge / visible;  // pixels per point
  CropGeometry crop = {};
  // The whole page at that scale and the part of it we keep.
  crop.pageW = static_cast<int>(pageWpt * scale + 0.5);
  crop.pageH = static_cast<int>(pageHpt * scale + 0.5);
  if (crop.pageW < 1) crop.pageW = 1;
  if (crop.pageH < 1) crop.pageH = 1;
  crop.bmpW = crop.pageW < static_cast<int>(edge) ? crop.pageW : static_cast<int>(edge);
  crop.bmpH = crop.pageH < static_cast<int>(edge) ? crop.pageH : static_cast<int>(edge);
  crop.pageLeft = (crop.bmpW - crop.pageW) / 2;
  crop.pageTop = (crop.bmpH - crop.pageH) / 2;
  return crop;
}

}  // namespace pdft
