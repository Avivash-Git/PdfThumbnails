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

// What DeleteRegistryTree fails with when it stopped with part of the tree still there: a key in
// it is a registry link, which it never follows and never deletes, or the tree is deeper, holds
// more keys or takes longer than its RegistryTreeLimits allow. What it could delete is gone; the
// rest stays as it is. Callers report it as "left". A code of this app's own (FACILITY_ITF), not
// a Windows error, so that no call on the way can return it.
inline constexpr HRESULT kRegistryTreeLeftHr = MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x2F0);

// The bounds of one DeleteRegistryTree call. The trees it is for are the product's own, a few keys
// at most two levels deep; a tree far beyond that was made by someone else, and one call stops
// there (kRegistryTreeLeftHr) instead of spending however long deleting it would take.
struct RegistryTreeLimits {
  DWORD maxDepth = 8;   // levels below the key
  DWORD maxKeys = 256;  // keys opened, the key itself included
  DWORD maxMs = 2000;   // the walk stops once this much time has passed
};

// What one DeleteRegistryTree call did.
struct RegistryTreeDeletion {
  bool existed = false;       // there was a key at the path
  bool deleted = false;       // ... and it is gone now, with everything below it
  size_t keysDeleted = 0;     // the key itself included
  size_t linksLeft = 0;       // links in the tree (the key itself included), left as they are
  bool boundReached = false;  // it stopped at |limits|
};

// Deletes the key with everything below it. A key that does not exist is not an error.
//
// Never follows a registry link at the key or below it, and never looks a key up by its path
// twice: the path down to the key is looked up as given (in a loaded user hive Windows itself
// makes Software\Classes a link to the user's classes hive), and from there the key and every key
// below it are opened as they are (REG_OPTION_OPEN_LINK), each relative to its parent's handle,
// and deleted through their own handles (DeleteOpenRegistryKey) once their subkeys are gone. A
// link is left as it is, with every key above it, and counted; the call then fails with
// kRegistryTreeLeftHr, as it does past |limits|.
[[nodiscard]] HRESULT DeleteRegistryTree(HKEY root, std::wstring_view subKey,
                                         const RegistryTreeLimits& limits,
                                         RegistryTreeDeletion* deletion) noexcept;

// The same with the default limits. |deleted| (optional) says whether there was a key, now gone.
[[nodiscard]] HRESULT DeleteRegistryTree(HKEY root, std::wstring_view subKey,
                                         bool* deleted = nullptr) noexcept;

// Deletes the key if it has no subkeys and no values; a default value counts as a value. Never
// deletes anything below it: a key that gains a subkey meanwhile makes the deletion fail, and is
// then kept. A key that does not exist is not an error, nor is one with subkeys or values that
// this process may read but not delete; an empty one it may not delete is
// (HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)). The key is opened as it is (REG_OPTION_OPEN_LINK)
// and deleted through that handle: a link holds the name of its target in a value, so it is never
// empty, and it stays together with its target.
[[nodiscard]] HRESULT DeleteRegistryKeyIfEmpty(HKEY root, std::wstring_view subKey,
                                               bool* deleted = nullptr) noexcept;

// Deletes the key |key| is an open handle to (opened with DELETE), and never another one: not a
// key made at its path after it was opened and, for a link opened with REG_OPTION_OPEN_LINK, the
// link itself, never the key it names. Fails while the key has subkeys. A key deleted already
// (by someone else meanwhile) is not an error.
[[nodiscard]] HRESULT DeleteOpenRegistryKey(HKEY key) noexcept;

}  // namespace pdft
