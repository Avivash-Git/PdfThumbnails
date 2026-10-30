#include "registry_sandbox.h"

#include <objbase.h>

#include "project_config.h"

namespace pdft::test {
namespace {

// HKCU\Software\PdfThumbnails-Test: the parent of every private key.
std::wstring TestRoot() { return std::wstring(pdft::kRegKey) + L"-Test"; }

// Deletes the parent once it holds no keys and no values: another redirection, in this or another
// process, may still be using it.
void RemoveTestRootIfEmpty() {
  const std::wstring root = TestRoot();
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, root.c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  DWORD subkeys = 1, values = 1;
  const LSTATUS status = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr,
                                          nullptr, &values, nullptr, nullptr, nullptr, nullptr);
  RegCloseKey(key);
  // RegDeleteKeyW never deletes subkeys, so a key that another process adds in the meantime
  // makes it fail instead of being lost. A key still being created can lose its parent here; the
  // constructor then tries again.
  if (status == ERROR_SUCCESS && subkeys == 0 && values == 0) {
    RegDeleteKeyW(HKEY_CURRENT_USER, root.c_str());
  }
}

}  // namespace

ScopedHkcuRedirect::ScopedHkcuRedirect() {
  GUID guid = {};
  error_ = CoCreateGuid(&guid);
  if (FAILED(error_)) return;
  wchar_t name[39] = {};
  if (StringFromGUID2(guid, name, ARRAYSIZE(name)) == 0) {
    error_ = E_UNEXPECTED;
    return;
  }
  const std::wstring path = TestRoot() + L"\\" + name;
  HKEY key = nullptr;
  DWORD disposition = 0;
  // This creates the parent and the private key one after the other. Another process may delete
  // the still empty parent in between; the call then fails with ERROR_KEY_DELETED, and trying
  // again creates both.
  LSTATUS status = ERROR_KEY_DELETED;
  for (int attempt = 0; attempt < 3 && status == ERROR_KEY_DELETED; ++attempt) {
    status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_ALL_ACCESS, nullptr, &key, &disposition);
  }
  if (status != ERROR_SUCCESS) {
    error_ = HRESULT_FROM_WIN32(status);
    RemoveTestRootIfEmpty();
    return;
  }
  if (disposition != REG_CREATED_NEW_KEY) {  // a fresh GUID, so never expected: leave it alone
    RegCloseKey(key);
    error_ = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    return;
  }
  // The system keeps its own reference to |key|, so closing it after this call would be safe;
  // it stays open for key().
  status = RegOverridePredefKey(HKEY_CURRENT_USER, key);
  if (status != ERROR_SUCCESS) {
    error_ = HRESULT_FROM_WIN32(status);
    RegCloseKey(key);
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    RemoveTestRootIfEmpty();
    return;
  }
  path_ = path;
  key_ = key;
}

ScopedHkcuRedirect::~ScopedHkcuRedirect() {
  if (!key_) return;
  const LSTATUS restored = RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
  RegCloseKey(key_);
  key_ = nullptr;
  // Deleting by path needs the real HKCU again; while still redirected the path would name a key
  // inside the private one.
  if (restored != ERROR_SUCCESS) return;
  RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
  RemoveTestRootIfEmpty();
}

}  // namespace pdft::test
