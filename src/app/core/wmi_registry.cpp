#include "wmi_registry.h"

// oleauto.h before (another inclusion of) wil/resource.h enables WIL's BSTR and VARIANT wrappers.
#include <oleauto.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <memory>

namespace pdft {
namespace {

constexpr wchar_t kNamespace[] = L"ROOT\\DEFAULT";
constexpr wchar_t kProvider[] = L"StdRegProv";

// A VARIANT holding a copy of |text|.
HRESULT MakeBstrVariant(std::wstring_view text, wil::unique_variant* out) noexcept {
  RETURN_HR_IF(E_INVALIDARG, text.size() > MAXUINT32 / sizeof(wchar_t));
  BSTR value = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
  RETURN_IF_NULL_ALLOC(value);
  out->reset();
  out->vt = VT_BSTR;
  out->bstrVal = value;
  return S_OK;
}

}  // namespace

HRESULT RegistryThroughWmi::Connect() noexcept {
  services_.reset();
  class_.reset();
  wil::com_ptr_nothrow<IWbemLocator> locator;
  RETURN_IF_FAILED(CoCreateInstance(__uuidof(WbemLocator), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(locator.put())));
  wil::unique_bstr name = wil::make_bstr_nothrow(kNamespace);
  RETURN_IF_NULL_ALLOC(name);
  RETURN_IF_FAILED(locator->ConnectServer(name.get(), nullptr, nullptr, nullptr,
                                          WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr,
                                          services_.put()));
  // Impersonate: the provider must read the registry as this user, not as itself.
  RETURN_IF_FAILED(CoSetProxyBlanket(services_.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                                     RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE,
                                     nullptr, EOAC_NONE));
  wil::unique_bstr provider = wil::make_bstr_nothrow(kProvider);
  RETURN_IF_NULL_ALLOC(provider);
  RETURN_IF_FAILED(services_->GetObject(provider.get(), 0, nullptr, class_.put(), nullptr));
  return S_OK;
}

HRESULT RegistryThroughWmi::Call(const wchar_t* method, DWORD root, std::wstring_view subKey,
                                 const std::wstring_view* valueName,
                                 wil::com_ptr_nothrow<IWbemClassObject>* out,
                                 bool* found) noexcept {
  *found = false;
  out->reset();
  RETURN_HR_IF(E_NOT_VALID_STATE, !services_ || !class_);
  wil::com_ptr_nothrow<IWbemClassObject> signature;
  RETURN_IF_FAILED(class_->GetMethod(method, 0, signature.put(), nullptr));
  wil::com_ptr_nothrow<IWbemClassObject> in;
  RETURN_IF_FAILED(signature->SpawnInstance(0, in.put()));

  wil::unique_variant rootValue;
  rootValue.vt = VT_I4;  // WMI takes a uint32 as VT_I4
  rootValue.lVal = static_cast<LONG>(root);
  RETURN_IF_FAILED(in->Put(L"hDefKey", 0, &rootValue, 0));
  wil::unique_variant keyValue;
  RETURN_IF_FAILED(MakeBstrVariant(subKey, &keyValue));
  RETURN_IF_FAILED(in->Put(L"sSubKeyName", 0, &keyValue, 0));
  if (valueName) {
    wil::unique_variant nameValue;
    RETURN_IF_FAILED(MakeBstrVariant(*valueName, &nameValue));
    RETURN_IF_FAILED(in->Put(L"sValueName", 0, &nameValue, 0));
  }

  wil::unique_bstr provider = wil::make_bstr_nothrow(kProvider);
  RETURN_IF_NULL_ALLOC(provider);
  wil::unique_bstr methodName = wil::make_bstr_nothrow(method);
  RETURN_IF_NULL_ALLOC(methodName);
  RETURN_IF_FAILED(services_->ExecMethod(provider.get(), methodName.get(), 0, nullptr, in.get(),
                                         out->put(), nullptr));
  wil::unique_variant result;
  RETURN_IF_FAILED((*out)->Get(L"ReturnValue", 0, &result, nullptr, nullptr));
  RETURN_HR_IF(E_UNEXPECTED, result.vt != VT_I4);
  const DWORD code = static_cast<DWORD>(result.lVal);
  // No such key (2), or for a value, no such value in an existing key (1, which StdRegProv uses
  // for that rather than ERROR_INVALID_FUNCTION): an answer, not a failure. A value of another
  // type comes back as WBEM_E_TYPE_MISMATCH and fails.
  if (code == ERROR_FILE_NOT_FOUND || (valueName && code == 1)) return S_OK;
  RETURN_IF_WIN32_ERROR(code);
  *found = true;
  return S_OK;
}

HRESULT RegistryThroughWmi::GetString(DWORD root, std::wstring_view subKey,
                                      std::wstring_view valueName,
                                      std::optional<std::wstring>* value) noexcept try {
  value->reset();
  wil::com_ptr_nothrow<IWbemClassObject> out;
  bool found = false;
  RETURN_IF_FAILED(Call(L"GetStringValue", root, subKey, &valueName, std::addressof(out), &found));
  if (!found) return S_OK;
  wil::unique_variant data;
  RETURN_IF_FAILED(out->Get(L"sValue", 0, &data, nullptr, nullptr));
  if (data.vt == VT_BSTR && data.bstrVal) {
    value->emplace(data.bstrVal, SysStringLen(data.bstrVal));
  } else {
    RETURN_HR_IF(E_UNEXPECTED, data.vt != VT_NULL);
    value->emplace();  // an empty string comes back as NULL
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT RegistryThroughWmi::GetDword(DWORD root, std::wstring_view subKey,
                                     std::wstring_view valueName,
                                     std::optional<DWORD>* value) noexcept {
  value->reset();
  wil::com_ptr_nothrow<IWbemClassObject> out;
  bool found = false;
  RETURN_IF_FAILED(Call(L"GetDWORDValue", root, subKey, &valueName, std::addressof(out), &found));
  if (!found) return S_OK;
  wil::unique_variant data;
  RETURN_IF_FAILED(out->Get(L"uValue", 0, &data, nullptr, nullptr));
  RETURN_HR_IF(E_UNEXPECTED, data.vt != VT_I4);
  *value = static_cast<DWORD>(data.lVal);
  return S_OK;
}

HRESULT RegistryThroughWmi::EnumKeys(DWORD root, std::wstring_view subKey,
                                     std::vector<std::wstring>* names) noexcept try {
  names->clear();
  wil::com_ptr_nothrow<IWbemClassObject> out;
  bool found = false;
  RETURN_IF_FAILED(Call(L"EnumKey", root, subKey, nullptr, std::addressof(out), &found));
  if (!found) return S_OK;
  wil::unique_variant data;
  RETURN_IF_FAILED(out->Get(L"sNames", 0, &data, nullptr, nullptr));
  if (data.vt == VT_NULL) return S_OK;  // no subkeys
  RETURN_HR_IF(E_UNEXPECTED, data.vt != (VT_ARRAY | VT_BSTR) || !data.parray);
  LONG lower = 0;
  LONG upper = -1;
  RETURN_IF_FAILED(SafeArrayGetLBound(data.parray, 1, &lower));
  RETURN_IF_FAILED(SafeArrayGetUBound(data.parray, 1, &upper));
  for (LONG i = lower; i <= upper; ++i) {
    BSTR item = nullptr;
    RETURN_IF_FAILED(SafeArrayGetElement(data.parray, &i, &item));
    wil::unique_bstr owned(item);
    names->emplace_back(item ? item : L"", item ? SysStringLen(item) : 0);
  }
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
