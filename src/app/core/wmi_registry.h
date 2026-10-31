// Reading the registry through WMI's registry provider (StdRegProv in root\default).
//
// The provider runs in WMI's own process and impersonates the caller, so it reads the user's
// real hive. A process inside an app container (an MSIX-packaged app and everything it starts)
// is redirected to a private copy of HKCU instead. Comparing the two views is how the app tells
// whether its registry writes would reach Windows (context.h).

#pragma once

#include <windows.h>

#include <wbemcli.h>
#include <wil/com.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pdft {

class RegistryThroughWmi {
 public:
  // StdRegProv's hDefKey values for the two roots the app reads.
  static constexpr DWORD kLocalMachine = 0x80000002;  // HKEY_LOCAL_MACHINE
  static constexpr DWORD kUsers = 0x80000003;         // HKEY_USERS (HKCU is HKEY_USERS\<SID>)

  // Connects to root\default, impersonating this process's user. Needs COM initialized
  // with InitializeComSecurity (com_security.h).
  [[nodiscard]] HRESULT Connect() noexcept;

  // A REG_SZ value ("" names the default value). *value is std::nullopt when the key or the
  // value does not exist.
  [[nodiscard]] HRESULT GetString(DWORD root, std::wstring_view subKey, std::wstring_view valueName,
                                  std::optional<std::wstring>* value) noexcept;
  // A REG_DWORD value, likewise.
  [[nodiscard]] HRESULT GetDword(DWORD root, std::wstring_view subKey, std::wstring_view valueName,
                                 std::optional<DWORD>* value) noexcept;
  // The names of the subkeys of |subKey|; empty when it has none or does not exist.
  [[nodiscard]] HRESULT EnumKeys(DWORD root, std::wstring_view subKey,
                                 std::vector<std::wstring>* names) noexcept;

 private:
  // Runs StdRegProv.<method>. *found is false when the provider answered "no such key" or, for
  // a value (|valueName| given), "no such value"; any other provider error fails.
  HRESULT Call(const wchar_t* method, DWORD root, std::wstring_view subKey,
               const std::wstring_view* valueName, wil::com_ptr_nothrow<IWbemClassObject>* out,
               bool* found) noexcept;

  wil::com_ptr_nothrow<IWbemServices> services_;
  wil::com_ptr_nothrow<IWbemClassObject> class_;
};

}  // namespace pdft
