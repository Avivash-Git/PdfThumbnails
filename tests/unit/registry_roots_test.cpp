// The registry roots and the pure value decoders (registry_roots.h). No registry access.

#include <windows.h>

#include "registry_roots.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace pdft {
namespace {

RegistryValue Value(DWORD type, const void* data, size_t size) {
  RegistryValue value;
  value.type = type;
  value.data.resize(size);
  if (size) std::memcpy(value.data.data(), data, size);
  return value;
}

RegistryValue Text(DWORD type, const std::wstring& text, bool withNull = true) {
  return Value(type, text.c_str(), (text.size() + (withNull ? 1 : 0)) * sizeof(wchar_t));
}

TEST(RegistryRoots, SystemRootsAreTheDefault) {
  EXPECT_TRUE(UsesSystemRoots(RegistryRoots{}));
  const HKEY fake = reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0x1234));
  EXPECT_FALSE(UsesSystemRoots(RegistryRoots{fake, HKEY_LOCAL_MACHINE}));
  EXPECT_FALSE(UsesSystemRoots(RegistryRoots{HKEY_CURRENT_USER, fake}));
}

TEST(RegistryRoots, RootOfScope) {
  const HKEY user = reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0x10));
  const HKEY machine = reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0x20));
  const RegistryRoots roots{user, machine};
  EXPECT_EQ(RootOf(roots, Scope::kUser), user);
  EXPECT_EQ(RootOf(roots, Scope::kMachine), machine);
  EXPECT_EQ(RootOf(roots, Scope::kNone), nullptr);
}

TEST(RegistryValues, TextOfStrings) {
  EXPECT_EQ(RegistryText(Text(REG_SZ, L"C:\\x.dll")), L"C:\\x.dll");
  // Not expanded here: that is ReadRegistryString's job.
  EXPECT_EQ(RegistryText(Text(REG_EXPAND_SZ, L"%TEMP%\\x")), L"%TEMP%\\x");
  // Without its terminating null, and with an odd trailing byte.
  EXPECT_EQ(RegistryText(Text(REG_SZ, L"abc", false)), L"abc");
  RegistryValue odd = Text(REG_SZ, L"abc", false);
  odd.data.push_back(0x41);
  EXPECT_EQ(RegistryText(odd), L"abc");
  // Up to the first null: a REG_SZ with an embedded null is cut there.
  const wchar_t embedded[] = L"ab\0cd";
  EXPECT_EQ(RegistryText(Value(REG_SZ, embedded, sizeof(embedded))), L"ab");
  EXPECT_EQ(RegistryText(Value(REG_SZ, nullptr, 0)), L"");
}

TEST(RegistryValues, TextRefusesOtherTypes) {
  const DWORD number = 130;
  EXPECT_EQ(RegistryText(Value(REG_DWORD, &number, sizeof(number))), std::nullopt);
  EXPECT_EQ(RegistryText(Text(REG_MULTI_SZ, L"a")), std::nullopt);
  EXPECT_EQ(RegistryText(Text(REG_BINARY, L"a")), std::nullopt);
}

TEST(RegistryValues, DwordNeedsFourBytesOfRegDword) {
  const DWORD number = 130;
  EXPECT_EQ(RegistryDword(Value(REG_DWORD, &number, sizeof(number))), 130u);
  EXPECT_EQ(RegistryDword(Value(REG_DWORD, &number, 2)), std::nullopt);
  const ULONGLONG wide = 130;
  EXPECT_EQ(RegistryDword(Value(REG_DWORD, &wide, sizeof(wide))), std::nullopt);
  EXPECT_EQ(RegistryDword(Value(REG_DWORD_BIG_ENDIAN, &number, sizeof(number))), std::nullopt);
  EXPECT_EQ(RegistryDword(Value(REG_BINARY, &number, sizeof(number))), std::nullopt);
  EXPECT_EQ(RegistryDword(Text(REG_SZ, L"130")), std::nullopt);
}

TEST(RegistryValues, QwordNeedsEightBytesOfRegQword) {
  const ULONGLONG number = 0x01DC0123456789ABULL;
  EXPECT_EQ(RegistryQword(Value(REG_QWORD, &number, sizeof(number))), number);
  EXPECT_EQ(RegistryQword(Value(REG_QWORD, &number, 4)), std::nullopt);
  EXPECT_EQ(RegistryQword(Value(REG_BINARY, &number, sizeof(number))), std::nullopt);
}

}  // namespace
}  // namespace pdft
