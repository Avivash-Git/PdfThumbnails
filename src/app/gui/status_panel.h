// The settings window's status block (pure but for the time zone used to show times): three lines
// and the Fix link, from a snapshot and its findings.

#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "check.h"
#include "evaluate.h"
#include "snapshot.h"

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

}  // namespace pdft
