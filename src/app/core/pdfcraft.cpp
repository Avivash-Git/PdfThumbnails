#include "pdfcraft.h"

#include <msi.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include "build_folder.h"
#include "delay_load.h"
#include "project_config.h"
#include "registration.h"
#include "strings.h"

namespace pdft {
namespace {

// A product code is a GUID in braces: 38 characters and the null.
constexpr DWORD kProductCodeChars = 39;

// Longest property value read from Windows Installer; names and versions are far shorter.
constexpr DWORD kMaxPropertyChars = 1024;

// Products and Uninstall entries looked at, at most: far more than any PC has, so a broken
// enumeration cannot keep going for ever.
constexpr DWORD kMaxEntries = 100'000;

// Registry key names are at most 255 characters.
constexpr DWORD kMaxKeyNameChars = 256;

HRESULT MsiError(UINT error) noexcept { return HRESULT_FROM_WIN32(error); }

// One property of an installed product (MsiGetProductInfoExW). |sid| is nullptr for the current
// user or a machine-wide product. A product or property Windows Installer does not know is "".
HRESULT ReadProductProperty(const wchar_t* product, const wchar_t* sid, MSIINSTALLCONTEXT context,
                            const wchar_t* property, std::wstring* value) {
  value->clear();
  std::wstring buffer(64, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD chars = static_cast<DWORD>(buffer.size());
    const UINT error = MsiGetProductInfoExW(product, sid, context, property, buffer.data(), &chars);
    if (error == ERROR_SUCCESS) {
      buffer.resize(chars);
      *value = std::move(buffer);
      return S_OK;
    }
    if (error == ERROR_UNKNOWN_PRODUCT || error == ERROR_UNKNOWN_PROPERTY) return S_OK;
    if (error != ERROR_MORE_DATA) return MsiError(error);
    // |chars| is the length without the null.
    if (chars >= kMaxPropertyChars) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    buffer.assign(chars + 1, L'\0');
  }
  return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
}

// The same for a product found by UpgradeCode, whose context is not known.
HRESULT ReadProductInfo(const wchar_t* product, const wchar_t* property, std::wstring* value) {
  value->clear();
  std::wstring buffer(64, L'\0');
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD chars = static_cast<DWORD>(buffer.size());
    const UINT error = MsiGetProductInfoW(product, property, buffer.data(), &chars);
    if (error == ERROR_SUCCESS) {
      buffer.resize(chars);
      *value = std::move(buffer);
      return S_OK;
    }
    if (error == ERROR_UNKNOWN_PRODUCT || error == ERROR_UNKNOWN_PROPERTY) return S_OK;
    if (error != ERROR_MORE_DATA) return MsiError(error);
    if (chars >= kMaxPropertyChars) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    buffer.assign(chars + 1, L'\0');
  }
  return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
}

// Installed, not merely advertised or registered for another user.
bool IsInstalled(const wchar_t* product) noexcept {
  return MsiQueryProductStateW(product) == INSTALLSTATE_DEFAULT;
}

HRESULT FindByUpgradeCode(PdfCraftInfo* info) {
  for (DWORD index = 0; index < kMaxEntries; ++index) {
    wchar_t product[kProductCodeChars] = {};
    const UINT error = MsiEnumRelatedProductsW(kPdfCraftUpgradeCodeString, 0, index, product);
    if (error == ERROR_NO_MORE_ITEMS) return S_OK;
    if (error != ERROR_SUCCESS) return MsiError(error);
    if (!IsInstalled(product)) continue;
    PdfCraftInfo found;
    found.installed = true;
    found.productCode = product;
    found.source = PdfCraftSource::kUpgradeCode;
    // Without a version it is still installed; the status then shows no number.
    std::wstring version;
    if (SUCCEEDED(ReadProductInfo(product, INSTALLPROPERTY_VERSIONSTRING, &version))) {
      found.version = CleanVersionText(version);
    }
    *info = info->installed ? NewerPdfCraft(*info, found) : found;
  }
  return S_OK;
}

HRESULT FindByProductName(PdfCraftInfo* info) {
  for (DWORD index = 0; index < kMaxEntries; ++index) {
    wchar_t product[kProductCodeChars] = {};
    MSIINSTALLCONTEXT context = MSIINSTALLCONTEXT_NONE;
    wchar_t sid[256] = {};
    DWORD sidChars = ARRAYSIZE(sid);
    // nullptr as the user: this user's products and the machine's.
    const UINT error = MsiEnumProductsExW(nullptr, nullptr, MSIINSTALLCONTEXT_ALL, index, product,
                                          &context, sid, &sidChars);
    if (error == ERROR_NO_MORE_ITEMS) return S_OK;
    if (error == ERROR_MORE_DATA) continue;  // a SID longer than any real one: not this user
    if (error != ERROR_SUCCESS) return MsiError(error);
    // Machine-wide products take no SID.
    const wchar_t* user = context == MSIINSTALLCONTEXT_MACHINE || sid[0] == L'\0' ? nullptr : sid;
    // One product whose data Windows Installer cannot give (a broken registration) does not
    // end the search for another.
    std::wstring name;
    if (FAILED(ReadProductProperty(product, user, context, INSTALLPROPERTY_PRODUCTNAME, &name))) {
      continue;
    }
    if (!IsPdfCraftName(name) || !IsInstalled(product)) continue;
    PdfCraftInfo found;
    found.installed = true;
    found.productCode = product;
    found.source = PdfCraftSource::kProductName;
    std::wstring version;
    if (SUCCEEDED(
            ReadProductProperty(product, user, context, INSTALLPROPERTY_VERSIONSTRING, &version))) {
      found.version = CleanVersionText(version);
    }
    *info = info->installed ? NewerPdfCraft(*info, found) : found;
  }
  return S_OK;
}

// The Uninstall entries under |root| in one registry view.
HRESULT ScanUninstallKey(HKEY root, REGSAM view, PdfCraftInfo* info) {
  wil::unique_hkey key;
  LSTATUS status = RegOpenKeyExW(root, kUninstallKey, 0,
                                 KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | view, key.put());
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return S_OK;
  RETURN_IF_WIN32_ERROR(status);
  for (DWORD index = 0; index < kMaxEntries; ++index) {
    wchar_t name[kMaxKeyNameChars] = {};
    DWORD nameChars = ARRAYSIZE(name);
    status = RegEnumKeyExW(key.get(), index, name, &nameChars, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) return S_OK;
    if (status == ERROR_MORE_DATA) continue;
    RETURN_IF_WIN32_ERROR(status);
    wil::unique_hkey entry;
    status = RegOpenKeyExW(key.get(), name, 0, KEY_QUERY_VALUE | view, entry.put());
    // Deleted meanwhile, or not readable by this user: skipped, as if it were not there.
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_ACCESS_DENIED) continue;
    RETURN_IF_WIN32_ERROR(status);
    // Any installer writes these entries; one that cannot be read (an oversized value, say) is
    // skipped rather than ending the search.
    std::optional<RegistryValue> raw;
    if (FAILED(ReadRegistryValue(entry.get(), L"", L"DisplayName", &raw))) continue;
    const std::optional<std::wstring> displayName = raw ? RegistryText(*raw) : std::nullopt;
    if (!displayName || !IsPdfCraftName(*displayName)) continue;
    PdfCraftInfo found;
    found.installed = true;
    found.source = PdfCraftSource::kUninstallKey;
    if (IsGuidString(name)) found.productCode = name;
    if (FAILED(ReadRegistryValue(entry.get(), L"", L"DisplayVersion", &raw))) raw.reset();
    if (const std::optional<std::wstring> version = raw ? RegistryText(*raw) : std::nullopt) {
      found.version = CleanVersionText(*version);
    }
    *info = info->installed ? NewerPdfCraft(*info, found) : found;
  }
  return S_OK;
}

}  // namespace

