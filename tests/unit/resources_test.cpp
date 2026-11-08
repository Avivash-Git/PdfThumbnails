// The executables' string tables hold text outside ASCII exactly as written: the resource
// scripts are UTF-8 and say so (#pragma code_page(65001)); without it the resource compiler
// reads them in the ANSI code page and the strings come out garbled. Loads the built executables
// as data files (nothing runs) from this test program's folder.

#include <windows.h>

#include <gtest/gtest.h>

#include <string>

#include "medium_il.h"
#include "project_config.h"
#include "resource_ids.h"

namespace pdft {
namespace {

std::wstring OwnFolder() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  std::wstring self(path, length);
  return self.substr(0, self.find_last_of(L'\\'));
}

std::wstring ResourceString(HMODULE module, UINT id) {
  const wchar_t* text = nullptr;
  const int length = LoadStringW(module, id, reinterpret_cast<LPWSTR>(&text), 0);
  return length > 0 && text ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
}

class Resources : public ::testing::TestWithParam<const wchar_t*> {};

TEST_P(Resources, NonAsciiStringsSurviveTheResourceCompiler) {
  const std::wstring path = OwnFolder() + L"\\" + GetParam();
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (!module && test::IsApplicationControlBlock(HRESULT_FROM_WIN32(GetLastError()))) {
    GTEST_SKIP() << "Application Control blocked reading " << ::testing::PrintToString(path);
  }
  ASSERT_NE(module, nullptr) << "error " << GetLastError();
  EXPECT_EQ(ResourceString(module, IDS_REBUILD_THUMBNAILS), L"Rebuild thumbnails\u2026");
  EXPECT_EQ(ResourceString(module, IDS_VERSION_CHANGE), L"0.14.0 \u2192 0.15.0");
  EXPECT_EQ(ResourceString(module, IDS_SEPARATOR), L"\u00b7");
  FreeLibrary(module);
}

INSTANTIATE_TEST_SUITE_P(Executables, Resources, ::testing::Values(kCliExe, kSettingsExe),
                         [](const ::testing::TestParamInfo<const wchar_t*>& info) {
                           return std::string(info.param == kCliExe ? "Cli" : "Settings");
                         });

TEST(Resources, SettingsCarriesThePreviewSamples) {
  const std::wstring path = OwnFolder() + L"\\" + kSettingsExe;
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (!module && test::IsApplicationControlBlock(HRESULT_FROM_WIN32(GetLastError()))) {
    GTEST_SKIP() << "Application Control blocked reading the settings executable";
  }
  ASSERT_NE(module, nullptr) << "error " << GetLastError();
  for (const UINT id : {IDR_SAMPLE_DOCUMENT, IDR_SAMPLE_SLIDE}) {
    const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    ASSERT_NE(found, nullptr) << id;
    EXPECT_GT(SizeofResource(module, found), 0u) << id;
  }
  FreeLibrary(module);
}

TEST(Resources, SettingsCarriesItsWindow) {
  const std::wstring path = OwnFolder() + L"\\" + kSettingsExe;
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (!module && test::IsApplicationControlBlock(HRESULT_FROM_WIN32(GetLastError()))) {
    GTEST_SKIP() << "Application Control blocked reading the settings executable";
  }
  ASSERT_NE(module, nullptr) << "error " << GetLastError();
  EXPECT_NE(FindResourceW(module, MAKEINTRESOURCEW(IDD_SETTINGS), RT_DIALOG), nullptr);
  EXPECT_NE(FindResourceW(module, MAKEINTRESOURCEW(IDI_APP), RT_GROUP_ICON), nullptr);
  EXPECT_NE(FindResourceW(module, MAKEINTRESOURCEW(IDA_SETTINGS), RT_ACCELERATOR), nullptr);
  EXPECT_FALSE(ResourceString(module, IDS_BANNER_OTHER_USER).empty());
  EXPECT_NE(ResourceString(module, IDS_READING_STATE).find(L"\u2026"), std::wstring::npos);
  FreeLibrary(module);
}

TEST(Resources, SettingsNoticeNamesTheCommandLineThroughAPlaceholder) {
  const std::wstring path = OwnFolder() + L"\\" + kSettingsExe;
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (!module && test::IsApplicationControlBlock(HRESULT_FROM_WIN32(GetLastError()))) {
    GTEST_SKIP() << "Application Control blocked reading the settings executable";
  }
  ASSERT_NE(module, nullptr) << "error " << GetLastError();
  EXPECT_FALSE(ResourceString(module, IDS_WINDOW_NOT_READY).empty());
  EXPECT_NE(ResourceString(module, IDS_WINDOW_USE_CLI).find(L"{cli} --help"), std::wstring::npos);
  FreeLibrary(module);
}

}  // namespace
}  // namespace pdft
