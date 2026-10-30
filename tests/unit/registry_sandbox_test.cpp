// ScopedHkcuRedirect, which the handler tests and thumbtest --zoom rely on: while it lasts,
// HKEY_CURRENT_USER is an empty private key on every thread, and afterwards the real HKCU is back
// and the private key is gone. Nothing here writes through HKEY_CURRENT_USER before checking
// that it is the private key.

#include <windows.h>

#include "registry_sandbox.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "project_config.h"

namespace pdft::test {
namespace {

bool KeyExists(HKEY root, const std::wstring& path) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  RegCloseKey(key);
  return true;
}

std::wstring TestRoot() { return std::wstring(pdft::kRegKey) + L"-Test"; }

TEST(RegistrySandbox, HkcuIsAnEmptyPrivateKey) {
  ASSERT_TRUE(KeyExists(HKEY_CURRENT_USER, L"Software")) << "every real HKCU has one";
  ScopedHkcuRedirect redirect;
  ASSERT_TRUE(redirect.ok()) << std::hex << redirect.error();
  const std::wstring prefix = TestRoot() + L"\\{";
  EXPECT_EQ(redirect.path().rfind(prefix, 0), 0u);
  EXPECT_EQ(redirect.path().size(), prefix.size() + 37);  // the rest of a {GUID}
  EXPECT_EQ(redirect.path().back(), L'}');

  EXPECT_FALSE(KeyExists(HKEY_CURRENT_USER, L"Software"));
  DWORD subkeys = 1, values = 1;
  ASSERT_EQ(RegQueryInfoKeyW(redirect.key(), nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr,
                             &values, nullptr, nullptr, nullptr, nullptr),
            ERROR_SUCCESS);
  EXPECT_EQ(subkeys, 0u);
  EXPECT_EQ(values, 0u);
}

TEST(RegistrySandbox, WritesThroughHkcuLandInThePrivateKey) {
  ScopedHkcuRedirect redirect;
  ASSERT_TRUE(redirect.ok()) << std::hex << redirect.error();
  ASSERT_FALSE(KeyExists(HKEY_CURRENT_USER, L"Software")) << "HKCU is not redirected";
  const DWORD written = 123;
  ASSERT_EQ(RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Probe", L"Value", REG_DWORD, &written,
                            sizeof(written)),
            ERROR_SUCCESS);
  DWORD read = 0, size = sizeof(read);
  ASSERT_EQ(RegGetValueW(redirect.key(), L"Software\\Probe", L"Value", RRF_RT_REG_DWORD, nullptr,
                         &read, &size),
            ERROR_SUCCESS);
  EXPECT_EQ(read, written);

  // The mapping is process-wide: another thread sees the same key.
  LSTATUS status = ERROR_GEN_FAILURE;
  read = 0;
  std::thread([&] {
    size = sizeof(read);
    status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Probe", L"Value", RRF_RT_REG_DWORD,
                          nullptr, &read, &size);
  }).join();
  EXPECT_EQ(status, ERROR_SUCCESS);
  EXPECT_EQ(read, written);
}

TEST(RegistrySandbox, RestoresHkcuAndDeletesTheKey) {
  const bool rootExisted = KeyExists(HKEY_CURRENT_USER, TestRoot());
  std::wstring path;
  {
    ScopedHkcuRedirect redirect;
    ASSERT_TRUE(redirect.ok()) << std::hex << redirect.error();
    path = redirect.path();
    const DWORD value = 1;
    ASSERT_EQ(RegSetKeyValueW(redirect.key(), L"Software\\Nested\\Deeper", L"Value", REG_DWORD,
                              &value, sizeof(value)),
              ERROR_SUCCESS);
  }
  EXPECT_TRUE(KeyExists(HKEY_CURRENT_USER, L"Software")) << "the real HKCU is back";
  EXPECT_FALSE(KeyExists(HKEY_CURRENT_USER, path));
  // Its parent goes too once empty. Another process (another preset's tests, thumbtest --zoom)
  // may have added a key of its own meanwhile; then the parent rightly stays.
  if (!rootExisted) {
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, TestRoot().c_str(), 0, KEY_QUERY_VALUE, &root) ==
        ERROR_SUCCESS) {
      DWORD subkeys = 0;
      RegQueryInfoKeyW(root, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, nullptr,
                       nullptr, nullptr, nullptr, nullptr);
      RegCloseKey(root);
      EXPECT_GT(subkeys, 0u) << "an empty parent was left behind";
    }
  }

  // A second redirection afterwards gets a key of its own.
  ScopedHkcuRedirect again;
  ASSERT_TRUE(again.ok()) << std::hex << again.error();
  EXPECT_NE(again.path(), path);
}

}  // namespace
}  // namespace pdft::test
