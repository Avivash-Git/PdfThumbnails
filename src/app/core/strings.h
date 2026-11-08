// Text conversions and comparisons. Windows APIs take UTF-16; pipes, files and the log carry
// UTF-8.

#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace pdft {

// UTF-8 to UTF-16. Malformed input fails with HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION)
// instead of being patched up; *out is then empty.
[[nodiscard]] HRESULT Utf8ToWide(std::string_view utf8, std::wstring* out) noexcept;

// UTF-16 to UTF-8. A lone surrogate fails the same way.
[[nodiscard]] HRESULT WideToUtf8(std::wstring_view wide, std::string* out) noexcept;

// UTF-16 to UTF-8 for output and the log, where one odd character must not cost the whole
// line: a lone surrogate becomes U+FFFD. Empty only for empty input or when out of memory.
std::string WideToUtf8Lossy(std::wstring_view wide) noexcept;

// Equal ignoring case, compared code unit by code unit with the operating system's case table
// (CompareStringOrdinal): no locale, so "I" and "i" match everywhere. For paths, registry names
// and package names, which Windows compares the same way.
bool IEquals(std::wstring_view a, std::wstring_view b) noexcept;

// |text| starts with |prefix|, ignoring case like IEquals.
bool IStartsWith(std::wstring_view text, std::wstring_view prefix) noexcept;

// |text| with Windows line ends ("\n" becomes "\r\n"), for the clipboard and files. May throw
// std::bad_alloc.
std::wstring ToCrLf(std::wstring_view text);

// "0x80070005": how HRESULTs appear in messages and the log. May throw std::bad_alloc.
std::wstring HResultText(HRESULT hr);

}  // namespace pdft
