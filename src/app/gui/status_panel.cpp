#include "status_panel.h"

#include <algorithm>
#include <memory>

#include "commands.h"
#include "project_config.h"
#include "status_format.h"

namespace pdft {
namespace {

constexpr wchar_t kSeparator[] = L" · ";

bool IsSacFinding(FindingId id) noexcept {
  return id == FindingId::kSacBlocked || id == FindingId::kSacEvaluation ||
         id == FindingId::kSacOn || id == FindingId::kSacUnknown;
}

// A finding about this process, which the window's banner already says.
bool IsAboutThisProcess(FindingId id) noexcept {
  return id == FindingId::kElevated || id == FindingId::kDifferentUser ||
         id == FindingId::kVirtualized;
}

// Whether File Explorer's way of making a thumbnail was seen to work: in this reading, or by the
// check recorded last.
bool ExplorerTestPassed(const Snapshot& snapshot) noexcept {
  if (snapshot.live) return snapshot.live->hr == S_OK;
  return snapshot.last && snapshot.last->liveTest && *snapshot.last->liveTest == S_OK;
}

StatusPanelLine ThumbnailsLine(const Snapshot& snapshot, const Finding* problem) {
  if (problem) return {StatusGlyph::kProblem, FindingText(snapshot, *problem)};
  const std::wstring scope =
      snapshot.scope == Scope::kUser ? L"installed for this user" : L"installed for all users";
  // Nothing wrong was found, but only the Explorer test shows that thumbnails are really made.
  if (!ExplorerTestPassed(snapshot))
    return {StatusGlyph::kNone, L"Not tested yet (" + scope + L")"};
  return {StatusGlyph::kOk, L"PDF thumbnails are working (" + scope + L")"};
}

StatusPanelLine LastCheckLine(const Snapshot& snapshot) {
  StatusPanelLine line;
  if (snapshot.last) {
    line.glyph = snapshot.last->result == ExitCode::kOk ? StatusGlyph::kOk : StatusGlyph::kWarning;
    line.text = L"Last check: " + LocalTimeText(snapshot.last->time);
    if (!snapshot.last->summary.empty()) line.text += L", " + snapshot.last->summary;
  } else {
    line.text = L"No check has run yet";
  }
  if (snapshot.pdfcraft.installed && !snapshot.pdfcraft.version.empty()) {
    line.text += kSeparator + std::wstring(kPdfCraftProductName) + L" " + snapshot.pdfcraft.version;
  }
  return line;
}

StatusPanelLine VersionLine(const Snapshot& snapshot, std::span<const Finding> findings) {
  StatusPanelLine line;
  const auto sac = std::find_if(findings.begin(), findings.end(),
                                [](const Finding& f) { return IsSacFinding(f.id); });
  if (sac != findings.end()) {
    line.text = FindingText(snapshot, *sac) + kSeparator;
    line.glyph = sac->severity == Severity::kError     ? StatusGlyph::kProblem
                 : sac->severity == Severity::kWarning ? StatusGlyph::kWarning
                                                       : StatusGlyph::kNone;
  }
  line.text += VersionText();
  return line;
}

FixLink ChooseFixLink(std::span<const Finding> findings) noexcept {
  bool automatic = false;
  bool takeBack = false;
  for (const Finding& finding : findings) {
    if (finding.severity == Severity::kInfo || IsAboutThisProcess(finding.id)) continue;
    if (finding.fix == FixKind::kNeedsAdmin) return FixLink::kFixNeedsAdmin;
    automatic = automatic || finding.fix == FixKind::kAuto;
    takeBack = takeBack || finding.fix == FixKind::kTakeBack;
  }
  if (automatic) return FixLink::kFix;
  return takeBack ? FixLink::kTakeBack : FixLink::kNone;
}

}  // namespace

std::wstring CheckOutcomeText(const CheckResult& result) {
  if (result.busy) return {};
  if (result.namedObjectsRefused) return NamedObjectsRefusedMessage();
  if (result.refused) return L"The check cannot run here.";
  if (result.code == ExitCode::kCancelled) {
    return L"The administrator prompt was declined; the part for all users was not changed.";
  }
  if (result.adminStepNeeded && result.adminStepUnavailable) {
    return L"The part for all users needs an administrator, and no settings program is installed "
           L"for all users to do it.";
  }
  std::wstring text;
  for (const Finding& finding : result.findings) {
    if (finding.id == FindingId::kPdfCraftChanged) {
      text = std::wstring(kPdfCraftProductName) + L" " + finding.from + L" \u2192 " + finding.to +
             L" detected; ";
    }
  }
  return text + result.summary;
}

StatusPanel BuildStatusPanel(const Snapshot& snapshot, std::span<const Finding> findings) {
  const auto first = std::find_if(findings.begin(), findings.end(), [](const Finding& f) {
    return f.breaksThumbnails && !IsAboutThisProcess(f.id);
  });
  const Finding* problem = first != findings.end() ? std::addressof(*first) : nullptr;

  StatusPanel panel;
  panel.lines[0] = ThumbnailsLine(snapshot, problem);
  panel.lines[1] = LastCheckLine(snapshot);
  panel.lines[2] = VersionLine(snapshot, findings);
  panel.fix = ChooseFixLink(findings);
  for (const Finding& finding : findings) {
    if (finding.id == FindingId::kSlotOtherHandler) {
      panel.takeBack.push_back(FindingText(snapshot, finding));
    }
  }
  if (problem && (problem->fix == FixKind::kUserAction || problem->fix == FixKind::kNone)) {
    panel.hint = FindingHint(snapshot, *problem);
  }
  return panel;
}

}  // namespace pdft
