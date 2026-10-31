#include "registry_roots.h"

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <cstring>

namespace pdft {
namespace {

// "The key or the value does not exist": an answer for readers, never worth a log line.
bool IsMissing(LSTATUS status) noexcept {
  return status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
}

// Opens |subKey| below |root| in the 64-bit view. Returns the status so callers can tell a
// missing key from a failure.
LSTATUS OpenKey(HKEY root, std::wstring_view subKey, REGSAM access, wil::unique_hkey& key) {
  const std::wstring path(subKey);
  return RegOpenKeyExW(root, path.c_str(), 0, access | KEY_WOW64_64KEY, key.put());
}

HRESULT CreateKey(HKEY root, std::wstring_view subKey, wil::unique_hkey& key) {
  const std::wstring path(subKey);
  RETURN_IF_WIN32_ERROR(RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                                        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, key.put(),
                                        nullptr));
  return S_OK;
}

HRESULT WriteValue(HKEY root, std::wstring_view subKey, std::wstring_view name, DWORD type,
                   const void* data, size_t size) {
  RETURN_HR_IF(E_INVALIDARG, root == nullptr);
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), size > kMaxRegistryValueBytes);
  wil::unique_hkey key;
  RETURN_IF_FAILED(CreateKey(root, subKey, key));
  const std::wstring valueName(name);
  RETURN_IF_WIN32_ERROR(RegSetValueExW(key.get(), valueName.c_str(), 0, type,
                                       static_cast<const BYTE*>(data), static_cast<DWORD>(size)));
  return S_OK;
}

}  // namespace

bool UsesSystemRoots(const RegistryRoots& roots) noexcept {
  return roots.user == HKEY_CURRENT_USER && roots.machine == HKEY_LOCAL_MACHINE;
}

HKEY RootOf(const RegistryRoots& roots, Scope scope) noexcept {
  switch (scope) {
    case Scope::kUser:
      return roots.user;
    case Scope::kMachine:
      return roots.machine;
    case Scope::kNone:
      break;
  }
  return nullptr;
}

HRESULT ReadRegistryValue(HKEY root, std::wstring_view subKey, std::wstring_view name,
                          std::optional<RegistryValue>* value) noexcept try {
  value->reset();
  RETURN_HR_IF(E_INVALIDARG, root == nullptr);
  wil::unique_hkey key;
  LSTATUS status = OpenKey(root, subKey, KEY_QUERY_VALUE, key);
  if (IsMissing(status)) return S_OK;
  RETURN_IF_WIN32_ERROR(status);
  const std::wstring valueName(name);
  // The value can change between asking for its size and reading it; a few rounds settle that.
  for (int attempt = 0; attempt < 4; ++attempt) {
    DWORD type = REG_NONE;
    DWORD size = 0;
    status = RegQueryValueExW(key.get(), valueName.c_str(), nullptr, &type, nullptr, &size);
    if (IsMissing(status)) return S_OK;
    RETURN_IF_WIN32_ERROR(status);
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE), size > kMaxRegistryValueBytes);
    RegistryValue result;
    result.data.resize(size);
    DWORD read = size;
    status = RegQueryValueExW(key.get(), valueName.c_str(), nullptr, &type,
                              size ? result.data.data() : nullptr, &read);
    if (status == ERROR_MORE_DATA) continue;
    if (IsMissing(status)) return S_OK;
    RETURN_IF_WIN32_ERROR(status);
    result.data.resize(read);
    result.type = type;
    *value = std::move(result);
    return S_OK;
  }
  return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
}
CATCH_RETURN();

std::optional<std::wstring> RegistryText(const RegistryValue& value) {
  if (value.type != REG_SZ && value.type != REG_EXPAND_SZ) return std::nullopt;
  std::wstring text(value.data.size() / sizeof(wchar_t), L'\0');
  if (!text.empty()) std::memcpy(text.data(), value.data.data(), text.size() * sizeof(wchar_t));
  // Stored strings usually end with a null character, but nothing enforces it: take what comes
  // before the first one, or everything.
  const size_t end = text.find(L'\0');
  if (end != std::wstring::npos) text.resize(end);
  return text;
}

std::optional<DWORD> RegistryDword(const RegistryValue& value) noexcept {
  if (value.type != REG_DWORD || value.data.size() != sizeof(DWORD)) return std::nullopt;
  DWORD result = 0;
  std::memcpy(&result, value.data.data(), sizeof(result));
  return result;
}

std::optional<ULONGLONG> RegistryQword(const RegistryValue& value) noexcept {
  if (value.type != REG_QWORD || value.data.size() != sizeof(ULONGLONG)) return std::nullopt;
  ULONGLONG result = 0;
  std::memcpy(&result, value.data.data(), sizeof(result));
  return result;
}

