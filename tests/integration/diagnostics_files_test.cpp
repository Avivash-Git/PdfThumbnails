// The diagnostics' file side (diagnostics.h): reading the end of a log file and of the rotated one
// before it, in private folders under %TEMP%; and the details of this user and PC that the
// report masks, read for real (read-only).

#include <windows.h>

#include <gtest/gtest.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <fstream>
#include <string>
#include <vector>

#include "context.h"
#include "diagnostics.h"
#include "temp_dir.h"

namespace pdft {
namespace {

void WriteText(const std::wstring& file, const std::string& text) {
  std::ofstream out(file, std::ios::binary);
  out << text;
}

TEST(LogTail, LastLinesOfTheLog) {
  test::TempDir temp(L"logtail");
  ASSERT_TRUE(temp.ok());
  const std::wstring log = temp.File(L"app.log");
  std::string text;
  for (int i = 1; i <= 40; ++i) text += "line " + std::to_string(i) + "\r\n";
  WriteText(log, text);
  std::vector<std::wstring> lines;
  ASSERT_HRESULT_SUCCEEDED(ReadLogTailFrom(log, 30, &lines));
  ASSERT_EQ(lines.size(), 30u);
  EXPECT_EQ(lines.front(), L"line 11");
  EXPECT_EQ(lines.back(), L"line 40");
}

TEST(LogTail, TheRotatedFileFillsUpAShortLog) {
  test::TempDir temp(L"logtail");
  ASSERT_TRUE(temp.ok());
  const std::wstring log = temp.File(L"app.log");
  std::string older;
  for (int i = 1; i <= 50; ++i) older += "old " + std::to_string(i) + "\r\n";
  WriteText(log + L".1", older);
  WriteText(log, "new 1\r\nnew 2\r\n");
  std::vector<std::wstring> lines;
  ASSERT_HRESULT_SUCCEEDED(ReadLogTailFrom(log, 30, &lines));
  ASSERT_EQ(lines.size(), 30u);
  EXPECT_EQ(lines.front(), L"old 23");
  EXPECT_EQ(lines[27], L"old 50");
  EXPECT_EQ(lines.back(), L"new 2");
}

TEST(LogTail, ALongLogIsReadFromItsEndOnly) {
  test::TempDir temp(L"logtail");
  ASSERT_TRUE(temp.ok());
  const std::wstring log = temp.File(L"app.log");
  // About 300 KB of long lines, then non-ASCII text and a broken UTF-8 byte at the end.
  std::string text;
  for (int i = 0; i < 3000; ++i) text += std::string(99, 'x') + "\n";
  text += "caf\xC3\xA9\n\xFF tail\n";
  WriteText(log, text);
  std::vector<std::wstring> lines;
  ASSERT_HRESULT_SUCCEEDED(ReadLogTailFrom(log, 30, &lines));
  ASSERT_EQ(lines.size(), 30u);
  EXPECT_EQ(lines[28], std::wstring(L"caf") + wchar_t{0xE9});
  EXPECT_EQ(lines[29], std::wstring(1, wchar_t{0xFFFD}) + L" tail");
  EXPECT_EQ(lines[0], std::wstring(99, L'x')) << "the first, cut line is dropped";
}

TEST(LogTail, NoLogIsNoLines) {
  test::TempDir temp(L"logtail");
  ASSERT_TRUE(temp.ok());
  std::vector<std::wstring> lines = {L"stale"};
  ASSERT_HRESULT_SUCCEEDED(ReadLogTailFrom(temp.File(L"missing.log"), 30, &lines));
  EXPECT_TRUE(lines.empty());
  // A folder where the log should be cannot be read, and says so.
  EXPECT_HRESULT_FAILED(ReadLogTailFrom(temp.path(), 30, &lines));
}

TEST(Redaction, ThisUserAndPc) {
  ProcessContext context;
  ASSERT_HRESULT_SUCCEEDED(GetProcessContext(ContextProbe::kNone, &context));
  RedactContext redact;
  ASSERT_HRESULT_SUCCEEDED(GetRedactContext(context, &redact));
  wchar_t* profile = nullptr;
  ASSERT_HRESULT_SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile));
  const std::wstring profilePath = profile;
  CoTaskMemFree(profile);
  EXPECT_EQ(redact.profile, profilePath);
  EXPECT_FALSE(redact.computer.empty());
  EXPECT_FALSE(redact.hostName.empty());
  EXPECT_FALSE(redact.user.empty());
  const std::wstring masked =
      Redact(context.userAccount + L" " + context.userSid + L" " + profilePath + L"\\x", redact);
  EXPECT_EQ(masked.find(context.userSid), std::wstring::npos);
  EXPECT_EQ(masked.find(profilePath), std::wstring::npos);
  EXPECT_NE(masked.find(L"<user>"), std::wstring::npos);
  EXPECT_NE(masked.find(L"%USERPROFILE%\\x"), std::wstring::npos);
}

}  // namespace
}  // namespace pdft
