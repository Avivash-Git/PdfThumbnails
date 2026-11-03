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

// What a key that may be deleted is opened with: enough to list its subkeys and values, to read
// whether it is a link, and to delete it.
constexpr REGSAM kDeletableKeyAccess = DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE;

// Opens |subKey| below |parent| in the 64-bit view as it is: when the key itself is a link, the
// link, never the key it names. The path on the way to it is looked up as given.
LSTATUS OpenKeyItself(HKEY parent, std::wstring_view subKey, wil::unique_hkey& key) {
  const std::wstring path(subKey);
  return RegOpenKeyExW(parent, path.c_str(), REG_OPTION_OPEN_LINK,
                       kDeletableKeyAccess | KEY_WOW64_64KEY, key.put());
}

// Whether the open |key| (with KEY_QUERY_VALUE) has subkeys or values.
LSTATUS HasContents(HKEY key, bool* contents) noexcept {
  DWORD subkeys = 0;
  DWORD values = 0;
  const LSTATUS status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr,
                                          nullptr, &values, nullptr, nullptr, nullptr, nullptr);
  *contents = subkeys != 0 || values != 0;
  return status;
}

// The value in which a registry link holds the full name of the key it leads to (REG_LINK).
constexpr wchar_t kSymbolicLinkValue[] = L"SymbolicLinkValue";

// Whether |key|, opened by OpenKeyItself, is a registry link. Any key may hold a REG_LINK value
// of that name, and one that does counts as a link here too: it is only left alone.
HRESULT IsRegistryLink(HKEY key, bool* link) noexcept {
  *link = false;
  DWORD type = REG_NONE;
  const LSTATUS status =
      RegQueryValueExW(key, kSymbolicLinkValue, nullptr, &type, nullptr, nullptr);
  if (IsMissing(status)) return S_OK;
  RETURN_IF_WIN32_ERROR(status);
  *link = type == REG_LINK;
  return S_OK;
}

// ntdll's NtDeleteKey and its conversion of status codes to Windows errors. No Win32 call deletes
// the key an open handle refers to: RegDeleteKeyExW opens the key again by a name (an empty one
// for the handle's own key) without REG_OPTION_OPEN_LINK, so on a link it may reach the key the
// link names. Every process has ntdll loaded.
struct NtKeyFunctions {
  LONG(NTAPI* deleteKey)(HANDLE key) = nullptr;
  ULONG(NTAPI* toWin32Error)(LONG status) = nullptr;
};

const NtKeyFunctions& NtKey() noexcept {
  static const NtKeyFunctions functions = [] {
    NtKeyFunctions found;
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
      found.deleteKey = reinterpret_cast<decltype(found.deleteKey)>(
          reinterpret_cast<void*>(GetProcAddress(ntdll, "NtDeleteKey")));
      found.toWin32Error = reinterpret_cast<decltype(found.toWin32Error)>(
          reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlNtStatusToDosError")));
    }
    return found;
  }();
  return functions;
}

// One DeleteRegistryTree call's walk: its bounds, and what it did so far.
struct TreeWalk {
  RegistryTreeLimits limits;
  ULONGLONG start = 0;
  DWORD opened = 0;  // keys opened, the top one included
  RegistryTreeDeletion* deletion = nullptr;

  bool OutOfTime() const noexcept { return GetTickCount64() - start >= limits.maxMs; }
  // Returned as it is, never through a WIL macro: it is an answer the callers report, not a
  // failure of this call worth a log line here.
  HRESULT Stop() noexcept {
    deletion->boundReached = true;
    return kRegistryTreeLeftHr;
  }
};

