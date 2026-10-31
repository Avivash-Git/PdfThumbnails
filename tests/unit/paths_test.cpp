// Where things live and path comparisons (paths.h). Reads known-folder locations only; creates
// files only in a private temporary folder.

#include <windows.h>

#include "paths.h"

#include <gtest/gtest.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cwctype>
#include <filesystem>
#include <string>
#include <vector>

#include "project_config.h"
#include "temp_dir.h"

namespace pdft {
namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  std::wstring result;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path))) result = path;
  CoTaskMemFree(path);
  return result;
}

bool EndsWith(const std::wstring& text, const std::wstring& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::wstring Upper(std::wstring text) {
  for (wchar_t& c : text) c = static_cast<wchar_t>(std::towupper(c));
  return text;
}

bool MakeFolder(const std::wstring& path) { return CreateDirectoryW(path.c_str(), nullptr) != 0; }

bool MakeFile(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  CloseHandle(file);
  return true;
}

using test::MakeJunction;

TEST(Paths, IsUnderLexically) {
  struct Row {
    const wchar_t* child;
    const wchar_t* parent;
    bool under;
  };
  const Row rows[] = {
      {L"C:\\a\\b\\c", L"C:\\a\\b", true},
      {L"C:\\a\\b\\c\\d.dll", L"C:\\a\\b", true},
      {L"C:\\a\\bc", L"C:\\a\\b", false},  // a longer name, not a child
      {L"C:\\a\\b", L"C:\\a\\bc", false},
      {L"C:\\A\\B\\c", L"c:\\a\\b", true},  // case
      {L"C:\\a\\b", L"C:\\a\\b", false},    // not inside itself
      {L"C:\\a\\b\\", L"C:\\a\\b", false},
      {L"C:\\a\\b\\c", L"C:\\a\\b\\", true},
      {L"C:\\a\\b\\..\\bc", L"C:\\a\\b", false},
      {L"C:\\a\\b\\..\\b\\c", L"C:\\a\\b", true},
      {L"C:\\a\\b\\.\\c", L"C:\\a\\b", true},
      {L"C:\\a\\b\\c\\..", L"C:\\a\\b", false},
      {L"C:\\a/b/c", L"C:\\a\\b", true},  // either separator
      {L"D:\\a\\b\\c", L"C:\\a\\b", false},
      {L"C:\\x", L"C:\\", true},
      {L"C:\\", L"C:\\", false},
      {L"a\\b\\c", L"a\\b", false},      // relative paths never count
      {L"C:a\\b", L"C:a", false},        // nor drive-relative ones
      {L"\\a\\b\\c", L"\\a\\b", false},  // nor ones relative to the current drive
      {L"\\\\?\\C:\\a\\b\\c", L"C:\\a\\b", true},
      {L"C:\\a\\b\\c", L"\\\\?\\C:\\a\\b", true},
      {L"\\\\server\\share\\x", L"\\\\server\\share", true},
      {L"\\\\?\\UNC\\server\\share\\x", L"\\\\SERVER\\share", true},
      {L"\\\\server\\other\\x", L"\\\\server\\share", false},
  };
  for (const Row& row : rows) {
    EXPECT_EQ(IsUnderLexically(row.child, row.parent), row.under)
        << ::testing::PrintToString(std::wstring(row.child)) << " under "
        << ::testing::PrintToString(std::wstring(row.parent));
  }
}

TEST(Paths, IsUnderFollowsTheFileSystem) {
  test::TempDir dir(L"paths");
  ASSERT_TRUE(dir.ok());
  for (const wchar_t* folder :
       {L"a", L"a\\b", L"a\\bc", L"a\\b\\x", L"inside", L"inside\\link", L"outside"}) {
    ASSERT_TRUE(MakeFolder(dir.File(folder))) << GetLastError();
  }
  ASSERT_TRUE(MakeFile(dir.File(L"outside\\file.txt")));
  const std::wstring b = dir.File(L"a\\b");
  EXPECT_TRUE(IsUnder(dir.File(L"a\\b\\x"), b));
  EXPECT_TRUE(IsUnder(Upper(dir.File(L"a\\b\\x")), b));
  EXPECT_FALSE(IsUnder(dir.File(L"a\\bc"), b));
  EXPECT_FALSE(IsUnder(dir.File(L"a\\b\\..\\bc"), b));
  EXPECT_FALSE(IsUnder(b, b));
  EXPECT_FALSE(IsUnder(dir.File(L"a\\b\\missing"), b));  // cannot be canonicalized
  EXPECT_FALSE(IsUnder(dir.File(L"a\\b\\x"), dir.File(L"missing")));

  // A junction inside one folder that leads to another: by its name it is inside, by where it
  // leads it is not.
  ASSERT_TRUE(MakeJunction(dir.File(L"inside\\link"), dir.File(L"outside"))) << GetLastError();
  const std::wstring throughLink = dir.File(L"inside\\link\\file.txt");
  EXPECT_TRUE(IsUnderLexically(throughLink, dir.File(L"inside")));
  EXPECT_FALSE(IsUnder(throughLink, dir.File(L"inside")));
  EXPECT_TRUE(IsUnder(throughLink, dir.File(L"outside")));
}

TEST(Paths, CanonicalizeGivesThePlainFinalPath) {
  test::TempDir dir(L"paths");
  ASSERT_TRUE(dir.ok());
  ASSERT_TRUE(MakeFile(dir.File(L"File.TXT")));
  std::filesystem::path canonical;
  ASSERT_HRESULT_SUCCEEDED(Canonicalize(Upper(dir.File(L"file.txt")), &canonical));
  EXPECT_FALSE(canonical.native().starts_with(L"\\\\?\\")) << canonical;
  EXPECT_TRUE(EndsWith(canonical.native(), L"\\File.TXT")) << canonical;  // the case on disk
  std::filesystem::path again;
  ASSERT_HRESULT_SUCCEEDED(Canonicalize(canonical, &again));
  EXPECT_EQ(again, canonical);
  // Through the long-path prefix too.
  ASSERT_HRESULT_SUCCEEDED(Canonicalize(L"\\\\?\\" + canonical.native(), &again));
  EXPECT_EQ(again, canonical);
  EXPECT_EQ(Canonicalize(dir.File(L"missing.txt"), &again),
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
}

TEST(Paths, KnownLocations) {
  const std::wstring product = kProductName;
  std::filesystem::path path;
  ASSERT_HRESULT_SUCCEEDED(GetMachineRoot(&path));
  EXPECT_EQ(path.native(), KnownFolder(FOLDERID_ProgramFiles) + L"\\" + product);
  ASSERT_HRESULT_SUCCEEDED(GetUserRoot(&path));
  EXPECT_EQ(path.native(), KnownFolder(FOLDERID_UserProgramFiles) + L"\\" + product);
  ASSERT_HRESULT_SUCCEEDED(GetDataDir(&path));
  EXPECT_EQ(path.native(), KnownFolder(FOLDERID_LocalAppData) + L"\\" + product);
  ASSERT_HRESULT_SUCCEEDED(GetLogPath(&path));
  EXPECT_EQ(path.native(),
            KnownFolder(FOLDERID_LocalAppData) + L"\\" + product + L"\\Logs\\" + product + L".log");
}

TEST(Paths, OwnExe) {
  wchar_t expected[MAX_PATH] = {};
  ASSERT_GT(GetModuleFileNameW(nullptr, expected, MAX_PATH), 0u);
  std::filesystem::path exe;
  ASSERT_HRESULT_SUCCEEDED(GetOwnExe(&exe));
  EXPECT_EQ(exe.native(), expected);
}

}  // namespace
}  // namespace pdft
