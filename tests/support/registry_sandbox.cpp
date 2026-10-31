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

// Creates the new, empty key HKCU\Software\PdfThumbnails-Test\<new GUID> with KEY_ALL_ACCESS.
HRESULT CreatePrivateKey(std::wstring* path, HKEY* key) {
  *key = nullptr;
  GUID guid = {};
  const HRESULT hr = CoCreateGuid(&guid);
  if (FAILED(hr)) return hr;
  wchar_t name[39] = {};
  if (StringFromGUID2(guid, name, ARRAYSIZE(name)) == 0) return E_UNEXPECTED;
  *path = TestRoot() + L"\\" + name;
  DWORD disposition = 0;
  // This creates the parent and the private key one after the other. Another process may delete
  // the still empty parent in between; the call then fails with ERROR_KEY_DELETED, and trying
  // again creates both.
  LSTATUS status = ERROR_KEY_DELETED;
  for (int attempt = 0; attempt < 3 && status == ERROR_KEY_DELETED; ++attempt) {
    status = RegCreateKeyExW(HKEY_CURRENT_USER, path->c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                             KEY_ALL_ACCESS, nullptr, key, &disposition);
  }
  if (status != ERROR_SUCCESS) {
    *key = nullptr;
    RemoveTestRootIfEmpty();
    return HRESULT_FROM_WIN32(status);
  }
  if (disposition != REG_CREATED_NEW_KEY) {  // a fresh GUID, so never expected: leave it alone
    RegCloseKey(*key);
    *key = nullptr;
    return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
  }
  return S_OK;
}

}  // namespace

ScopedHkcuRedirect::ScopedHkcuRedirect() {
  std::wstring path;
  HKEY key = nullptr;
  error_ = CreatePrivateKey(&path, &key);
  if (FAILED(error_)) return;
  // The system keeps its own reference to |key|, so closing it after this call would be safe;
  // it stays open for key().
  const LSTATUS status = RegOverridePredefKey(HKEY_CURRENT_USER, key);
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

ScopedRegistrySandbox::ScopedRegistrySandbox() {
  std::wstring path;
  HKEY key = nullptr;
  error_ = CreatePrivateKey(&path, &key);
  if (FAILED(error_)) return;
  HKEY user = nullptr;
  HKEY machine = nullptr;
  LSTATUS status = RegCreateKeyExW(key, L"user", 0, nullptr, REG_OPTION_NON_VOLATILE,
                                   KEY_ALL_ACCESS, nullptr, &user, nullptr);
  if (status == ERROR_SUCCESS) {
    status = RegCreateKeyExW(key, L"machine", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS,
                             nullptr, &machine, nullptr);
  }
  RegCloseKey(key);
  if (status != ERROR_SUCCESS) {
    error_ = HRESULT_FROM_WIN32(status);
    if (user) RegCloseKey(user);
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    RemoveTestRootIfEmpty();
    return;
  }
  path_ = path;
  user_ = user;
  machine_ = machine;
}

ScopedRegistrySandbox::~ScopedRegistrySandbox() {
  if (!user_ || !machine_) return;
  RegCloseKey(user_);
  RegCloseKey(machine_);
  user_ = machine_ = nullptr;
  RegDeleteTreeW(HKEY_CURRENT_USER, path_.c_str());
  RemoveTestRootIfEmpty();
}

}  // namespace pdft::test