// Deletes |key| (opened by OpenKeyItself), |depth| levels below the top of the tree, with
// everything below it. Every subkey is opened relative to |key|'s handle and deleted through its
// own handle once its own subkeys are gone. A link stays, and so does every key above it, while
// the walk goes on with their siblings; past the bounds it stops at once.
HRESULT DeleteKeyAndBelow(TreeWalk& walk, HKEY key, DWORD depth) {
  bool link = false;
  RETURN_IF_FAILED(IsRegistryLink(key, &link));
  if (link) {
    ++walk.deletion->linksLeft;
    return kRegistryTreeLeftHr;
  }
  bool left = false;
  for (DWORD index = 0;;) {
    if (walk.OutOfTime()) return walk.Stop();
    wchar_t name[256] = {};  // key names are at most 255 characters
    DWORD size = ARRAYSIZE(name);
    const LSTATUS status =
        RegEnumKeyExW(key, index, name, &size, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    RETURN_IF_WIN32_ERROR(status);
    if (depth >= walk.limits.maxDepth || walk.opened >= walk.limits.maxKeys) return walk.Stop();
    wil::unique_hkey child;
    const LSTATUS opened = OpenKeyItself(key, std::wstring_view(name, size), child);
    if (IsMissing(opened)) continue;  // deleted meanwhile: the next one moved up to |index|
    RETURN_IF_WIN32_ERROR(opened);
    ++walk.opened;
    const HRESULT hr = DeleteKeyAndBelow(walk, child.get(), depth + 1);
    if (hr == kRegistryTreeLeftHr) {
      if (walk.deletion->boundReached) return hr;
      left = true;  // it stays where it is: on to the next one
      ++index;
      continue;
    }
    RETURN_IF_FAILED(hr);
  }
  if (left) return kRegistryTreeLeftHr;
  RETURN_IF_FAILED(DeleteOpenRegistryKey(key));
  ++walk.deletion->keysDeleted;
  return S_OK;
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

HRESULT DeleteRegistryTree(HKEY root, std::wstring_view subKey, const RegistryTreeLimits& limits,
                           RegistryTreeDeletion* deletion) noexcept try {
  *deletion = RegistryTreeDeletion{};
  // Never the root itself: an empty path would empty the whole hive the root stands for.
  RETURN_HR_IF(E_INVALIDARG, root == nullptr || subKey.empty());
  TreeWalk walk{limits, GetTickCount64(), 1, deletion};
  wil::unique_hkey key;
  const LSTATUS status = OpenKeyItself(root, subKey, key);
  if (IsMissing(status)) return S_OK;
  RETURN_IF_WIN32_ERROR(status);
  deletion->existed = true;
  const HRESULT hr = DeleteKeyAndBelow(walk, key.get(), 0);
  if (hr == kRegistryTreeLeftHr) return hr;
  RETURN_IF_FAILED(hr);
  deletion->deleted = true;
  return S_OK;
}
CATCH_RETURN();

HRESULT DeleteRegistryTree(HKEY root, std::wstring_view subKey, bool* deleted) noexcept {
  if (deleted) *deleted = false;
  RegistryTreeDeletion deletion;
  const HRESULT hr = DeleteRegistryTree(root, subKey, RegistryTreeLimits{}, &deletion);
  if (deleted) *deleted = deletion.deleted;
  return hr;
}

HRESULT DeleteRegistryKeyIfEmpty(HKEY root, std::wstring_view subKey, bool* deleted) noexcept try {
  if (deleted) *deleted = false;
  RETURN_HR_IF(E_INVALIDARG, root == nullptr || subKey.empty());
  wil::unique_hkey key;
  const LSTATUS status = OpenKeyItself(root, subKey, key);
  if (IsMissing(status)) return S_OK;
  if (status == ERROR_ACCESS_DENIED) {
    // A key this process may not delete matters only when it is empty: one with subkeys or values
    // stays anyway, which is no failure of this call. Its permissions are someone's choice.
    const std::wstring path(subKey);
    wil::unique_hkey readable;
    const LSTATUS opened = RegOpenKeyExW(root, path.c_str(), REG_OPTION_OPEN_LINK,
                                         KEY_QUERY_VALUE | KEY_WOW64_64KEY, readable.put());
    if (IsMissing(opened)) return S_OK;
    bool contents = false;
    if (opened == ERROR_SUCCESS && HasContents(readable.get(), &contents) == ERROR_SUCCESS &&
        contents) {
      return S_OK;
    }
  }
  RETURN_IF_WIN32_ERROR(status);
  bool contents = false;
  RETURN_IF_WIN32_ERROR(HasContents(key.get(), &contents));
  if (contents) return S_OK;
  // Through the handle: a subkey added meanwhile makes the deletion fail, and a key made at the
  // same path meanwhile is another key, which stays.
  RETURN_IF_FAILED(DeleteOpenRegistryKey(key.get()));
  if (deleted) *deleted = true;
  return S_OK;
}
CATCH_RETURN();

HRESULT DeleteOpenRegistryKey(HKEY key) noexcept {
  RETURN_HR_IF(E_INVALIDARG, key == nullptr);
  const NtKeyFunctions& nt = NtKey();
  RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND), !nt.deleteKey || !nt.toWin32Error);
  const LONG status = nt.deleteKey(key);
  if (status >= 0) return S_OK;  // NT_SUCCESS
  const ULONG error = nt.toWin32Error(status);
  if (error == ERROR_KEY_DELETED) return S_OK;  // by someone else meanwhile: gone all the same
  RETURN_WIN32(error);
}

}  // namespace pdft
