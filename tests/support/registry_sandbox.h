// A private stand-in for HKEY_CURRENT_USER, so tests and tools can run code that reads or writes
// per-user settings (the handler's HKCU\Software\PdfThumbnails\Zoom) without ever touching the
// user's real ones.
//
// ScopedHkcuRedirect creates the empty key HKCU\Software\PdfThumbnails-Test\<new GUID> (the
// product's registry key plus "-Test") and maps HKEY_CURRENT_USER to it with
// RegOverridePredefKey. The mapping covers every thread of this process and nothing else; keys
// already open below the real HKCU stay where they are. The destructor restores the real HKCU,
// deletes the private key with everything in it, and then Software\PdfThumbnails-Test too if
// nothing else is left in it. A crash leaves the private key behind; it is never read again.
//
// One redirection at a time per process: the mapping is process-wide, and the destructor restores
// the real HKCU, not an outer redirection.

#pragma once

#include <windows.h>

#include <string>

namespace pdft::test {

class ScopedHkcuRedirect {
 public:
  ScopedHkcuRedirect();
  ScopedHkcuRedirect(const ScopedHkcuRedirect&) = delete;
  ScopedHkcuRedirect& operator=(const ScopedHkcuRedirect&) = delete;
  ~ScopedHkcuRedirect();

  // False when the key could not be created or HKCU not redirected; error() says why, and
  // nothing is left behind.
  bool ok() const { return key_ != nullptr; }
  HRESULT error() const { return error_; }
  // The private key, relative to the real HKCU: "Software\PdfThumbnails-Test\{...}".
  const std::wstring& path() const { return path_; }
  // The private key (what HKEY_CURRENT_USER now opens), with KEY_ALL_ACCESS. Tests write through
  // this handle rather than through HKEY_CURRENT_USER: even if the redirection were not in
  // effect, a write could then never reach the user's real keys.
  HKEY key() const { return key_; }

 private:
  std::wstring path_;
  HKEY key_ = nullptr;
  HRESULT error_ = S_OK;
};

}  // namespace pdft::test
