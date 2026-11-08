// The settings window's fonts, brushes and status glyphs. System colours only, so High Contrast
// and any Windows colour choice simply work; there is no dark mode for classic controls.

#pragma once

#include <windows.h>

#include <wil/resource.h>

#include "status_panel.h"

namespace pdft {

class WindowTheme {
 public:
  // (Re)makes the fonts for |dpi|, and the brushes.
  void Rebuild(UINT dpi) noexcept;

  HFONT heading() const noexcept { return heading_.get(); }
  HBRUSH content() const noexcept { return content_.get(); }
  HBRUSH footer() const noexcept { return footer_.get(); }

  // Draws |glyph| centred in |area|, on the content background.
  void DrawGlyph(HDC dc, const RECT& area, StatusGlyph glyph) const noexcept;

 private:
  wil::unique_hfont heading_;
  wil::unique_hfont glyphs_;
  bool iconFont_ = false;  // |glyphs_| is one of Windows' icon fonts
  bool highContrast_ = false;
  wil::unique_hbrush content_;
  wil::unique_hbrush footer_;
};

}  // namespace pdft
