// The log (log.h): the line format, rotation and concurrent appends. Every test points the log
// at a file in a private temporary folder (SetLogFileForTesting); the real log is never opened.

#include <windows.h>

#include "log.h"

#include <gtest/gtest.h>
#include <wil/result_macros.h>

#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.h"

namespace pdft {
namespace {

// "2026-10-30T01:26:04.512+02:00 4812 component LEVEL message"
const std::regex kLine(
    R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}[+-]\d{2}:\d{2} \d+ [^ ]+ (INFO|WARN|ERROR) .*$)");

std::string ReadFile(const std::wstring& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

// The lines of |text|, which must each end in CR LF.
std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find("\r\n", start);
    if (end == std::string::npos) {
      lines.push_back(text.substr(start) + "<no line end>");
      break;
    }
    lines.push_back(text.substr(start, end - start));
    start = end + 2;
  }
  return lines;
}

// Points the log at a file for one test and back to "off" afterwards.
class LogFile {
 public:
  LogFile(const std::wstring& path, ULONGLONG rotateBytes) {
    SetLogFileForTesting(path, rotateBytes);
  }
  LogFile(const LogFile&) = delete;
  LogFile& operator=(const LogFile&) = delete;
  ~LogFile() { SetLogFileForTesting({}, 0); }
};

SYSTEMTIME Time(WORD year, WORD month, WORD day, WORD hour, WORD minute, WORD second,
                WORD milliseconds) {
  SYSTEMTIME time = {};
  time.wYear = year;
  time.wMonth = month;
  time.wDay = day;
  time.wHour = hour;
  time.wMinute = minute;
  time.wSecond = second;
  time.wMilliseconds = milliseconds;
  return time;
}

TEST(Log, LineFormat) {
  const SYSTEMTIME time = Time(2026, 10, 30, 1, 26, 4, 512);
  EXPECT_EQ(FormatLogLine(time, 120, 4812, L"check", LogLevel::kInfo, L"trigger=task ms=1210"),
            L"2026-10-30T01:26:04.512+02:00 4812 check INFO trigger=task ms=1210\r\n");
  EXPECT_EQ(FormatLogLine(Time(2026, 1, 2, 3, 4, 5, 6), -330, 7, L"zoom", LogLevel::kWarning, L"x"),
            L"2026-01-02T03:04:05.006-05:30 7 zoom WARN x\r\n");
  EXPECT_EQ(FormatLogLine(time, 0, 1, L"wil", LogLevel::kError, L""),
            L"2026-10-30T01:26:04.512+00:00 1 wil ERROR \r\n");
}

TEST(Log, OneEntryIsAlwaysOneLine) {
  const SYSTEMTIME time = Time(2026, 10, 30, 1, 26, 4, 512);
  EXPECT_EQ(FormatLogLine(time, 60, 1, L"a b\nc", LogLevel::kInfo, L"one\r\ntwo\tthree\u2028four"),
            L"2026-10-30T01:26:04.512+01:00 1 a-b-c INFO one  two three four\r\n");
  EXPECT_EQ(FormatLogLine(time, 60, 1, L"", LogLevel::kInfo, L"m"),
            L"2026-10-30T01:26:04.512+01:00 1 app INFO m\r\n");
}

TEST(Log, UtcOffset) {
  const auto at = [](long long ticks) {
    FILETIME time;
    time.dwLowDateTime = static_cast<DWORD>(ticks);
    time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return time;
  };
  constexpr long long kMinute = 600'000'000;
  const long long base = 133'000'000'000'000'000;
  EXPECT_EQ(UtcOffsetMinutes(at(base), at(base)), 0);
  EXPECT_EQ(UtcOffsetMinutes(at(base), at(base + 120 * kMinute)), 120);
  EXPECT_EQ(UtcOffsetMinutes(at(base), at(base - 330 * kMinute)), -330);
  EXPECT_EQ(UtcOffsetMinutes(at(base), at(base + 345 * kMinute + 1)), 345);  // rounds
}

TEST(Log, WritesUtf8LinesToTheFile) {
  test::TempDir dir(L"log");
  ASSERT_TRUE(dir.ok());
  const std::wstring path = dir.File(L"test.log");
  {
    LogFile file(path, kLogRotateBytes);
    Log(LogLevel::kInfo, L"zoom", L"start");
    Log(LogLevel::kError, L"zoom", L"caf\u00e9 end exit=5");
  }
  Log(LogLevel::kInfo, L"zoom", L"after the test: goes nowhere");
  const std::vector<std::string> lines = Lines(ReadFile(path));
  ASSERT_EQ(lines.size(), 2u);
  for (const std::string& line : lines) EXPECT_TRUE(std::regex_match(line, kLine)) << line;
  const std::string pid = " " + std::to_string(GetCurrentProcessId()) + " ";
  EXPECT_NE(lines[0].find(pid + "zoom INFO start"), std::string::npos) << lines[0];
  EXPECT_NE(lines[1].find(pid + "zoom ERROR caf\xc3\xa9 end exit=5"), std::string::npos)
      << lines[1];
}

