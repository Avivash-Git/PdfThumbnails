// pdft-run-outside's pure parts (tools/run-outside/run_outside_core.h): its command line, run ids,
// the request file the child reads, the program's command line, the result record, and how a
// run's outcome becomes the exit code.

#include <windows.h>

#include <gtest/gtest.h>
#include <shellapi.h>

#include <string>
#include <string_view>
#include <vector>

#include "run_outside_core.h"

namespace pdft::outside {
namespace {

bool Parse(std::vector<std::wstring_view> args, Options* options, std::wstring* error) {
  return ParseOptions(args, options, error);
}

// What CommandLineToArgvW makes of |line|, which is how the C runtime of most programs splits it.
std::vector<std::wstring> Split(const std::wstring& line) {
  int count = 0;
  LPWSTR* argv = CommandLineToArgvW(line.c_str(), &count);
  std::vector<std::wstring> out;
  for (int i = 0; argv && i < count; ++i) out.emplace_back(argv[i]);
  LocalFree(argv);
  return out;
}

const std::vector<std::wstring>& AwkwardArguments() {
  static const std::vector<std::wstring> arguments = {
      L"",
      L"plain",
      L"with space",
      L"tab\there",
      L"\"quoted\"",
      L"in\"side",
      L"trailing\\",
      L"two trailing\\\\",
      L"\\\"backslash then quote",
      L"C:\\Program Files\\x\\",
      L"--gtest_filter=Context*:ComSecurity*",
      L"%TEMP% & | ^ ! < > ( )",
      // Greek and Japanese ("test"), and a character outside the BMP (a surrogate pair).
      L"\u03b4\u03bf\u03ba\u03b9\u03bc\u03ae-\u30c6\u30b9\u30c8 \U0001F600",
      L"   ",
  };
  return arguments;
}

TEST(RunOutsideOptions, RunWithAndWithoutTimeout) {
  Options options;
  std::wstring error;
  ASSERT_TRUE(Parse({L"--", L"C:\\x.exe", L"--version", L""}, &options, &error)) << error;
  EXPECT_EQ(options.mode, Mode::kRun);
  EXPECT_EQ(options.timeoutSeconds, kDefaultTimeoutSeconds);
  EXPECT_EQ(options.program, L"C:\\x.exe");
  EXPECT_EQ(options.arguments, (std::vector<std::wstring>{L"--version", L""}));

  ASSERT_TRUE(Parse({L"--timeout", L"300", L"--", L"x.exe", L"--", L"--timeout"}, &options, &error))
      << error;
  EXPECT_EQ(options.timeoutSeconds, 300u);
  EXPECT_EQ(options.program, L"x.exe");
  // Everything after the program belongs to it, "--" included.
  EXPECT_EQ(options.arguments, (std::vector<std::wstring>{L"--", L"--timeout"}));

  ASSERT_TRUE(Parse({L"--timeout", L"86400", L"--", L"x.exe"}, &options, &error)) << error;
  EXPECT_EQ(options.timeoutSeconds, kMaxTimeoutSeconds);
  ASSERT_TRUE(Parse({L"--timeout", L"0001", L"--", L"x.exe"}, &options, &error)) << error;
  EXPECT_EQ(options.timeoutSeconds, 1u);
}

TEST(RunOutsideOptions, HelpAndChild) {
  Options options;
  std::wstring error;
  for (const wchar_t* help : {L"--help", L"-h", L"-?", L"/?"}) {
    ASSERT_TRUE(Parse({help}, &options, &error)) << help;
    EXPECT_EQ(options.mode, Mode::kHelp);
  }
  ASSERT_TRUE(Parse({L"--child", L"0C3534C4-5B7A-48AB-828D-1CD660DC447F"}, &options, &error));
  EXPECT_EQ(options.mode, Mode::kChild);
  EXPECT_EQ(options.runId, L"0C3534C4-5B7A-48AB-828D-1CD660DC447F");
}

TEST(RunOutsideOptions, UsageErrors) {
  const std::vector<std::vector<std::wstring_view>> bad = {
      {},
      {L"x.exe"},      // no "--"
      {L"--"},         // no program
      {L"--", L""},    // empty program
      {L"--timeout"},  // no value
      {L"--timeout", L"--", L"x.exe"},
      {L"--timeout", L"0", L"--", L"x.exe"},
      {L"--timeout", L"86401", L"--", L"x.exe"},
      {L"--timeout", L"-5", L"--", L"x.exe"},
      {L"--timeout", L"1e3", L"--", L"x.exe"},
      {L"--timeout", L"\uFF11\uFF12", L"--", L"x.exe"},  // fullwidth digits are not digits here
      {L"--timeout", L"18446744073709551617", L"--", L"x.exe"},  // must not wrap around
      {L"--timeout", L"5", L"--timeout", L"5", L"--", L"x.exe"},
      {L"--timeout=5", L"--", L"x.exe"},
      {L"--bogus", L"--", L"x.exe"},
      {L"--help", L"x"},
      {L"--child"},
      {L"--child", L"{0C3534C4-5B7A-48AB-828D-1CD660DC447F}"},
      {L"--child", L"0C3534C4-5B7A-48AB-828D-1CD660DC447F", L"extra"},
  };
  for (const auto& args : bad) {
    Options options;
    std::wstring error;
    EXPECT_FALSE(Parse(args, &options, &error)) << args.size();
    EXPECT_FALSE(error.empty());
  }
}

TEST(RunOutsideRunId, FromGuidAndValidation) {
  const GUID guid = {0x0c3534c4, 0x5b7a, 0x48ab, {0x82, 0x8d, 0x1c, 0xd6, 0x60, 0xdc, 0x44, 0x7f}};
  const std::wstring id = RunIdFromGuid(guid);
  EXPECT_EQ(id, L"0C3534C4-5B7A-48AB-828D-1CD660DC447F");
  EXPECT_TRUE(IsRunId(id));
  EXPECT_TRUE(IsRunId(L"0c3534c4-5b7a-48ab-828d-1cd660dc447f"));
  const GUID zero = {};
  EXPECT_EQ(RunIdFromGuid(zero), L"00000000-0000-0000-0000-000000000000");
  for (const wchar_t* bad :
       {L"", L"0C3534C4-5B7A-48AB-828D-1CD660DC447", L"0C3534C4-5B7A-48AB-828D-1CD660DC447FF",
        L"0C3534C45B7A-48AB-828D-1CD660DC447F-", L"0C3534C4-5B7A-48AB-828D-1CD660DC447G",
        L"..\\..\\x-5B7A-48AB-828D-1CD660DC447F", L"0C3534C4 5B7A-48AB-828D-1CD660DC447F"}) {
    EXPECT_FALSE(IsRunId(bad)) << bad;
  }
}

TEST(RunOutsidePaths, AbsoluteAndExe) {
  for (const wchar_t* path : {L"C:\\x.exe", L"c:/x.exe", L"\\\\server\\share\\x.exe",
                              L"\\\\?\\C:\\x.exe", L"\\\\.\\C:\\x"}) {
    EXPECT_TRUE(IsAbsolutePath(path)) << path;
  }
  for (const wchar_t* path :
       {L"", L"x.exe", L"C:x.exe", L"\\x.exe", L"..\\x.exe", L"\\\\\\x", L"1:\\x.exe"}) {
    EXPECT_FALSE(IsAbsolutePath(path)) << path;
  }
  for (const wchar_t* path : {L"C:\\a b\\x.exe", L"x.EXE", L"C:\\dir.d\\x.Exe"}) {
    EXPECT_TRUE(HasExeExtension(path)) << path;
  }
  for (const wchar_t* path : {L"C:\\x.cmd", L"C:\\x.bat", L"C:\\x.exe ", L"C:\\x.exe.txt", L".exe",
                              L"C:\\dir\\.exe", L"C:\\x.com", L"C:\\xexe"}) {
    EXPECT_FALSE(HasExeExtension(path)) << path;
  }
}

TEST(RunOutsideRequest, RoundTripKeepsEveryArgumentExactly) {
  Request request;
  request.program = L"C:\\Some Projects\\build dir\\bin\\PdfThumbnailsCli.exe";
  request.currentDirectory = L"C:\\Some Projects\\\u03b4\u03bf\u03ba\u03b9\u03bc\u03ae";
  request.arguments = AwkwardArguments();
  std::vector<BYTE> bytes;
  ASSERT_HRESULT_SUCCEEDED(EncodeRequest(request, &bytes));
  Request decoded;
  ASSERT_HRESULT_SUCCEEDED(DecodeRequest(bytes, &decoded));
  EXPECT_EQ(decoded.program, request.program);
  EXPECT_EQ(decoded.currentDirectory, request.currentDirectory);
  EXPECT_EQ(decoded.arguments, request.arguments);

  // No arguments, and only empty ones, are different requests.
  request.arguments.clear();
  ASSERT_HRESULT_SUCCEEDED(EncodeRequest(request, &bytes));
  ASSERT_HRESULT_SUCCEEDED(DecodeRequest(bytes, &decoded));
  EXPECT_TRUE(decoded.arguments.empty());
  request.arguments = {L"", L""};
  ASSERT_HRESULT_SUCCEEDED(EncodeRequest(request, &bytes));
  ASSERT_HRESULT_SUCCEEDED(DecodeRequest(bytes, &decoded));
  EXPECT_EQ(decoded.arguments, request.arguments);
}

TEST(RunOutsideRequest, EncodeRefusesWhatCannotBeRun) {
  Request request;
  request.program = L"C:\\x.exe";
  request.currentDirectory = L"C:\\";
  std::vector<BYTE> bytes;
  request.arguments = {std::wstring(L"a\0b", 3)};
  EXPECT_EQ(EncodeRequest(request, &bytes), E_INVALIDARG);
  request.arguments.clear();
  request.program = L"x.exe";
  EXPECT_EQ(EncodeRequest(request, &bytes), E_INVALIDARG);
  request.program = L"C:\\x.exe";
  request.currentDirectory = L"relative";
  EXPECT_EQ(EncodeRequest(request, &bytes), E_INVALIDARG);
}

TEST(RunOutsideRequest, DecodeRefusesAnythingElse) {
  Request request;
  request.program = L"C:\\x.exe";
  request.currentDirectory = L"C:\\work";
  request.arguments = {L"a", L"b"};
  std::vector<BYTE> good;
  ASSERT_HRESULT_SUCCEEDED(EncodeRequest(request, &good));
  const HRESULT invalid = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);

