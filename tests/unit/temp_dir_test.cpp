#include <windows.h>

#include "temp_dir.h"

#include <gtest/gtest.h>

#include <string>

namespace pdft::test {
namespace {

bool Exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriteText(const std::wstring& path, const char* text) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(file, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
  CloseHandle(file);
  return ok != FALSE;
}

TEST(TempDir, CreatesAPrivateEmptyFolderUnderTemp) {
  TempDir dir(L"unit");
  ASSERT_TRUE(dir.ok());
  const DWORD attributes = GetFileAttributesW(dir.path().c_str());
  ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES);
  EXPECT_TRUE(attributes & FILE_ATTRIBUTE_DIRECTORY);
  wchar_t temp[MAX_PATH + 1];
  ASSERT_GT(GetTempPathW(MAX_PATH + 1, temp), 0u);
  EXPECT_EQ(dir.path().rfind(std::wstring(temp) + L"pdft-tests\\unit-", 0), 0u);
  EXPECT_EQ(dir.File(L"x.pdf"), dir.path() + L"\\x.pdf");

  WIN32_FIND_DATAW data;
  HANDLE find = FindFirstFileW((dir.path() + L"\\*").c_str(), &data);
  ASSERT_NE(find, INVALID_HANDLE_VALUE);
  int entries = 0;
  do {
    if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) ++entries;
  } while (FindNextFileW(find, &data));
  FindClose(find);
  EXPECT_EQ(entries, 0);
}

TEST(TempDir, EachInstanceGetsItsOwnFolder) {
  TempDir a(L"unit"), b(L"unit");
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  EXPECT_NE(a.path(), b.path());
}

TEST(TempDir, DeletesEverythingWhenDestroyed) {
  std::wstring path;
  {
    TempDir dir(L"unit");
    ASSERT_TRUE(dir.ok());
    path = dir.path();
    ASSERT_TRUE(CreateDirectoryW(dir.File(L"nested").c_str(), nullptr));
    ASSERT_TRUE(WriteText(dir.File(L"nested\\a.txt"), "a"));
    ASSERT_TRUE(WriteText(dir.File(L"δοκιμή-テスト.pdf"), "b"));
    ASSERT_TRUE(SetFileAttributesW(dir.File(L"nested\\a.txt").c_str(), FILE_ATTRIBUTE_READONLY));
  }
  EXPECT_FALSE(Exists(path));
}

TEST(TempDir, RemoveTreeOfAMissingPathSucceeds) {
  TempDir dir(L"unit");
  ASSERT_TRUE(dir.ok());
  EXPECT_TRUE(RemoveTree(dir.File(L"not-there")));
}

}  // namespace
}  // namespace pdft::test
