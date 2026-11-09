#include <windows.h>

#include <objbase.h>

// Defines the property GUIDs here, so that nothing has to link oleacc.lib.
#include <initguid.h>
#include <oleacc.h>
#include <uiautomation.h>

#include "accessibility.h"

namespace pdft {
namespace {

constexpr LONG kLivePolite = 1;     // LiveSetting Polite
constexpr LONG kLiveAssertive = 2;  // LiveSetting Assertive

}  // namespace

void WindowAccessibility::Start() noexcept {
  (void)CoCreateInstance(__uuidof(CAccPropServices), nullptr, CLSCTX_INPROC_SERVER,
                         IID_PPV_ARGS(service_.put()));
}

void WindowAccessibility::SetName(HWND control, const std::wstring& name) noexcept {
  if (service_) {
    (void)service_->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                                   PROPID_ACC_NAME, name.c_str());
  }
}

void WindowAccessibility::SetValueText(HWND control, const std::wstring& value) noexcept {
  if (service_) {
    (void)service_->SetHwndPropStr(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                                   PROPID_ACC_VALUE, value.c_str());
  }
}

void WindowAccessibility::MarkAsPicture(HWND control) noexcept {
  if (!service_) return;
  VARIANT role = {};
  role.vt = VT_I4;
  role.lVal = ROLE_SYSTEM_GRAPHIC;
  (void)service_->SetHwndProp(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                              PROPID_ACC_ROLE, role);
}

void WindowAccessibility::MakeLiveRegion(HWND control, bool urgent) noexcept {
  if (!service_) return;
  VARIANT setting = {};
  setting.vt = VT_I4;
  setting.lVal = urgent ? kLiveAssertive : kLivePolite;
  (void)service_->SetHwndProp(control, static_cast<DWORD>(OBJID_CLIENT), CHILDID_SELF,
                              LiveSetting_Property_GUID, setting);
}

void WindowAccessibility::Announce(HWND control) noexcept {
  NotifyWinEvent(EVENT_OBJECT_LIVEREGIONCHANGED, control, OBJID_CLIENT, CHILDID_SELF);
}

}  // namespace pdft