  // Builds a file from strings, each ended by a NUL.
  const auto file = [](std::vector<std::wstring> strings) {
    std::vector<BYTE> bytes;
    for (const std::wstring& text : strings) {
      const BYTE* data = reinterpret_cast<const BYTE*>(text.c_str());
      bytes.insert(bytes.end(), data, data + (text.size() + 1) * sizeof(wchar_t));
    }
    return bytes;
  };
  const std::wstring tag = L"pdft-run-outside request 1";
  ASSERT_EQ(file({tag, L"C:\\work", L"C:\\x.exe", L"2", L"a", L"b"}), good);

  Request decoded;
  std::vector<BYTE> odd = good;
  odd.pop_back();
  EXPECT_EQ(DecodeRequest(odd, &decoded), invalid);
  std::vector<BYTE> unterminated(good.begin(), good.end() - 2);
  EXPECT_EQ(DecodeRequest(unterminated, &decoded), invalid);
  EXPECT_EQ(DecodeRequest({}, &decoded), invalid);
  const std::vector<std::vector<std::wstring>> bad = {
      {L"pdft-run-outside request 2", L"C:\\work", L"C:\\x.exe", L"2", L"a", L"b"},
      {tag, L"C:\\work", L"C:\\x.exe", L"3", L"a", L"b"},   // too few arguments
      {tag, L"C:\\work", L"C:\\x.exe", L"1", L"a", L"b"},   // one too many
      {tag, L"C:\\work", L"C:\\x.exe", L"", L"a", L"b"},    // no count
      {tag, L"C:\\work", L"C:\\x.exe", L"+2", L"a", L"b"},  // not just digits
      {tag, L"C:\\work", L"C:\\x.exe", L"99999999999999999999"},
      {tag, L"C:\\work", L"x.exe", L"0"},  // relative program
      {tag, L"work", L"C:\\x.exe", L"0"},  // relative current folder
      {tag, L"C:\\work", L"C:\\x.exe"},    // no count at all
      {tag},
  };
  for (const auto& strings : bad) {
    EXPECT_EQ(DecodeRequest(file(strings), &decoded), invalid) << strings.size();
  }
  std::vector<BYTE> huge(kMaxRequestBytes + 2, 0);
  EXPECT_EQ(DecodeRequest(huge, &decoded), invalid);
}

// The child refuses a script however the request was made: CreateProcessW would hand it to the
// command interpreter, which interprets the arguments.
TEST(RunOutsideRequest, DecodeRefusesAProgramThatIsNotAnExe) {
  for (const wchar_t* program : {L"C:\\Some Projects\\x.cmd", L"C:\\Some Projects\\x.bat"}) {
    Request request;
    request.program = program;
    request.currentDirectory = L"C:\\Some Projects";
    request.arguments = {L"a & b"};
    std::vector<BYTE> bytes;
    ASSERT_HRESULT_SUCCEEDED(EncodeRequest(request, &bytes)) << program;  // not checked here
    Request decoded;
    EXPECT_EQ(DecodeRequest(bytes, &decoded), HRESULT_FROM_WIN32(ERROR_INVALID_DATA)) << program;
    EXPECT_TRUE(decoded.program.empty());
  }
}

TEST(RunOutsideCommandLine, SplitsBackIntoTheSameArguments) {
  const std::wstring program = L"C:\\Some Projects\\bin dir\\program.exe";
  std::wstring line;
  ASSERT_HRESULT_SUCCEEDED(BuildCommandLine(program, AwkwardArguments(), &line));
  std::vector<std::wstring> expected = {program};
  expected.insert(expected.end(), AwkwardArguments().begin(), AwkwardArguments().end());
  EXPECT_EQ(Split(line), expected) << line;
  // The program is always in quotes, also without spaces.
  ASSERT_HRESULT_SUCCEEDED(BuildCommandLine(L"C:\\x.exe", {}, &line));
  EXPECT_EQ(line, L"\"C:\\x.exe\"");
  const std::vector<std::wstring> one = {L"a b"};
  ASSERT_HRESULT_SUCCEEDED(BuildCommandLine(L"C:\\x.exe", one, &line));
  EXPECT_EQ(line, L"\"C:\\x.exe\" \"a b\"");
}

TEST(RunOutsideCommandLine, RefusesWhatCreateProcessCannotTake) {
  std::wstring line;
  EXPECT_EQ(BuildCommandLine(L"", {}, &line), E_INVALIDARG);
  EXPECT_EQ(BuildCommandLine(L"C:\\a\"b.exe", {}, &line), E_INVALIDARG);
  // 32767 characters including the NUL is the limit.
  const std::wstring program = L"C:\\x.exe";  // "\"C:\\x.exe\" " = 11 characters
  std::vector<std::wstring> fits = {std::wstring(kMaxCommandLine - 1 - 11, L'a')};
  ASSERT_HRESULT_SUCCEEDED(BuildCommandLine(program, fits, &line));
  EXPECT_EQ(line.size(), kMaxCommandLine - 1);
  fits[0] += L'a';
  EXPECT_EQ(BuildCommandLine(program, fits, &line), E_BOUNDS);
  EXPECT_TRUE(line.empty());
}

TEST(RunOutsideResult, RecordRoundTrip) {
  for (const DWORD code : {0ul, 3ul, 125ul, 0xC0000005ul, 0xFFFFFFFFul}) {
    DWORD parsed = 1234;
    ASSERT_TRUE(ParseResultRecord(FormatResultRecord(code), &parsed)) << code;
    EXPECT_EQ(parsed, code);
  }
  for (const char* bad : {"", "\r\n", "3", "3\n", " 3\r\n", "-3\r\n", "4294967296\r\n",
                          "99999999999\r\n", "3\r\n\r\n", "0x10\r\n"}) {
    DWORD parsed = 0;
    EXPECT_FALSE(ParseResultRecord(bad, &parsed)) << bad;
  }
}

TEST(RunOutsideResult, TheChildsRecordIsTheExitCode) {
  Outcome outcome;
  outcome.childExitCode = 3;
  outcome.lastTaskResult = 3;
  Verdict verdict = Translate(outcome);
  EXPECT_EQ(verdict.exitCode, 3);
  EXPECT_TRUE(verdict.message.empty());

  // The child's own codes pass through; it explained them in the output.
  outcome.childExitCode = static_cast<DWORD>(kExitBlocked);
  outcome.lastTaskResult = kExitBlocked;
  EXPECT_EQ(Translate(outcome).exitCode, kExitBlocked);

  // A program whose exit code looks like a Task Scheduler status still gets it back.
  outcome.childExitCode = static_cast<DWORD>(SCHED_S_TASK_HAS_NOT_RUN);
  outcome.lastTaskResult = SCHED_S_TASK_HAS_NOT_RUN;
  EXPECT_EQ(Translate(outcome).exitCode, SCHED_S_TASK_HAS_NOT_RUN);

  // An exception code comes back unchanged as the process exit code.
  outcome.childExitCode = 0xC0000005;
  outcome.lastTaskResult = static_cast<LONG>(0xC0000005);
  EXPECT_EQ(static_cast<DWORD>(Translate(outcome).exitCode), 0xC0000005u);

  // The record wins over a timeout or Ctrl+C that came too late to matter, and over a different
  // task result (with a note).
  outcome.childExitCode = 0;
  outcome.lastTaskResult = SCHED_S_TASK_TERMINATED;
  outcome.timedOut = true;
  outcome.interrupted = true;
  verdict = Translate(outcome);
  EXPECT_EQ(verdict.exitCode, 0);
  EXPECT_NE(verdict.message.find(L"0x00041306"), std::wstring::npos) << verdict.message;
}

TEST(RunOutsideResult, WithoutARecordTheCauseIsExplained) {
  Outcome outcome;
  outcome.lastTaskResult = SCHED_S_TASK_TERMINATED;
  outcome.interrupted = true;
  outcome.timedOut = true;
  EXPECT_EQ(Translate(outcome).exitCode, kExitInterrupted);

  outcome.interrupted = false;
  outcome.timeoutSeconds = 30;
  Verdict verdict = Translate(outcome);
  EXPECT_EQ(verdict.exitCode, kExitTimedOut);
  EXPECT_NE(verdict.message.find(L"30 s"), std::wstring::npos) << verdict.message;

  outcome.timedOut = false;
  const struct {
    LONG result;
    int exitCode;
    const wchar_t* says;
  } cases[] = {
      {SCHED_S_TASK_HAS_NOT_RUN, kExitFailed, L"did not start"},
      {SCHED_S_TASK_RUNNING, kExitFailed, L"had not finished"},
      {SCHED_S_TASK_QUEUED, kExitFailed, L"had not finished"},
      {SCHED_S_TASK_TERMINATED, kExitFailed, L"ended before"},
      {static_cast<LONG>(STATUS_CONTROL_C_EXIT), kExitFailed, L"ended before"},
      {SCHED_S_TASK_DISABLED, kExitFailed, L"disabled"},
      {HRESULT_FROM_WIN32(ERROR_REQUEST_REFUSED), kExitFailed, L"signed in"},
      {kExitFailed, kExitFailed, L"run folder"},
      {HRESULT_FROM_WIN32(ERROR_SYSTEM_INTEGRITY_POLICY_VIOLATION), kExitBlocked, L"Application"},
      {HRESULT_FROM_WIN32(ERROR_INVALID_IMAGE_HASH), kExitBlocked, L"Application"},
      {HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), kExitFailed, L"without recording"},
      {0, kExitFailed, L"without recording"},
      {3, kExitFailed, L"0x00000003"},
  };
  for (const auto& c : cases) {
    outcome.lastTaskResult = c.result;
    verdict = Translate(outcome);
    EXPECT_EQ(verdict.exitCode, c.exitCode) << std::hex << c.result;
    EXPECT_NE(verdict.message.find(c.says), std::wstring::npos)
        << std::hex << c.result << " " << testing::PrintToString(verdict.message);
  }
}

}  // namespace
}  // namespace pdft::outside