bool IsPdfCraftName(std::wstring_view name) noexcept { return IEquals(name, kPdfCraftProductName); }

std::wstring CleanVersionText(std::wstring_view version) {
  std::wstring text;
  for (const wchar_t c : version) {
    if (text.size() >= kMaxPdfCraftVersionChars) break;
    if (c >= 0x20 && c != 0x7F) text.push_back(c);
  }
  return text;
}

const PdfCraftInfo& NewerPdfCraft(const PdfCraftInfo& a, const PdfCraftInfo& b) noexcept {
  return CompareFileVersions(b.version, a.version) > 0 ? b : a;
}

HRESULT DetectPdfCraftWithInstaller(PdfCraftInfo* info) noexcept try {
  *info = {};
  RETURN_IF_FAILED(EnsureDelayLoaded("msi.dll"));
  RETURN_IF_FAILED(FindByUpgradeCode(info));
  if (info->installed) return S_OK;
  return FindByProductName(info);
}
CATCH_RETURN();

HRESULT DetectPdfCraftInUninstallKeys(const RegistryRoots& roots, PdfCraftInfo* info) noexcept try {
  *info = {};
  RETURN_IF_FAILED(ScanUninstallKey(roots.machine, KEY_WOW64_64KEY, info));
  RETURN_IF_FAILED(ScanUninstallKey(roots.machine, KEY_WOW64_32KEY, info));
  return ScanUninstallKey(roots.user, KEY_WOW64_64KEY, info);
}
CATCH_RETURN();

HRESULT DetectPdfCraft(PdfCraftInfo* info) noexcept try {
  *info = {};
  PdfCraftInfo found;
  HRESULT hr = DetectPdfCraftWithInstaller(&found);
  if (hr == E_OUTOFMEMORY) return hr;
  if (SUCCEEDED(hr) && found.installed) {
    *info = std::move(found);
    return S_OK;
  }
  // A failure above is in the log (WIL); the Uninstall keys may still know.
  hr = DetectPdfCraftInUninstallKeys(RegistryRoots{}, &found);
  if (hr == E_OUTOFMEMORY) return hr;
  if (SUCCEEDED(hr)) *info = std::move(found);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
