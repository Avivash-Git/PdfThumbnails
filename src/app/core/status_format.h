// The status report (pure): the lines --status prints and the settings window's status block
// shows, built from a snapshot and its findings. One line per check, in a fixed order, then a
// line for each finding that has no check line of its own, then the result:
//
//   PdfThumbnails 2.0.0.0 (x64), Windows build 26300.9818
//        Install        machine-wide (C:\Program Files\PdfThumbnails); settings per user
//     OK Handler        C:\Program Files\PdfThumbnails\app\2.0.0.0-1a2b3c4d\PdfThumbnails.dll (...)
//     !! .pdf slot      .pdf -> {...} (Some viewer, another handler)
//                       --fix --take-back points it back at PdfThumbnails
//   Result: 1 problem keeps PDF thumbnails from working
//
// "OK" marks a check that passed, "!!" a problem, and a problem line is followed by a line that
// says what fixes it (a command where one exists). The Handler line names the DLL registered for
// the install's scope, which is the one Explorer's thumbnail process loads.
//
// Every text a user reads is built here, in one place, so it can move into the string table
// with the settings window. Non-ASCII characters are written as \u escapes.

#pragma once

#include <windows.h>

#include <span>
#include <string>
#include <vector>

#include "check_schedule.h"
#include "evaluate.h"
#include "exit_code.h"
#include "live_test.h"
#include "repair.h"
#include "snapshot.h"

namespace pdft {

struct StatusLine {
  char mark = ' ';     // 'K' OK, '!' problem, ' ' neutral
  std::wstring label;  // "Handler"; empty for a line of its own (the first line, the result)
  std::wstring text;
  std::wstring hint;  // what fixes it; printed on its own line below (empty: none)
};

// The report's lines (pure but for the time zone used to show times). May throw std::bad_alloc.
std::vector<StatusLine> FormatStatus(const Snapshot& snapshot, std::span<const Finding> findings);

// The lines as text (pure): one line each, hints indented below their line, joined with "\n"
// and no line end after the last. May throw std::bad_alloc.
std::wstring RenderStatus(std::span<const StatusLine> lines);

// What a finding says, in one sentence without a final period, and what fixes it (empty when
// nothing does). The JSON output and the diagnostics use the same texts. May throw
// std::bad_alloc.
std::wstring FindingText(const Snapshot& snapshot, const Finding& finding);
std::wstring FindingHint(const Snapshot& snapshot, const Finding& finding);

// The last line: "Result: healthy", "Result: not installed" or how many problems keep thumbnails
// from working (pure). May throw std::bad_alloc.
std::wstring ResultText(std::span<const Finding> findings);

// What is wrong with a --test-file path, as the end of "The test file ..." (pure). May throw
// std::bad_alloc.
std::wstring TestFileProblemText(TestFileProblem problem);

// One line about a repair step and what became of it (pure): "Done: point the empty .pdf slot
// at PdfThumbnails". May throw std::bad_alloc.
std::wstring RepairOutcomeText(const RepairOutcome& outcome);

// The two checkboxes as the Checks line and --checks show them (pure): "after PdfCraft updates:
// on · at sign-in: off". May throw std::bad_alloc.
std::wstring ChecksStateText(bool afterPdfCraft, bool atSignIn);

// The --checks value that asks for these checkboxes (pure): "default" (both), "pdfcraft",
// "signin" or "none".
const wchar_t* ChecksValue(bool afterPdfCraft, bool atSignIn) noexcept;

// What --checks prints (pure): |out| for standard output, |error| for standard error.
struct ScheduleReport {
  std::vector<std::wstring> out;
  std::wstring error;
};

// The report of a change of the check task that was not refused for its context or busy (the
// command says why then): after a change, the checkboxes as the task has them now and where the
// task is; for an install that cannot run the task, or a change that failed, why. May throw
// std::bad_alloc.
ScheduleReport ScheduleResultReport(const ScheduleResult& result);

// Another account's tasks hold every name the user's check task may have (kTaskTakenHr): what
// that means, as words that can end a sentence (pure). May throw std::bad_alloc.
std::wstring TaskTakenText();

// ... and the way out, which only an administrator has: deleting the tasks at |location|'s names
// in Task Scheduler, or uninstalling for all users, which deletes every user's check task (pure).
// One sentence without a final period. May throw std::bad_alloc.
std::wstring TaskTakenHint(const TaskLocation& location);

// What the administrator step |verb| would do, as the end of "The machine-wide part needs an
// administrator to ..." (pure). May throw std::bad_alloc.
std::wstring ElevatedVerbText(ElevatedVerb verb);

// "x64", "arm64", "other".
const wchar_t* ArchName(Arch arch) noexcept;

// "30 Oct 2026 01:26" (pure). May throw std::bad_alloc.
std::wstring ShortDateTimeText(const SYSTEMTIME& time);

// A UTC time in this PC's time zone, as ShortDateTimeText shows it; "" when it cannot be
// converted. May throw std::bad_alloc.
std::wstring LocalTimeText(const FILETIME& utc);

// A Task Scheduler time (an OLE date in local time), as ShortDateTimeText shows it; "" when it
// is not a valid date. May throw std::bad_alloc.
std::wstring TaskTimeText(DATE local);

}  // namespace pdft
