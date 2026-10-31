// The parts of pdft-run-outside that need no system state, so the unit tests can cover them: the
// command line, the run id, the request file the child reads, the child's command line for the
// program, the result record, and how the run's outcome becomes the exit code.

#pragma once

#include <windows.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pdft::outside {

// Exit codes of pdft-run-outside itself; otherwise it returns the program's own exit code. Like
// the ones of timeout(1) and env(1), so scripts can tell them apart from most programs' codes.
inline constexpr int kExitTimedOut = 124;     // ran longer than --timeout and was stopped
inline constexpr int kExitFailed = 125;       // pdft-run-outside could not do its part
inline constexpr int kExitBlocked = 126;      // Application Control refused to start it
inline constexpr int kExitNotStarted = 127;   // it could not be started for another reason
inline constexpr int kExitInterrupted = 130;  // Ctrl+C (or the console closed) while it ran

inline constexpr DWORD kDefaultTimeoutSeconds = 120;
inline constexpr DWORD kMaxTimeoutSeconds = 24 * 60 * 60;
// CreateProcessW's limit for a command line, its terminating NUL included.
inline constexpr size_t kMaxCommandLine = 32767;

// Names inside the dev task folder and the per-run folder (both from project_config.h).
inline constexpr wchar_t kTaskNamePrefix[] = L"run-";
inline constexpr wchar_t kRequestFile[] = L"argv";
inline constexpr wchar_t kOutputFile[] = L"output";
inline constexpr wchar_t kResultFile[] = L"result";

enum class Mode {
  kHelp,   // --help
  kRun,    // [--timeout <seconds>] -- <program> [arguments...]
  kChild,  // --child <run id>: started by the scheduled task
};

struct Options {
  Mode mode = Mode::kHelp;
  DWORD timeoutSeconds = kDefaultTimeoutSeconds;
  std::wstring program;  // as given
  std::vector<std::wstring> arguments;
  std::wstring runId;  // kChild
};

// Parses the arguments after the program name. On a usage error returns false and sets |error|
// to one line that says what is wrong.
bool ParseOptions(std::span<const std::wstring_view> args, Options* options, std::wstring* error);

// A run id is a GUID as 36 hexadecimal digits and dashes (8-4-4-4-12), without braces: it names
// the task and the per-run folder, and the child takes it from its command line.
std::wstring RunIdFromGuid(const GUID& guid);
bool IsRunId(std::wstring_view text);

// "X:\..." or "\\server\share\..." (also "\\?\..."): a path that does not depend on a current
// drive or folder.
bool IsAbsolutePath(std::wstring_view path);
// True when |path| names a file ending in ".exe" (any case). Only programs are run: CreateProcessW
// would hand a .bat or .cmd file to the command interpreter, which interprets the arguments.
bool HasExeExtension(std::wstring_view path);

// What the child starts, as the caller wrote it to the request file.
struct Request {
  std::wstring program;  // absolute path
  std::vector<std::wstring> arguments;
  std::wstring currentDirectory;  // absolute path
};

// The request file: UTF-16LE strings, each ended by a NUL: a format tag, the current directory,
// the program, the number of arguments in decimal digits, then each argument (an empty argument
// is just its NUL). E_INVALIDARG when a string contains a NUL or a path is not absolute.
HRESULT EncodeRequest(const Request& request, std::vector<BYTE>* bytes);
// The reverse. HRESULT_FROM_WIN32(ERROR_INVALID_DATA) for anything that is not exactly such a
// file: another tag, an odd size, a missing or extra string, a bad count, a relative path, or a
// program that is not an .exe (EncodeRequest does not check that; the caller does, and this
// checks again).
HRESULT DecodeRequest(std::span<const BYTE> bytes, Request* request);
// Larger request files are refused without being read.
inline constexpr size_t kMaxRequestBytes = 1 << 20;

// The command line for CreateProcessW: the program in quotes (a path cannot contain a quote, and
// quoting it keeps a path with spaces from being read as a shorter one), then each argument
// quoted as CommandLineToArgvW and the C runtime parse it. E_INVALIDARG for an empty program or
// one containing a quote, E_BOUNDS when the line is longer than kMaxCommandLine allows.
HRESULT BuildCommandLine(const std::wstring& program, std::span<const std::wstring> arguments,
                         std::wstring* line);

// The child's result record: its exit code in decimal digits and a line end. The child writes
// it just before it exits, so the caller can tell a program's exit code from a status code of
// Task Scheduler's own, which arrives in the same place (the task's last result).
std::string FormatResultRecord(DWORD exitCode);
bool ParseResultRecord(std::string_view text, DWORD* exitCode);

// How a run ended, as the caller saw it.
struct Outcome {
  std::optional<DWORD> childExitCode;  // from the result record, when the child wrote one
  LONG lastTaskResult = 0;             // IRegisteredTask::get_LastTaskResult
  bool timedOut = false;               // it was still running at the time limit
  bool interrupted = false;            // Ctrl+C or the console closed
  DWORD timeoutSeconds = kDefaultTimeoutSeconds;
};

struct Verdict {
  int exitCode = kExitFailed;
  std::wstring message;  // for standard error; empty when there is nothing to add
};

// The exit code and message for |outcome|. The child's record wins: it is the program's exit
// code (or the child's own 125, 126 or 127, explained in the output). Without one the child did
// not finish: interrupted, timed out, or the task's last result is a Task Scheduler status or
// the code the child was ended with, which is explained.
Verdict Translate(const Outcome& outcome);

}  // namespace pdft::outside
