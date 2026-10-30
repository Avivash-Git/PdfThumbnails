// The thumbnail zoom, a magnification in percent. 100 shows the whole page; above 100 the
// page's longer side is cropped evenly from both ends to longSide * 100 / zoom (200 shows the
// middle half), but a crop never goes past a square (see crop_math.h).

#pragma once

namespace pdft {

inline constexpr int kMinZoom = 100;
inline constexpr int kMaxZoom = 200;

// The REG_DWORD value under HKCU\<kRegKey> (project_config.h) that holds the zoom. The settings
// app writes it; the handler reads it before the 1.x settings.ini.
inline constexpr wchar_t kZoomValueName[] = L"Zoom";

// |percent| limited to kMinZoom..kMaxZoom. The parameter is a 64-bit signed integer so that every
// int, every DWORD and every long long converts to it unchanged: negative values, DWORDs above
// INT_MAX and huge values clamp to the nearest limit instead of wrapping around (the 1.x
// PowerShell manager crashed on such values).
constexpr int ClampZoom(long long percent) noexcept {
  return percent < kMinZoom ? kMinZoom : percent > kMaxZoom ? kMaxZoom : static_cast<int>(percent);
}

}  // namespace pdft
