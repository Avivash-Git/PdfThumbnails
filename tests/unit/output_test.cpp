// Output (output.h): the decisions and encodings, the display filter, and writing to a file the way
// a redirected standard output is written.

#include <windows.h>

#include "output.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "json.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

TEST(Output, SettingsExecutableUsesInheritedHandlesWhenThereAreAny) {
  EXPECT_EQ(ChooseGuiOutput(FILE_TYPE_PIPE), GuiOutputSource::kInherited);
  EXPECT_EQ(ChooseGuiOutput(FILE_TYPE_DISK), GuiOutputSource::kInherited);
  EXPECT_EQ(ChooseGuiOutput(FILE_TYPE_CHAR), GuiOutputSource::kInherited);
  EXPECT_EQ(ChooseGuiOutput(FILE_TYPE_UNKNOWN), GuiOutputSource::kParentConsole);
}

TEST(Output, ColourOnlyOnAVirtualTerminalConsoleWithoutNoColor) {
  for (int bits = 0; bits < 8; ++bits) {
    const bool console = bits & 1;
    const bool virtualTerminal = bits & 2;
    const bool noColor = bits & 4;
    EXPECT_EQ(ShouldUseColor(console, virtualTerminal, noColor), bits == 3) << bits;
  }
}

TEST(Output, LinesAreUtf8WithCrLfAndNoByteOrderMark) {
  EXPECT_EQ(EncodeLine(L"caf\u00e9 \u2192 ok"), "caf\xc3\xa9 \xe2\x86\x92 ok\r\n");
  EXPECT_EQ(EncodeLine(L""), "\r\n");
  EXPECT_EQ(EncodeLine(L"one\ntwo"), "one\r\ntwo\r\n");
  EXPECT_EQ(EncodeLine(L"one\r\ntwo"), "one\r\ntwo\r\n");
  const wchar_t loneSurrogate[] = {L'a', static_cast<wchar_t>(0xD800), 0};
  EXPECT_EQ(EncodeLine(loneSurrogate), "a\xef\xbf\xbd\r\n");
}

TEST(Output, Red) { EXPECT_EQ(Red(L"no"), L"\x1b[31mno\x1b[0m"); }

std::wstring Char(unsigned code) { return std::wstring(1, static_cast<wchar_t>(code)); }

TEST(DisplayText, CleanTextStaysAsItIs) {
  for (const wchar_t* text :
       {L"", L"caf\u00e9 \u2192 ok", L"C:\\Program Files\\Product\\x.dll", L"a\tb",
        L"one\ntwo\r\nthree\n", L"\u00a0\u200e\u200f\u2028\u2029\u202f\u2065\u206a\ufeff",
        L"Result: healthy"}) {
    EXPECT_EQ(DisplayText(text), text) << ::testing::PrintToString(std::wstring(text));
  }
}

TEST(DisplayText, ControlCharactersAreShownAsTheReplacementCharacter) {
  const std::wstring replacement = Char(0xFFFD);
  struct Range {
    unsigned first;
    unsigned last;
  };
  // C0 but TAB and LF, DEL, C1, and the bidirectional embeddings, overrides and isolates.
  for (const Range& range : {Range{0x00, 0x08}, Range{0x0B, 0x1F}, Range{0x7F, 0x9F},
                             Range{0x202A, 0x202E}, Range{0x2066, 0x2069}}) {
    for (unsigned code = range.first; code <= range.last; ++code) {
      if (code == 0x0D) continue;  // "\r", below: it depends on what follows
      EXPECT_EQ(DisplayText(L"a" + Char(code) + L"b"), L"a" + replacement + L"b") << code;
    }
  }
  // A "\r" before "\n" is a line end; on its own it would go back over the line.
  EXPECT_EQ(DisplayText(L"a\r\nb"), L"a\r\nb");
  EXPECT_EQ(DisplayText(L"safe\rspoof"), L"safe" + replacement + L"spoof");
  EXPECT_EQ(DisplayText(L"a\r"), L"a" + replacement);
  EXPECT_EQ(DisplayText(L"a\r\r\n"), L"a" + replacement + L"\r\n");
  // An escape sequence, an operating system command (the window title) and a bell.
  EXPECT_EQ(DisplayText(L"\x1b[2J\x1b]0;title\x07!"),
            replacement + L"[2J" + replacement + L"]0;title" + replacement + L"!");
}

// The JSON writer escapes control characters itself, so JSON passes the filter unchanged.
TEST(DisplayText, JsonPassesUnchanged) {
  const std::string json = "{\"text\":" + JsonString(L"tab\there, line\r\nend \x1b[31m\x07") +
                           ",\"path\":" + JsonString(L"C:\\caf\u00e9\\\"q\".pdf") + "}";
  std::wstring wide;
  ASSERT_HRESULT_SUCCEEDED(Utf8ToWide(json, &wide));
  EXPECT_EQ(DisplayText(wide), wide);
}

TEST(Output, WritesToAFileAsUtf8WithoutColour) {
  test::TempDir dir(L"output");
  ASSERT_TRUE(dir.ok());
  const std::wstring path = dir.File(L"out.txt");
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  {
    Output out = Output::ForHandles(file, file);
    out.Line(L"h\u00e9llo");
    out.Error(L"oops\nsecond");
  }
  CloseHandle(file);
  std::ifstream in(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << in.rdbuf();
  EXPECT_EQ(bytes.str(), "h\xc3\xa9llo\r\noops\r\nsecond\r\n");
}

// Lines and errors go through the display filter when redirected too; JSON goes as given.
TEST(Output, LinesAndErrorsAreFilteredJsonIsNot) {
  test::TempDir dir(L"output");
  ASSERT_TRUE(dir.ok());
  const std::wstring path = dir.File(L"out.txt");
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  {
    Output out = Output::ForHandles(file, file);
    out.Line(L"a\x1b[31mb");
    out.Error(L"c" + Char(0x202E) + L"d\nnext");
    out.Json(L"{\"a\":\"" + Char(0x202E) + L"\x9b\"}");
  }
  CloseHandle(file);
  std::ifstream in(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << in.rdbuf();
  EXPECT_EQ(bytes.str(),
            "a\xef\xbf\xbd[31mb\r\n"
            "c\xef\xbf\xbd"
            "d\r\nnext\r\n"
            "{\"a\":\"\xe2\x80\xae\xc2\x9b\"}\r\n");
}

TEST(Output, NoHandlesWriteNothing) {
  Output out = Output::ForHandles(nullptr, nullptr);
  out.Line(L"nowhere");
  out.Error(L"nowhere");
}

}  // namespace
}  // namespace pdft