TEST(Log, RotatesToDotOne) {
  test::TempDir dir(L"log");
  ASSERT_TRUE(dir.ok());
  const std::wstring path = dir.File(L"test.log");
  const std::wstring rotated = path + L".1";
  constexpr ULONGLONG kLimit = 1000;
  LogFile file(path, kLimit);
  // About 80 bytes a line: 12 lines fill a file.
  const auto write = [](int from, int to) {
    for (int i = from; i < to; ++i) {
      Log(LogLevel::kInfo, L"rotate", L"line " + std::to_wstring(1000 + i) + L" padding padding");
    }
  };
  write(0, 20);
  std::string current = ReadFile(path);
  std::string previous = ReadFile(rotated);
  EXPECT_LE(current.size(), kLimit);
  ASSERT_FALSE(previous.empty()) << "no rotation after 20 lines";
  EXPECT_LE(previous.size(), kLimit);
  EXPECT_NE(previous.find("line 1000 "), std::string::npos);  // the oldest line went first
  EXPECT_EQ(current.find("line 1000 "), std::string::npos);
  EXPECT_NE(current.find("line 1019 "), std::string::npos);

  // More lines rotate again: .log.1 is replaced, not appended to.
  write(20, 45);
  current = ReadFile(path);
  previous = ReadFile(rotated);
  EXPECT_LE(current.size(), kLimit);
  EXPECT_LE(previous.size(), kLimit);
  EXPECT_EQ(previous.find("line 1000 "), std::string::npos);
  EXPECT_NE(current.find("line 1044 "), std::string::npos);
  for (const std::string& line : Lines(previous + current)) {
    EXPECT_TRUE(std::regex_match(line, kLine)) << line;
  }
}

TEST(Log, TwoThreadsAppendWholeLines) {
  test::TempDir dir(L"log");
  ASSERT_TRUE(dir.ok());
  const std::wstring path = dir.File(L"test.log");
  constexpr int kPerThread = 300;
  {
    LogFile file(path, kLogRotateBytes * 16);
    const auto writer = [](wchar_t name) {
      for (int i = 0; i < kPerThread; ++i) {
        Log(LogLevel::kInfo, std::wstring(1, name), L"entry " + std::to_wstring(i) + L" end");
      }
    };
    std::thread first(writer, L'a');
    std::thread second(writer, L'b');
    first.join();
    second.join();
  }
  const std::vector<std::string> lines = Lines(ReadFile(path));
  ASSERT_EQ(lines.size(), 2u * kPerThread);
  std::vector<int> next = {0, 0};  // each thread's lines arrive in its own order
  for (const std::string& line : lines) {
    ASSERT_TRUE(std::regex_match(line, kLine)) << line;
    const size_t at = line.find(" INFO entry ");
    ASSERT_NE(at, std::string::npos) << line;
    const int thread = line[at - 1] == 'a' ? 0 : 1;
    EXPECT_EQ(line.substr(at + 12), std::to_string(next[thread]++) + " end") << line;
  }
  EXPECT_EQ(next[0], kPerThread);
  EXPECT_EQ(next[1], kPerThread);
}

TEST(Log, TwoThreadsWhileRotating) {
  test::TempDir dir(L"log");
  ASSERT_TRUE(dir.ok());
  const std::wstring path = dir.File(L"test.log");
  {
    LogFile file(path, 2000);
    const auto writer = [](wchar_t name) {
      for (int i = 0; i < 200; ++i) {
        Log(LogLevel::kInfo, std::wstring(1, name), L"entry " + std::to_wstring(i));
      }
    };
    std::thread first(writer, L'a');
    std::thread second(writer, L'b');
    first.join();
    second.join();
  }
  const std::string current = ReadFile(path);
  const std::string previous = ReadFile(path + L".1");
  EXPECT_LE(current.size(), 2000u);
  EXPECT_LE(previous.size(), 2000u);
  EXPECT_FALSE(previous.empty());
  for (const std::string& line : Lines(previous + current)) {
    EXPECT_TRUE(std::regex_match(line, kLine)) << line;
  }
}

TEST(Log, WilFailuresGoToTheLog) {
  test::TempDir dir(L"log");
  ASSERT_TRUE(dir.ok());
  const std::wstring path = dir.File(L"test.log");
  {
    LogFile file(path, kLogRotateBytes);
    LogWilFailures();
    LOG_HR(E_ACCESSDENIED);
    wil::SetResultLoggingCallback(nullptr);
  }
  const std::vector<std::string> lines = Lines(ReadFile(path));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(std::regex_match(lines[0], kLine)) << lines[0];
  EXPECT_NE(lines[0].find(" wil WARN hr=0x80070005 line="), std::string::npos) << lines[0];
  EXPECT_NE(lines[0].find(" at=+0x"), std::string::npos) << lines[0];
}

}  // namespace
}  // namespace pdft
