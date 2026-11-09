// The executables' string tables hold text outside ASCII exactly as written: the resource
// scripts are UTF-8 and say so (#pragma code_page(65001)); without it the resource compiler
// reads them in the ANSI code page and the strings come out garbled. Loads the built executables
// as data files (nothing runs) from this test program's folder.

#include <windows.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

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

// The access keys in the settings dialog's template: "&X" in the text of each control.
std::vector<wchar_t> AccessKeys(HMODULE module) {
  const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(IDD_SETTINGS), RT_DIALOG);
  const auto* at = static_cast<const WORD*>(LockResource(LoadResource(module, found)));
  const auto skipText = [](const WORD* p) {  // none, an ordinal, or a string
    if (*p == 0x0000) return p + 1;
    if (*p == 0xFFFF) return p + 2;
    while (*p) ++p;
    return p + 1;
  };
  const auto alignDword = [](const WORD* p, const WORD* base) {
    return ((p - base) % 2) ? p + 1 : p;
  };
  const WORD* base = at;
  const WORD count = at[8];  // DLGTEMPLATEEX: dlgVer, signature, helpID, exStyle, style, cDlgItems
  at += 13;                  // ... cDlgItems, x, y, cx, cy
  at = skipText(at);         // menu
  at = skipText(at);         // class
  at = skipText(at);         // title
  at += 3;                   // font size, weight, italic and charset
  at = skipText(at);         // font name
  std::vector<wchar_t> keys;
  for (WORD item = 0; item < count; ++item) {
    at = alignDword(at, base);
    at += 12;           // helpID, exStyle, style, x, y, cx, cy, id: DLGITEMTEMPLATEEX
    at = skipText(at);  // class
    const std::wstring text = *at == 0xFFFF ? std::wstring() : reinterpret_cast<const wchar_t*>(at);
    at = skipText(at);
    const WORD extra = *at;
    at += 1 + (extra + 1) / 2;
    if (const size_t mark = text.find(L'&'); mark != std::wstring::npos && mark + 1 < text.size()) {
      keys.push_back(towupper(text[mark + 1]));
    }
  }
  return keys;
}

TEST(Resources, SettingsWindowAccessKeysAreUnique) {
  const std::wstring path = OwnFolder() + L"\\" + kSettingsExe;
  HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE);
  if (!module && test::IsApplicationControlBlock(HRESULT_FROM_WIN32(GetLastError()))) {
    GTEST_SKIP() << "Application Control blocked reading the settings executable";
  }
  ASSERT_NE(module, nullptr) << "error " << GetLastError();
  std::vector<wchar_t> keys = AccessKeys(module);
  FreeLibrary(module);
  std::sort(keys.begin(), keys.end());
  const std::vector<wchar_t> expected = {L'A', L'C', L'D', L'I', L'L', L'M',
                                         L'P', L'R', L'S', L'U', L'Z'};
  EXPECT_EQ(keys, expected);
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

}  // namespace
}  // namespace pdft