HRESULT ExpandEnvironment(std::wstring_view text, std::wstring* expanded) noexcept try {
  expanded->clear();
  const std::wstring input(text);
  DWORD needed = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
  RETURN_LAST_ERROR_IF(needed == 0);
  // The environment can change between the two calls (another thread); try again with the
  // larger size it then reports.
  for (int attempt = 0; attempt < 4; ++attempt) {
    std::wstring buffer(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(input.c_str(), buffer.data(), needed);
    RETURN_LAST_ERROR_IF(written == 0);
    if (written <= needed) {
      buffer.resize(written - 1);  // |written| counts the terminating null
      *expanded = std::move(buffer);
      return S_OK;
    }
    needed = written;
  }
  return HRESULT_FROM_WIN32(ERROR_MORE_DATA);
}
CATCH_RETURN();

HRESULT ReadRegistryString(HKEY root, std::wstring_view subKey, std::wstring_view name,
                           std::optional<std::wstring>* value) noexcept try {
  value->reset();
  std::optional<RegistryValue> raw;
  RETURN_IF_FAILED(ReadRegistryValue(root, subKey, name, &raw));
  if (!raw) return S_OK;
  std::optional<std::wstring> text = RegistryText(*raw);
  // An unexpected type is an answer the caller decides about, not a failure worth logging.
  if (!text) return HRESULT_FROM_WIN32(ERROR_DATATYPE_MISMATCH);
  if (raw->type == REG_EXPAND_SZ) {
    std::wstring expanded;
    RETURN_IF_FAILED(ExpandEnvironment(*text, &expanded));
    *value = std::move(expanded);
  } else {
    *value = std::move(*text);
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT RegistryKeyExists(HKEY root, std::wstring_view subKey, bool* exists) noexcept try {
  *exists = false;
  RETURN_HR_IF(E_INVALIDARG, root == nullptr);
  wil::unique_hkey key;
  const LSTATUS status = OpenKey(root, subKey, KEY_QUERY_VALUE, key);
  if (IsMissing(status)) return S_OK;
  RETURN_IF_WIN32_ERROR(status);
  *exists = true;
  return S_OK;
}
CATCH_RETURN();

HRESULT WriteRegistryString(HKEY root, std::wstring_view subKey, std::wstring_view name,
                            std::wstring_view value) noexcept try {
  const std::wstring text(value);  // with its terminating null, which REG_SZ stores
  return WriteValue(root, subKey, name, REG_SZ, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
}
CATCH_RETURN();

HRESULT WriteRegistryDword(HKEY root, std::wstring_view subKey, std::wstring_view name,
                           DWORD value) noexcept try {
  return WriteValue(root, subKey, name, REG_DWORD, &value, sizeof(value));
}
CATCH_RETURN();

HRESULT WriteRegistryQword(HKEY root, std::wstring_view subKey, std::wstring_view name,
                           ULONGLONG value) noexcept try {
  return WriteValue(root, subKey, name, REG_QWORD, &value, sizeof(value));
}
CATCH_RETURN();

HRESULT DeleteRegistryValue(HKEY root, std::wstring_view subKey, std::wstring_view name,
                            bool* deleted) noexcept try {
  if (deleted) *deleted = false;
  RETURN_HR_IF(E_INVALIDARG, root == nullptr);
  wil::unique_hkey key;
  const LSTATUS status = OpenKey(root, subKey, KEY_SET_VALUE, key);
  if (IsMissing(status)) return S_OK;
  RETURN_IF_WIN32_ERROR(status);
  const std::wstring valueName(name);
  const LSTATUS removed = RegDeleteValueW(key.get(), valueName.c_str());
  if (IsMissing(removed)) return S_OK;
  RETURN_IF_WIN32_ERROR(removed);
  if (deleted) *deleted = true;
  return S_OK;
}
CATCH_RETURN();

HRESULT DeleteRegistryTree(HKEY root, std::wstring_view subKey, bool* deleted) noexcept try {
  if (deleted) *deleted = false;
  // Never the root itself: an empty path would empty the whole hive the root stands for.
  RETURN_HR_IF(E_INVALIDARG, root == nullptr || subKey.empty());
  {
    wil::unique_hkey key;
    const LSTATUS status = OpenKey(
        root, subKey, DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | KEY_SET_VALUE, key);
    if (IsMissing(status)) return S_OK;
    RETURN_IF_WIN32_ERROR(status);
    // Everything below the key (its values and subkeys), then the key itself.
    RETURN_IF_WIN32_ERROR(RegDeleteTreeW(key.get(), nullptr));
  }
  const std::wstring path(subKey);
  const LSTATUS status = RegDeleteKeyExW(root, path.c_str(), KEY_WOW64_64KEY, 0);
  if (IsMissing(status)) return S_OK;  // deleted by someone else meanwhile
  RETURN_IF_WIN32_ERROR(status);
  if (deleted) *deleted = true;
  return S_OK;
}
CATCH_RETURN();

HRESULT DeleteRegistryKeyIfEmpty(HKEY root, std::wstring_view subKey, bool* deleted) noexcept try {
  if (deleted) *deleted = false;
  RETURN_HR_IF(E_INVALIDARG, root == nullptr || subKey.empty());
  {
    wil::unique_hkey key;
    const LSTATUS status = OpenKey(root, subKey, KEY_QUERY_VALUE, key);
    if (IsMissing(status)) return S_OK;
    RETURN_IF_WIN32_ERROR(status);
    DWORD subkeys = 0;
    DWORD values = 0;
    RETURN_IF_WIN32_ERROR(RegQueryInfoKeyW(key.get(), nullptr, nullptr, nullptr, &subkeys, nullptr,
                                           nullptr, &values, nullptr, nullptr, nullptr, nullptr));
    if (subkeys != 0 || values != 0) return S_OK;
  }
  // RegDeleteKeyExW deletes only a key without subkeys; one added meanwhile makes it fail.
  const std::wstring path(subKey);
  const LSTATUS status = RegDeleteKeyExW(root, path.c_str(), KEY_WOW64_64KEY, 0);
  if (IsMissing(status)) return S_OK;
  RETURN_IF_WIN32_ERROR(status);
  if (deleted) *deleted = true;
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
