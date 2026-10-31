// Where the check engine reads and writes the registry: the per-user and the machine-wide root,
// injectable so that tests run every registry access of the engine against a private key
// instead of the user's real hive.
//
// Paths are relative to a root ("Software\\Classes\\CLSID\\{...}") and always use the 64-bit
// view (KEY_WOW64_64KEY), the one Explorer and its thumbnail surrogate read. Values are read
// into bounded buffers and checked for their type: the registry is input like any other, and a
// value anyone in the session could have written must not be trusted to be well formed.

#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "exit_code.h"

namespace pdft {

struct RegistryRoots {
  HKEY user = HKEY_CURRENT_USER;
  HKEY machine = HKEY_LOCAL_MACHINE;
};

// True when |roots| are the real predefined keys. Side effects that reach beyond the registry
// and cannot be redirected (telling Explorer that associations changed, ...) run only then, so a
// test that injects private roots never announces changes it made in its sandbox. A process that
// remapped HKEY_CURRENT_USER with RegOverridePredefKey still counts as real here.
bool UsesSystemRoots(const RegistryRoots& roots) noexcept;

// The root of |scope|: roots.user, roots.machine, or nullptr for Scope::kNone.
HKEY RootOf(const RegistryRoots& roots, Scope scope) noexcept;

// Values larger than this are refused with HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE): nothing the
// engine reads comes close (the longest is a DLL path), and the cap keeps a planted value from
// costing more than a bounded buffer.
inline constexpr DWORD kMaxRegistryValueBytes = 64 * 1024;

// A value as stored: its type and its bytes.
struct RegistryValue {
  DWORD type = REG_NONE;
  std::vector<BYTE> data;
};

// Reads one value ("" names the default value). *value is std::nullopt when the key or the value
// does not exist; that is an answer, not a failure.
[[nodiscard]] HRESULT ReadRegistryValue(HKEY root, std::wstring_view subKey, std::wstring_view name,
                                        std::optional<RegistryValue>* value) noexcept;

// The text of a REG_SZ or REG_EXPAND_SZ value as stored (not expanded), up to the first null
// character; an odd trailing byte is ignored (pure). std::nullopt for any other type. May throw
// std::bad_alloc.
std::optional<std::wstring> RegistryText(const RegistryValue& value);

// A REG_DWORD of exactly 4 bytes (pure): what the handler accepts. std::nullopt for anything else,
// REG_DWORD_BIG_ENDIAN and a REG_DWORD of another size included.
std::optional<DWORD> RegistryDword(const RegistryValue& value) noexcept;

// A REG_QWORD of exactly 8 bytes (pure); std::nullopt for anything else.
std::optional<ULONGLONG> RegistryQword(const RegistryValue& value) noexcept;

// Expands %NAME% references with this process's environment.
[[nodiscard]] HRESULT ExpandEnvironment(std::wstring_view text, std::wstring* expanded) noexcept;

// A string value: REG_SZ as stored, REG_EXPAND_SZ expanded. std::nullopt when the key or the
// value does not exist. A value of another type fails with
// HRESULT_FROM_WIN32(ERROR_DATATYPE_MISMATCH) and leaves *value std::nullopt.
[[nodiscard]] HRESULT ReadRegistryString(HKEY root, std::wstring_view subKey,
                                         std::wstring_view name,
                                         std::optional<std::wstring>* value) noexcept;

// Whether the key exists.
[[nodiscard]] HRESULT RegistryKeyExists(HKEY root, std::wstring_view subKey, bool* exists) noexcept;

// Writes a REG_SZ, REG_DWORD or REG_QWORD value, creating the key and its parents as needed.
[[nodiscard]] HRESULT WriteRegistryString(HKEY root, std::wstring_view subKey,
                                          std::wstring_view name, std::wstring_view value) noexcept;
[[nodiscard]] HRESULT WriteRegistryDword(HKEY root, std::wstring_view subKey,
                                         std::wstring_view name, DWORD value) noexcept;
[[nodiscard]] HRESULT WriteRegistryQword(HKEY root, std::wstring_view subKey,
                                         std::wstring_view name, ULONGLONG value) noexcept;

// Deletes one value. A value or key that does not exist is not an error. |deleted| (optional)
// says whether there was something to delete.
[[nodiscard]] HRESULT DeleteRegistryValue(HKEY root, std::wstring_view subKey,
                                          std::wstring_view name, bool* deleted = nullptr) noexcept;

// Deletes the key with everything below it. A key that does not exist is not an error.
[[nodiscard]] HRESULT DeleteRegistryTree(HKEY root, std::wstring_view subKey,
                                         bool* deleted = nullptr) noexcept;

// Deletes the key if it has no subkeys and no values; a default value counts as a value. Never
// deletes anything below it: a key that gains a subkey meanwhile makes the deletion fail, and is
// then kept. A key that does not exist is not an error.
[[nodiscard]] HRESULT DeleteRegistryKeyIfEmpty(HKEY root, std::wstring_view subKey,
                                               bool* deleted = nullptr) noexcept;

}  // namespace pdft
