// Text conversions and comparisons (strings.h), and the exit-code table (exit_code.h).

#include <windows.h>

#include "strings.h"

#include <gtest/gtest.h>

#include <string>

#include "exit_code.h"

namespace pdft {
namespace {

// Greek and Japanese ("δοκιμή-テスト"): 2- and 3-byte UTF-8 sequences.
constexpr wchar_t kWide[] = L"\u03b4\u03bf\u03ba\u03b9\u03bc\u03ae-\u30c6\u30b9\u30c8";
constexpr char kUtf8[] =
    "\xce\xb4\xce\xbf\xce\xba\xce\xb9\xce\xbc\xce\xae-\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88";

TEST(Strings, Utf8RoundTrip) {
  std::wstring wide;
  ASSERT_HRESULT_SUCCEEDED(Utf8ToWide(kUtf8, &wide));
  EXPECT_EQ(wide, kWide);
  std::string utf8;
  ASSERT_HRESULT_SUCCEEDED(WideToUtf8(kWide, &utf8));
  EXPECT_EQ(utf8, kUtf8);
  // A character outside the BMP: a surrogate pair, 4 bytes of UTF-8.
  ASSERT_HRESULT_SUCCEEDED(WideToUtf8(L"\U0001F600", &utf8));
  EXPECT_EQ(utf8, "\xf0\x9f\x98\x80");
}

TEST(Strings, EmptyConvertsToEmpty) {
  std::wstring wide = L"x";
  ASSERT_HRESULT_SUCCEEDED(Utf8ToWide("", &wide));
  EXPECT_TRUE(wide.empty());
  std::string utf8 = "x";
  ASSERT_HRESULT_SUCCEEDED(WideToUtf8(L"", &utf8));
  EXPECT_TRUE(utf8.empty());
}

TEST(Strings, InvalidInputFails) {
  std::wstring wide = L"x";
  EXPECT_EQ(Utf8ToWide("a\xc3(", &wide), HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION));
  EXPECT_TRUE(wide.empty());
  std::string utf8 = "x";
  const wchar_t loneSurrogate[] = {L'a', static_cast<wchar_t>(0xD800), L'b', 0};
  EXPECT_EQ(WideToUtf8(loneSurrogate, &utf8), HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION));
  EXPECT_TRUE(utf8.empty());
}

TEST(Strings, LossyReplacesLoneSurrogates) {
  const wchar_t loneSurrogate[] = {L'a', static_cast<wchar_t>(0xDC00), L'b', 0};
  EXPECT_EQ(WideToUtf8Lossy(loneSurrogate),
            "a\xef\xbf\xbd"
            "b");
  EXPECT_EQ(WideToUtf8Lossy(kWide), kUtf8);
  EXPECT_EQ(WideToUtf8Lossy(L""), "");
}

TEST(Strings, IEqualsIgnoresCaseOrdinally) {
  EXPECT_TRUE(IEquals(L"PdfThumbnails", L"PDFTHUMBNAILS"));
  EXPECT_TRUE(IEquals(L"C:\\Program Files", L"c:\\program files"));
  EXPECT_TRUE(IEquals(L"\u0394\u03a3", L"\u03b4\u03c3"));  // Greek capital and small letters
  EXPECT_TRUE(IEquals(L"", L""));
  EXPECT_FALSE(IEquals(L"abc", L"abd"));
  EXPECT_FALSE(IEquals(L"abc", L"abcd"));
  EXPECT_FALSE(IEquals(L"a\\b", L"a/b"));
}

TEST(Strings, IStartsWith) {
  EXPECT_TRUE(IStartsWith(L"\\\\?\\UNC\\server", L"\\\\?\\unc\\"));
  EXPECT_TRUE(IStartsWith(L"abc", L""));
  EXPECT_FALSE(IStartsWith(L"ab", L"abc"));
  EXPECT_FALSE(IStartsWith(L"xabc", L"abc"));
}

TEST(Strings, HResultText) {
  EXPECT_EQ(HResultText(E_ACCESSDENIED), L"0x80070005");
  EXPECT_EQ(HResultText(S_OK), L"0x00000000");
  EXPECT_EQ(HResultText(E_NOTIMPL), L"0x80004001");
}

TEST(ExitCodes, ValuesAreTheContract) {
  EXPECT_EQ(static_cast<int>(ExitCode::kOk), 0);
  EXPECT_EQ(static_cast<int>(ExitCode::kProblems), 1);
  EXPECT_EQ(static_cast<int>(ExitCode::kUsage), 2);
  EXPECT_EQ(static_cast<int>(ExitCode::kCancelled), 3);
  EXPECT_EQ(static_cast<int>(ExitCode::kBusy), 4);
  EXPECT_EQ(static_cast<int>(ExitCode::kFailed), 5);
  EXPECT_EQ(static_cast<int>(ExitCode::kNotInstalled), 6);
  EXPECT_EQ(static_cast<int>(ExitCode::kNeedsAdmin), 7);
  EXPECT_EQ(static_cast<int>(ExitCode::kWrongContext), 8);
}

TEST(ExitCodes, TableHasEveryCodeOnce) {
  ASSERT_EQ(std::size(kExitCodes), 9u);
  for (size_t i = 0; i < std::size(kExitCodes); ++i) {
    EXPECT_EQ(static_cast<size_t>(kExitCodes[i].code), i);
    ASSERT_NE(kExitCodes[i].meaning, nullptr);
    EXPECT_GT(std::wstring(kExitCodes[i].meaning).size(), 1u);
  }
}

}  // namespace
}  // namespace pdft
