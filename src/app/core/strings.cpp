#include "strings.h"

#include <climits>
#include <cwchar>
#include <new>

namespace pdft {
namespace {

HRESULT LastError() noexcept {
  const DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

// WideCharToMultiByte into |out|; |flags| decides what happens to a lone surrogate.
HRESULT ToUtf8(std::wstring_view wide, DWORD flags, std::string* out) noexcept {
  out->clear();
  if (wide.empty()) return S_OK;
  if (wide.size() > INT_MAX) return E_INVALIDARG;
  const int length = static_cast<int>(wide.size());
  const int size =
      WideCharToMultiByte(CP_UTF8, flags, wide.data(), length, nullptr, 0, nullptr, nullptr);
  if (size <= 0) return LastError();
  try {
    out->resize(static_cast<size_t>(size));
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  }
  if (WideCharToMultiByte(CP_UTF8, flags, wide.data(), length, out->data(), size, nullptr,
                          nullptr) != size) {
    const HRESULT hr = LastError();
    out->clear();
    return hr;
  }
  return S_OK;
}

}  // namespace

HRESULT Utf8ToWide(std::string_view utf8, std::wstring* out) noexcept {
  out->clear();
  if (utf8.empty()) return S_OK;
  if (utf8.size() > INT_MAX) return E_INVALIDARG;
  const int length = static_cast<int>(utf8.size());
  const int size =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
  if (size <= 0) return LastError();
  try {
    out->resize(static_cast<size_t>(size));
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  }
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, out->data(), size) !=
      size) {
    const HRESULT hr = LastError();
    out->clear();
    return hr;
  }
  return S_OK;
}

HRESULT WideToUtf8(std::wstring_view wide, std::string* out) noexcept {
  return ToUtf8(wide, WC_ERR_INVALID_CHARS, out);
}

std::string WideToUtf8Lossy(std::wstring_view wide) noexcept {
  // Without WC_ERR_INVALID_CHARS, UTF-8 conversion replaces a lone surrogate with U+FFFD.
  std::string out;
  if (FAILED(ToUtf8(wide, 0, &out))) out.clear();
  return out;
}

bool IEquals(std::wstring_view a, std::wstring_view b) noexcept {
  if (a.size() != b.size()) return false;
  if (a.empty()) return true;
  if (a.size() > INT_MAX) return false;
  return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                              static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

bool IStartsWith(std::wstring_view text, std::wstring_view prefix) noexcept {
  return text.size() >= prefix.size() && IEquals(text.substr(0, prefix.size()), prefix);
}

std::wstring HResultText(HRESULT hr) {
  wchar_t text[16] = {};
  swprintf_s(text, L"0x%08lX", static_cast<unsigned long>(hr));
  return text;
}

std::wstring ToCrLf(std::wstring_view text) {
  std::wstring result;
  result.reserve(text.size() + text.size() / 32);
  for (wchar_t c : text) {
    if (c == L'\n') result += L'\r';
    result += c;
  }
  return result;
}

}  // namespace pdft
