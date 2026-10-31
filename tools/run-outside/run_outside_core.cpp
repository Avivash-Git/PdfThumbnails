#include "run_outside_core.h"

#include <cstring>
#include <cwchar>

#include "medium_il.h"

namespace pdft::outside {
namespace {

// The first string of a request file; a new layout gets a new tag.
constexpr wchar_t kRequestTag[] = L"pdft-run-outside request 1";
// More arguments than this cannot fit into one command line (each takes at least a space).
constexpr size_t kMaxArguments = kMaxCommandLine / 2;

HRESULT InvalidData() { return HRESULT_FROM_WIN32(ERROR_INVALID_DATA); }

bool IsAsciiDigits(std::wstring_view text) {
  if (text.empty()) return false;
  for (const wchar_t c : text) {
    if (c < L'0' || c > L'9') return false;
  }
  return true;
}

// Decimal digits only, at most |max|; stops counting as soon as the value is too large, so a
// long run of digits cannot wrap around to a small number.
bool ParseDecimal(std::wstring_view text, unsigned long long max, unsigned long long* value) {
  if (!IsAsciiDigits(text)) return false;
  unsigned long long result = 0;
  for (const wchar_t c : text) {
    result = result * 10 + static_cast<unsigned>(c - L'0');
    if (result > max) return false;
  }
  *value = result;
  return true;
}

bool IsHex(wchar_t c) {
  return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

bool IsAsciiLetter(wchar_t c) { return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z'); }

std::wstring Hex(LONG value) {
  wchar_t text[16];
  std::swprintf(text, 16, L"0x%08lX", static_cast<unsigned long>(value));
  return text;
}

void AppendString(std::wstring_view text, std::vector<BYTE>* bytes) {
  const BYTE* data = reinterpret_cast<const BYTE*>(text.data());
  bytes->insert(bytes->end(), data, data + text.size() * sizeof(wchar_t));
  bytes->push_back(0);  // the terminating NUL, two bytes in UTF-16
  bytes->push_back(0);
}

}  // namespace

bool ParseOptions(std::span<const std::wstring_view> args, Options* options, std::wstring* error) {
  *options = Options();
  error->clear();
  if (args.empty()) {
    *error = L"nothing to run";
    return false;
  }
  const std::wstring_view first = args[0];
  if (first == L"--help" || first == L"-h" || first == L"-?" || first == L"/?") {
    if (args.size() != 1) {
      *error = L"--help takes no other arguments";
      return false;
    }
    options->mode = Mode::kHelp;
    return true;
  }
  if (first == L"--child") {
    if (args.size() != 2 || !IsRunId(args[1])) {
      *error = L"--child needs exactly one run id (it is meant for the scheduled task only)";
      return false;
    }
    options->mode = Mode::kChild;
    options->runId = args[1];
    return true;
  }

  size_t i = 0;
  bool timeoutSeen = false;
  for (; i < args.size() && args[i] != L"--"; ++i) {
    if (args[i] != L"--timeout") {
      *error = L"unknown option \"" + std::wstring(args[i]) + L"\" (put -- before the program)";
      return false;
    }
    if (timeoutSeen) {
      *error = L"--timeout is given twice";
      return false;
    }
    timeoutSeen = true;
    unsigned long long seconds = 0;
    if (i + 1 >= args.size() || !ParseDecimal(args[i + 1], kMaxTimeoutSeconds, &seconds) ||
        seconds == 0) {
      *error =
          L"--timeout needs a number of seconds from 1 to " + std::to_wstring(kMaxTimeoutSeconds);
      return false;
    }
    options->timeoutSeconds = static_cast<DWORD>(seconds);
    ++i;
  }
  if (i >= args.size()) {
    *error = L"put -- before the program to run";
    return false;
  }
  ++i;  // the "--"
  if (i >= args.size() || args[i].empty()) {
    *error = L"no program after --";
    return false;
  }
  options->mode = Mode::kRun;
  options->program = args[i];
  for (++i; i < args.size(); ++i) options->arguments.emplace_back(args[i]);
  return true;
}

std::wstring RunIdFromGuid(const GUID& guid) {
  wchar_t text[40] = {};
  std::swprintf(text, 40, L"%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
                static_cast<unsigned long>(guid.Data1), guid.Data2, guid.Data3, guid.Data4[0],
                guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                guid.Data4[6], guid.Data4[7]);
  return text;
}

bool IsRunId(std::wstring_view text) {
  if (text.size() != 36) return false;
  for (size_t i = 0; i < text.size(); ++i) {
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? text[i] != L'-' : !IsHex(text[i])) return false;
  }
  return true;
}

bool IsAbsolutePath(std::wstring_view path) {
  if (path.size() >= 3 && IsAsciiLetter(path[0]) && path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  // \\server\share\... and the \\?\ and \\.\ forms.
  return path.size() >= 3 && path[0] == L'\\' && path[1] == L'\\' && path[2] != L'\\';
}

bool HasExeExtension(std::wstring_view path) {
  constexpr std::wstring_view kExe = L".exe";
  if (path.size() <= kExe.size()) return false;
  const std::wstring_view end = path.substr(path.size() - kExe.size());
  for (size_t i = 0; i < kExe.size(); ++i) {
    const wchar_t c = end[i] >= L'A' && end[i] <= L'Z' ? end[i] - L'A' + L'a' : end[i];
    if (c != kExe[i]) return false;
  }
  const wchar_t before = path[path.size() - kExe.size() - 1];
  return before != L'\\' && before != L'/';
}

HRESULT EncodeRequest(const Request& request, std::vector<BYTE>* bytes) {
  bytes->clear();
  if (!IsAbsolutePath(request.program) || !IsAbsolutePath(request.currentDirectory)) {
    return E_INVALIDARG;
  }
  const auto hasNul = [](std::wstring_view text) { return text.find(L'\0') != text.npos; };
  if (hasNul(request.program) || hasNul(request.currentDirectory)) return E_INVALIDARG;
  for (const std::wstring& argument : request.arguments) {
    if (hasNul(argument)) return E_INVALIDARG;
  }
  if (request.arguments.size() > kMaxArguments) return E_INVALIDARG;
  AppendString(kRequestTag, bytes);
  AppendString(request.currentDirectory, bytes);
  AppendString(request.program, bytes);
  AppendString(std::to_wstring(request.arguments.size()), bytes);
  for (const std::wstring& argument : request.arguments) AppendString(argument, bytes);
  return S_OK;
}

HRESULT DecodeRequest(std::span<const BYTE> bytes, Request* request) {
  *request = Request();
  if (bytes.empty() || bytes.size() % sizeof(wchar_t) != 0 || bytes.size() > kMaxRequestBytes) {
    return InvalidData();
  }
  std::wstring units(bytes.size() / sizeof(wchar_t), L'\0');
  std::memcpy(units.data(), bytes.data(), bytes.size());
  if (units.back() != L'\0') return InvalidData();  // the last string must be complete

  // Split at every NUL; the final NUL ends the last string.
  std::vector<std::wstring_view> strings;
  const std::wstring_view all(units.data(), units.size() - 1);
  size_t start = 0;
  for (;;) {
    const size_t end = all.find(L'\0', start);
    strings.push_back(all.substr(start, end == all.npos ? all.npos : end - start));
    if (end == all.npos) break;
    start = end + 1;
  }
  unsigned long long count = 0;
  if (strings.size() < 4 || strings[0] != kRequestTag ||
      !ParseDecimal(strings[3], kMaxArguments, &count) || strings.size() != 4 + count) {
    return InvalidData();
  }
  Request decoded;
  decoded.currentDirectory = strings[1];
  decoded.program = strings[2];
  if (!IsAbsolutePath(decoded.program) || !IsAbsolutePath(decoded.currentDirectory)) {
    return InvalidData();
  }
  // Checked again on the side that runs the program, however the request was made:
  // CreateProcessW would hand a .bat or .cmd to the command interpreter.
  if (!HasExeExtension(decoded.program)) return InvalidData();
  for (size_t i = 4; i < strings.size(); ++i) decoded.arguments.emplace_back(strings[i]);
  *request = std::move(decoded);
  return S_OK;
}

HRESULT BuildCommandLine(const std::wstring& program, std::span<const std::wstring> arguments,
                         std::wstring* line) {
  line->clear();
  if (program.empty() || program.find(L'"') != program.npos) return E_INVALIDARG;
  // The program name is read up to the closing quote, without the backslash rules of the
  // arguments, so it is quoted as it is.
  std::wstring result = L"\"" + program + L"\"";
  for (const std::wstring& argument : arguments) {
    result += L' ';
    result += pdft::test::QuoteArgument(argument);
    if (result.size() >= kMaxCommandLine) return E_BOUNDS;
  }
  if (result.size() >= kMaxCommandLine) return E_BOUNDS;
  *line = std::move(result);
  return S_OK;
}

std::string FormatResultRecord(DWORD exitCode) { return std::to_string(exitCode) + "\r\n"; }

bool ParseResultRecord(std::string_view text, DWORD* exitCode) {
  if (text.size() < 2 || text.substr(text.size() - 2) != "\r\n") return false;
  text.remove_suffix(2);
  if (text.empty() || text.size() > 10) return false;
  unsigned long long value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<unsigned>(c - '0');
  }
  if (value > MAXDWORD) return false;
  *exitCode = static_cast<DWORD>(value);
  return true;
}

Verdict Translate(const Outcome& outcome) {
  Verdict verdict;
  if (outcome.childExitCode) {
    verdict.exitCode = static_cast<int>(*outcome.childExitCode);
    if (outcome.lastTaskResult != static_cast<LONG>(*outcome.childExitCode)) {
      verdict.message = L"note: Task Scheduler gives " + Hex(outcome.lastTaskResult) +
                        L" as the task's result; the helper recorded exit code " +
                        std::to_wstring(*outcome.childExitCode) + L", which is returned";
    }
    return verdict;
  }
  if (outcome.interrupted) {
    verdict.exitCode = kExitInterrupted;
    verdict.message = L"interrupted: the task was stopped before the program finished";
    return verdict;
  }
  if (outcome.timedOut) {
    verdict.exitCode = kExitTimedOut;
    verdict.message = L"the program was still running after " +
                      std::to_wstring(outcome.timeoutSeconds) + L" s (--timeout) and was stopped";
    return verdict;
  }

  const LONG result = outcome.lastTaskResult;
  const std::wstring code = L" (task result " + Hex(result) + L")";
  verdict.exitCode = kExitFailed;
  switch (result) {
    case SCHED_S_TASK_HAS_NOT_RUN:
      verdict.message = L"Task Scheduler did not start the task" + code;
      return verdict;
    case SCHED_S_TASK_RUNNING:
    case SCHED_S_TASK_QUEUED:
      verdict.message = L"the task had not finished when pdft-run-outside stopped waiting" + code;
      return verdict;
    case SCHED_S_TASK_TERMINATED:
    case static_cast<LONG>(STATUS_CONTROL_C_EXIT):
      verdict.message = L"the task was ended before the program finished" + code;
      return verdict;
    case SCHED_S_TASK_DISABLED:
      verdict.message = L"the task is disabled" + code;
      return verdict;
    case HRESULT_FROM_WIN32(ERROR_REQUEST_REFUSED):
      verdict.message =
          L"Task Scheduler refused to start the task; a task with the user's interactive token "
          L"runs only while that user is signed in to Windows" +
          code;
      return verdict;
    case kExitFailed:
      verdict.message =
          L"the helper started by the task found no usable run folder under %TEMP% and exited "
          L"125 (does the scheduled task see the same %TEMP% as this terminal?)";
      return verdict;
    default:
      break;
  }
  if (pdft::test::IsApplicationControlBlock(result)) {
    verdict.exitCode = kExitBlocked;
    verdict.message =
        L"Application Control (Smart App Control or an App Control policy) did not let Task "
        L"Scheduler start pdft-run-outside.exe itself" +
        code;
    return verdict;
  }
  verdict.message = L"the helper started by the task ended without recording a result" + code;
  return verdict;
}

}  // namespace pdft::outside
