// The app's log: <data dir>\Logs\<Product>.log (paths.h), plain UTF-8 text, one line per entry:
//
//   2026-10-30T01:26:04.512+02:00 4812 check INFO trigger=task result=healthy ms=1210
//
// local time with its UTC offset, process id, component, level, message. When the file would
// grow past 256 KB it becomes <Product>.log.1 (replacing the previous one) and a new file starts.
// Several processes may write at once: each line is one WriteFile on a handle opened for
// appending, and a per-user named mutex serializes appending and rotating.
//
// What goes in: each command's start and end with its exit code, steps and their HRESULTs, and
// WIL's failure reports. What never goes in: PDF paths or names, anything else the user typed or
// owns, environment values. Logging is best effort: a log that cannot be written never fails a
// command.

#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace pdft {

enum class LogLevel { kInfo, kWarning, kError };

// The size above which the log rotates.
inline constexpr ULONGLONG kLogRotateBytes = 256 * 1024;

// Turns the log on for this process; until then Log() writes nothing. Commands that log call it
// once they are sure to run (--version and --help never do).
void EnableLog() noexcept;

// Appends one line. |component| is one word ("check", "zoom", "wil"); line breaks and other
// control characters in |message| become spaces, so an entry is always one line.
void Log(LogLevel level, std::wstring_view component, std::wstring_view message) noexcept;

// Routes WIL's failure reports (RETURN_IF_FAILED and friends, CATCH_RETURN) into the log as
// "wil ERROR hr=0x80070005 line=42 at=+0x1a2b0 caller=+0x3c4d0" (offsets into the module, to
// look up with its PDB; the build records no file names). Call once per process.
void LogWilFailures() noexcept;

// One log line, line break included (pure). |local| is the local time, |utcOffsetMinutes| its
// offset from UTC (+120 for UTC+2). May throw std::bad_alloc.
std::wstring FormatLogLine(const SYSTEMTIME& local, int utcOffsetMinutes, DWORD processId,
                           std::wstring_view component, LogLevel level, std::wstring_view message);

// The offset of |local| from |utc| in whole minutes (pure).
int UtcOffsetMinutes(const FILETIME& utc, const FILETIME& local) noexcept;

// For tests: log to |file| (its folder must exist) and rotate above |rotateBytes|, and turn the
// log on. An empty |file| goes back to the real log path, and the log off.
void SetLogFileForTesting(const std::filesystem::path& file, ULONGLONG rotateBytes) noexcept;

}  // namespace pdft
