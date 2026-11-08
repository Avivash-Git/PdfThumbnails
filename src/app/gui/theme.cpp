#include "theme.h"

#include <cwchar>

namespace pdft {
namespace {

int CALLBACK FontFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
  *reinterpret_cast<bool*>(found) = true;
  return 0;
}

bool FontExists(const wchar_t* name) noexcept {
  LOGFONTW query = {};
  query.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(query.lfFaceName, name, _TRUNCATE);
  bool found = false;
  wil::unique_hdc_window screen(wil::window_dc(GetDC(nullptr)));
  if (screen) {
    EnumFontFamiliesExW(screen.get(), &query, FontFound, reinterpret_cast<LPARAM>(&found), 0);
  }
  return found;
}

HFONT MakeFont(const wchar_t* name, int points, LONG weight, UINT dpi) noexcept {
  LOGFONTW font = {};
  font.lfHeight = -MulDiv(points, static_cast<int>(dpi), 72);
  font.lfWeight = weight;
  font.lfCharSet = DEFAULT_CHARSET;
  font.lfQuality = CLEARTYPE_QUALITY;
  wcsncpy_s(font.lfFaceName, name, _TRUNCATE);
  return CreateFontIndirectW(&font);
}

}  // namespace

void WindowTheme::Rebuild(UINT dpi) noexcept {
  HIGHCONTRASTW contrast = {sizeof(contrast)};
  highContrast_ = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
                  (contrast.dwFlags & HCF_HIGHCONTRASTON);
  content_.reset(CreateSolidBrush(GetSysColor(COLOR_WINDOW)));
  footer_.reset(CreateSolidBrush(GetSysColor(COLOR_BTNFACE)));
  heading_.reset(MakeFont(L"Segoe UI", 9, FW_SEMIBOLD, dpi));
  const wchar_t* icons = FontExists(L"Segoe Fluent Icons")  ? L"Segoe Fluent Icons"
                         : FontExists(L"Segoe MDL2 Assets") ? L"Segoe MDL2 Assets"
                                                            : nullptr;
  iconFont_ = icons != nullptr;
  glyphs_.reset(MakeFont(icons ? icons : L"Segoe UI", 10, FW_NORMAL, dpi));
}

void WindowTheme::DrawGlyph(HDC dc, const RECT& area, StatusGlyph glyph) const noexcept {
  FillRect(dc, &area, content_.get());
  const wchar_t* text = nullptr;
  COLORREF color = GetSysColor(COLOR_WINDOWTEXT);
  switch (glyph) {
    case StatusGlyph::kNone:
      return;
    case StatusGlyph::kOk:
      text = iconFont_ ? L"" : L"OK";
      if (!highContrast_) color = RGB(16, 124, 16);
      break;
    case StatusGlyph::kWarning:
      text = iconFont_ ? L"" : L"!";
      if (!highContrast_) color = RGB(157, 93, 0);
      break;
    case StatusGlyph::kProblem:
      text = iconFont_ ? L"" : L"!";
      if (!highContrast_) color = RGB(196, 43, 28);
      break;
  }
  const HGDIOBJ old = SelectObject(dc, glyphs_.get());
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, color);
  RECT box = area;
  DrawTextW(dc, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(dc, old);
}

}  // namespace pdft
