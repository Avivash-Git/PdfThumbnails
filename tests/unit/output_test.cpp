// Output (output.h): the decisions and encodings, and writing to a file the way a redirected
// standard output is written.

#include <windows.h>

#include "output.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

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

TEST(Output, NoHandlesWriteNothing) {
  Output out = Output::ForHandles(nullptr, nullptr);
  out.Line(L"nowhere");
  out.Error(L"nowhere");
}

}  // namespace
}  // namespace pdft
