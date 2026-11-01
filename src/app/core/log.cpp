#include "log.h"

#include <wil/resource.h>
#include <wil/result_macros.h>

#include <cstdlib>
#include <cwchar>
#include <memory>
#include <system_error>

#include "instance.h"
#include "paths.h"
#include "strings.h"

namespace pdft {
namespace {

// How long a line waits for another process to finish appending or rotating. After that it is
// appended anyway (one append is atomic on its own) and only the rotation waits for a later line.
constexpr DWORD kMutexWaitMs = 2000;

struct LogState {
  bool enabled = false;
  bool testFile = false;  // |path| was set by SetLogFileForTesting
  bool resolved = false;  // |path| and |mutex| looked up (once; a failed lookup stays failed)
  std::wstring path;      // empty: nowhere to log
  ULONGLONG rotateBytes = kLogRotateBytes;
  wil::unique_handle mutex;  // the per-user log mutex; empty if it could not be made
};

// Guards LogState and keeps this process's threads from writing at the same time. A constant
// initializer: no code runs before main (startup.h).
SRWLOCK g_lock = SRWLOCK_INIT;

// Created on first use, not before main.
LogState& State() {
  static LogState state;
  return state;
}

// Looks up the log file and the mutex; called with g_lock held.
void Resolve(LogState& state) {
  state.resolved = true;
  if (!state.testFile) {
    std::filesystem::path path;
    if (FAILED(GetLogPath(&path))) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return;
    state.path = path.native();
  }
  std::wstring sid;
  bool existed = false;
  if (SUCCEEDED(GetCurrentUserSid(&sid))) {
    (void)CreateNamedMutex(LogMutexName(sid), sid, false, 0, std::addressof(state.mutex), &existed);
  }
}

// The log file, opened for appending only: every WriteFile lands at its end, whatever another
// process wrote in between. Shared for reading, writing and deleting, so other processes can log
// and readers never block a rotation.
wil::unique_hfile OpenForAppend(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  return wil::unique_hfile(file);
}

void WriteLine(LogLevel level, std::wstring_view component, std::wstring_view message) {
  AcquireSRWLockExclusive(&g_lock);
  const auto unlock = wil::scope_exit([]() noexcept { ReleaseSRWLockExclusive(&g_lock); });
  LogState& state = State();
  if (!state.enabled) return;
  if (!state.resolved) Resolve(state);
  if (state.path.empty()) return;

  FILETIME utc = {};
  GetSystemTimeAsFileTime(&utc);
  SYSTEMTIME utcTime = {};
  SYSTEMTIME localTime = {};
  FILETIME local = {};
  if (!FileTimeToSystemTime(&utc, &utcTime) ||
      !SystemTimeToTzSpecificLocalTime(nullptr, &utcTime, &localTime) ||
      !SystemTimeToFileTime(&localTime, &local)) {
    return;
  }
  const std::string bytes = WideToUtf8Lossy(FormatLogLine(
      localTime, UtcOffsetMinutes(utc, local), GetCurrentProcessId(), component, level, message));
  if (bytes.empty() || bytes.size() > MAXDWORD) return;

  bool owned = false;
  if (state.mutex) {
    const DWORD wait = WaitForSingleObject(state.mutex.get(), kMutexWaitMs);
    // Abandoned: a writer died mid-line; the file is still fine to append to.
    owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
  }
  const auto release = wil::scope_exit([&]() noexcept {
    if (owned) ReleaseMutex(state.mutex.get());
  });

  wil::unique_hfile file = OpenForAppend(state.path);
  if (!file) return;
  LARGE_INTEGER size = {};
  // Rotate only while holding the mutex, so two processes never rotate the same file twice.
  if (owned && GetFileSizeEx(file.get(), &size) && size.QuadPart > 0 &&
      static_cast<ULONGLONG>(size.QuadPart) + bytes.size() > state.rotateBytes) {
    file.reset();
    // Fails when a reader holds the old .log.1 without sharing delete: then this file just
    // grows until a later line can rotate it.
    MoveFileExW(state.path.c_str(), (state.path + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
    file = OpenForAppend(state.path);
    if (!file) return;
  }
  DWORD written = 0;
  WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

const wchar_t* LevelName(LogLevel level) {
  switch (level) {
    case LogLevel::kInfo:
      return L"INFO";
    case LogLevel::kWarning:
      return L"WARN";
    case LogLevel::kError:
      return L"ERROR";
  }
  return L"INFO";
}

bool IsLineBreakOrControl(wchar_t c) {
  return c < 0x20 || c == 0x7F || c == 0x85 || c == 0x2028 || c == 0x2029;
}

// "+0x1a2b0" relative to the module that holds |address|; empty for no address.
std::wstring ModuleOffset(const void* address) {
  if (!address) return {};
  HMODULE module = nullptr;
  wchar_t text[32] = {};
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          static_cast<LPCWSTR>(address), &module)) {
    const auto offset = static_cast<const BYTE*>(address) - reinterpret_cast<const BYTE*>(module);
    swprintf_s(text, L"+0x%llX", static_cast<unsigned long long>(offset));
  } else {
    swprintf_s(text, L"0x%p", address);
  }
  return text;
}

void __stdcall OnWilFailure(const wil::FailureInfo& failure) noexcept {
  try {
    std::wstring message = L"hr=" + HResultText(failure.hr);
    if (failure.uLineNumber) message += L" line=" + std::to_wstring(failure.uLineNumber);
    if (failure.returnAddress) message += L" at=" + ModuleOffset(failure.returnAddress);
    if (failure.callerReturnAddress) {
      message += L" caller=" + ModuleOffset(failure.callerReturnAddress);
    }
    // Only our own *_MSG macros attach a message, and they never carry paths or user data.
    if (failure.pszMessage) message += std::wstring(L" ") + failure.pszMessage;
    // LOG_* macros report failures the code carries on from.
    const LogLevel level =
        failure.type == wil::FailureType::Log ? LogLevel::kWarning : LogLevel::kError;
    Log(level, L"wil", message);
  } catch (...) {
  }
}

}  // namespace

void EnableLog() noexcept {
  AcquireSRWLockExclusive(&g_lock);
  State().enabled = true;
  ReleaseSRWLockExclusive(&g_lock);
}

void DisableLog() noexcept {
  AcquireSRWLockExclusive(&g_lock);
  State().enabled = false;
  ReleaseSRWLockExclusive(&g_lock);
}

void Log(LogLevel level, std::wstring_view component, std::wstring_view message) noexcept {
  // A failure inside logging reaches WIL's callback, which would log again: once is enough.
  thread_local bool writing = false;
  if (writing) return;
  writing = true;
  try {
    WriteLine(level, component, message);
  } catch (...) {
  }
  writing = false;
}

void LogWilFailures() noexcept { wil::SetResultLoggingCallback(&OnWilFailure); }

std::wstring FormatLogLine(const SYSTEMTIME& local, int utcOffsetMinutes, DWORD processId,
                           std::wstring_view component, LogLevel level, std::wstring_view message) {
  const int offset = std::abs(utcOffsetMinutes);
  wchar_t stamp[48] = {};
  swprintf_s(stamp, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u%c%02d:%02d", local.wYear, local.wMonth,
             local.wDay, local.wHour, local.wMinute, local.wSecond, local.wMilliseconds,
             utcOffsetMinutes < 0 ? L'-' : L'+', offset / 60, offset % 60);
  std::wstring line = stamp;
  line += L' ';
  line += std::to_wstring(processId);
  line += L' ';
  if (component.empty()) component = L"app";
  for (wchar_t c : component) line += IsLineBreakOrControl(c) || c == L' ' ? L'-' : c;
  line += L' ';
  line += LevelName(level);
  line += L' ';
  for (wchar_t c : message) line += IsLineBreakOrControl(c) ? L' ' : c;
  line += L"\r\n";
  return line;
}

int UtcOffsetMinutes(const FILETIME& utc, const FILETIME& local) noexcept {
  const auto ticks = [](const FILETIME& time) {
    return static_cast<long long>((static_cast<unsigned long long>(time.dwHighDateTime) << 32) |
                                  time.dwLowDateTime);
  };
  constexpr long long kTicksPerMinute = 60LL * 10'000'000;
  const long long difference = ticks(local) - ticks(utc);
  const long long half = difference < 0 ? -kTicksPerMinute / 2 : kTicksPerMinute / 2;
  return static_cast<int>((difference + half) / kTicksPerMinute);
}

void SetLogFileForTesting(const std::filesystem::path& file, ULONGLONG rotateBytes) noexcept {
  AcquireSRWLockExclusive(&g_lock);
  LogState& state = State();
  try {
    state.path = file.native();
    state.testFile = !file.empty();
    state.enabled = !file.empty();
    state.rotateBytes = file.empty() ? kLogRotateBytes : rotateBytes;
    state.resolved = false;
    state.mutex.reset();
  } catch (...) {
    state.path.clear();
    state.enabled = false;
  }
  ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace pdft
