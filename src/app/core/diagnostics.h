// --diagnostics: a plain-text report for bug reports, with personal details masked.
//
// It holds what a maintainer needs to understand a report: this build and the compiler that made
// it, the Windows build, the process context, the scope, both classes, the slots with what the
// registry calls the handlers in them, the effective handler, the default app and its overlay,
// the Explorer test (the last recorded result: the report never starts a thumbnail process),
// Smart App Control with its verdict and block count, the check task, the last check, 1.x
// leftovers, the build folders, what could not be read, the findings, the status lines and the
// last lines of the log.
//
// Everything passes through Redact before it is shown, the log lines and every path included:
//   the profile folder               -> %USERPROFILE%
//   the user, computer and domain    -> <user>, <pc>, <domain>
//   SIDs                             -> S-1-5-21-<masked>-<RID> (other long SIDs keep only their
//                                       first two parts: S-1-12-1-<masked>)

#pragma once

#include <windows.h>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "context.h"
#include "evaluate.h"
#include "snapshot.h"

namespace pdft {

// What Redact masks. Empty fields mask nothing.
struct RedactContext {
  std::wstring profile;  // "C:\Users\<name>"
  // The same folder in its 8.3 form ("C:\Users\<NAME~1>"), when it has one. Its own name is
  // masked as the user's name too, and so is the long folder name when it is not the account
  // name (an account renamed after its profile was made).
  std::wstring profileShort;
  std::wstring user;      // the account name
  std::wstring computer;  // the computer's NetBIOS name
  std::wstring hostName;  // its DNS host name, which may be longer than the NetBIOS name
  std::wstring domain;    // the account's domain (the computer's name for a local account)
};

// Names this long or longer are masked wherever they occur, also inside a longer word or a
// path; shorter ones only as a word of their own (a user called "al" must not turn "already"
// into "<user>ready").
inline constexpr size_t kRedactAnywhereChars = 4;

// |text| with the details of |context| masked (pure), in one pass from left to right, so a
// replacement is never masked again: at each position a SID first, then the longest matching
// name, compared ignoring case. May throw std::bad_alloc.
std::wstring Redact(std::wstring_view text, const RedactContext& context);

// How many log lines the report ends with.
inline constexpr size_t kDiagnosticsLogLines = 30;

// The report (pure), already masked, lines separated by "\n". |logTail| is the end of the log,
// oldest line first. May throw std::bad_alloc.
std::wstring BuildDiagnostics(const Snapshot& snapshot, std::span<const Finding> findings,
                              std::span<const std::wstring> logTail, const RedactContext& redact);

// "MSVC 19.51.36231.0": the compiler that built this program (pure). May throw std::bad_alloc.
std::wstring CompilerText();

// The last |maxLines| lines of |text| (pure), without line ends. When |startsMidLine|, the first
// line is a fragment (the text was cut from the middle of a file) and is dropped. May throw
// std::bad_alloc.
std::vector<std::wstring> LastLines(std::wstring_view text, size_t maxLines, bool startsMidLine);

// The details of this user and PC to mask: the profile folder, the account and domain from
// |context|, the computer's names. Parts that cannot be read stay empty (fails only when out of
// memory).
[[nodiscard]] HRESULT GetRedactContext(const ProcessContext& context,
                                       RedactContext* redact) noexcept;

// The masked report for a process in |context|: reads the state (never starting a thumbnail
// process: it shows the last recorded Explorer test), the findings and DiagnosticsLogTail. Lines
// are separated by "\n" (ToCrLf for the clipboard). What --diagnostics and the settings window's
// Copy diagnostics give. Fails when the state cannot be read.
[[nodiscard]] HRESULT MakeDiagnosticsReport(const ProcessContext& context,
                                            std::wstring* report) noexcept;

// Puts |text| on the clipboard as CF_UNICODETEXT; waits up to a second for another program that
// has the clipboard open.
[[nodiscard]] HRESULT CopyTextToClipboard(std::wstring_view text) noexcept;

// The last |maxLines| lines of the app's log, from the rotated file too when the current one is
// shorter. A log that does not exist gives no lines; S_OK then.
[[nodiscard]] HRESULT ReadLogTail(size_t maxLines, std::vector<std::wstring>* lines) noexcept;

// The same for the log file |log| (and |log|.1).
[[nodiscard]] HRESULT ReadLogTailFrom(const std::filesystem::path& log, size_t maxLines,
                                      std::vector<std::wstring>* lines) noexcept;

// What the report shows in place of the log's lines when an elevated process makes it.
inline constexpr wchar_t kLogNotReadElevated[] = L"(not read: running as administrator)";

// The log lines the report of a process in |context| ends with: the last kDiagnosticsLogLines of
// the app's log (ReadLogTail), or one line that says why there are none. An elevated process does
// not open the log at all (kLogNotReadElevated): it lies in the user's profile, whose folders the
// user's unelevated programs can turn into links to any file an administrator may read. A log that
// cannot be read costs its lines, not the report. May throw std::bad_alloc.
std::vector<std::wstring> DiagnosticsLogTail(const ProcessContext& context);

}  // namespace pdft
