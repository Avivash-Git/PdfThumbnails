// What the settings window tells screen readers beyond what its controls say by themselves: names
// for controls without text, roles for the drawings, and live regions for the lines that change.

#pragma once

#include <windows.h>

#include <oleacc.h>
#include <wil/com.h>

#include <string>

namespace pdft {

class WindowAccessibility {
 public:
  // Call on the window's thread. Without the service the window works, only less well described.
  void Start() noexcept;

  void SetName(HWND control, const std::wstring& name) noexcept;
  void MarkAsPicture(HWND control) noexcept;
  // Changes to |control| are read out: politely, or at once when |urgent|.
  void MakeLiveRegion(HWND control, bool urgent) noexcept;
  // Tells screen readers that a live region changed.
  void Announce(HWND control) noexcept;

 private:
  wil::com_ptr_nothrow<IAccPropServices> service_;
};

}  // namespace pdft
