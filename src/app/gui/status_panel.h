// The settings window's status block (pure but for the time zone used to show times): three lines
// and the Fix link, from a snapshot and its findings.

#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "check.h"
#include "check_schedule.h"
#include "evaluate.h"
#include "snapshot.h"
#include "zoom_change.h"

namespace pdft {

enum class StatusGlyph { kNone, kOk, kWarning, kProblem };

// What the Fix link offers.
enum class FixLink {
  kNone,
  kFix,            // the repairs a check makes
  kFixNeedsAdmin,  // ... and the part for all users, through an administrator prompt
  kTakeBack,       // take the .pdf slots back from another handler (asks first)
};

struct StatusPanelLine {
  StatusGlyph glyph = StatusGlyph::kNone;
  std::wstring text;
};

struct StatusPanel {
  // [0] whether thumbnails work, [1] the last check and PdfCraft, [2] Smart App Control when it
  // has something to say, and the version.
  std::array<StatusPanelLine, 3> lines;
  FixLink fix = FixLink::kNone;
  // What the user can do about the first problem, when the link cannot do it (else empty).
  std::wstring hint;
  // What taking the slots back would replace, for the question before it (kTakeBack).
  std::vector<std::wstring> takeBack;
};

// May throw std::bad_alloc.
StatusPanel BuildStatusPanel(const Snapshot& snapshot, std::span<const Finding> findings);

// What the window says when a check or fix ended (pure); empty when |result| found a lock taken,
// which the banner says. May throw std::bad_alloc.
std::wstring CheckOutcomeText(const CheckResult& result);

// What Save did, part by part.
struct SaveOutcome {
  bool checksAsked = false;
  HRESULT checksHr = S_OK;
  ScheduleResult checks;
  bool zoomAsked = false;
  int zoom = 100;
  HRESULT zoomHr = S_OK;
  ZoomChangeResult zoomResult;
};

struct SaveReport {
  std::wstring note;     // the one line the status block shows
  std::wstring problem;  // what went wrong, for a dialog; empty when nothing did
  bool busy = false;     // another process holds the lock: what was asked stays unsaved
};

// What to say about |outcome| (pure). |refreshDefault| is kPdfRefreshDefault. May throw
// std::bad_alloc.
SaveReport DescribeSave(const SaveOutcome& outcome, bool refreshDefault);

}  // namespace pdft
