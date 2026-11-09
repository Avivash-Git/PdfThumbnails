// The settings window's status block (status_panel.h) on made-up snapshots.

#include "status_panel.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "commands.h"
#include "snapshot_fixtures.h"
#include "status_format.h"

namespace pdft {
namespace {

using namespace test;  // NOLINT(build/namespaces): the fixtures

StatusPanel Panel(const Snapshot& s) { return BuildStatusPanel(s, Evaluate(s)); }

bool Has(const std::wstring& text, const std::wstring& part) {
  return text.find(part) != std::wstring::npos;
}

TEST(StatusPanelLines, AHealthyInstallSaysSo) {
  const StatusPanel panel = Panel(MachineInstallSnapshot());
  EXPECT_EQ(panel.lines[0].glyph, StatusGlyph::kOk);
  EXPECT_TRUE(Has(panel.lines[0].text, L"PDF thumbnails are working")) << panel.lines[0].text;
  EXPECT_TRUE(Has(panel.lines[0].text, L"all users"));
  EXPECT_EQ(panel.fix, FixLink::kNone);
  EXPECT_TRUE(panel.hint.empty());
  EXPECT_TRUE(Has(panel.lines[2].text, VersionText())) << panel.lines[2].text;
}

TEST(StatusPanelLines, NothingWrongIsNotTheSameAsTested) {
  Snapshot s = MachineInstallSnapshot();
  s.live = std::nullopt;
  s.liveSkip = LiveTestSkip::kNotRequested;
  const StatusPanel panel = Panel(s);
  EXPECT_EQ(panel.lines[0].glyph, StatusGlyph::kNone);
  EXPECT_TRUE(Has(panel.lines[0].text, L"Not tested yet")) << panel.lines[0].text;
  EXPECT_FALSE(Has(panel.lines[0].text, L"working"));
}

TEST(StatusPanelLines, TheLastCheckCountsAsATest) {
  Snapshot s = MachineInstallSnapshot();
  s.live = std::nullopt;
  s.liveSkip = LiveTestSkip::kNotRequested;
  CheckRecord record;
  record.time = {0, 0x01DD0000};
  record.liveTest = S_OK;
  s.last = record;
  EXPECT_TRUE(Has(Panel(s).lines[0].text, L"PDF thumbnails are working"));
  s.last->liveTest = E_FAIL;
  EXPECT_TRUE(Has(Panel(s).lines[0].text, L"Not tested yet"));
  s.last->liveTest = std::nullopt;
  EXPECT_TRUE(Has(Panel(s).lines[0].text, L"Not tested yet"));
}

TEST(StatusPanelLines, APerUserInstallSaysSo) {
  EXPECT_TRUE(Has(Panel(UserInstallSnapshot()).lines[0].text, L"this user"));
}

TEST(StatusPanelLines, NoRecordMeansNoCheckYet) {
  const StatusPanel panel = Panel(MachineInstallSnapshot());
  EXPECT_EQ(panel.lines[1].glyph, StatusGlyph::kNone);
  EXPECT_TRUE(Has(panel.lines[1].text, L"No check has run yet")) << panel.lines[1].text;
}

TEST(StatusPanelLines, TheLastCheckAndPdfCraftShow) {
  Snapshot s = MachineInstallSnapshot();
  CheckRecord record;
  record.time = {0, 0x01DD0000};
  record.result = ExitCode::kOk;
  record.summary = L"nothing to fix";
  s.last = record;
  s.pdfcraft.installed = true;
  s.pdfcraft.version = L"0.14.0";
  const StatusPanel panel = Panel(s);
  EXPECT_EQ(panel.lines[1].glyph, StatusGlyph::kOk);
  EXPECT_TRUE(Has(panel.lines[1].text, L"nothing to fix")) << panel.lines[1].text;
  EXPECT_TRUE(Has(panel.lines[1].text, std::wstring(kPdfCraftProductName) + L" 0.14.0"));
}

TEST(StatusPanelLines, ACheckThatFoundProblemsIsAWarning) {
  Snapshot s = MachineInstallSnapshot();
  CheckRecord record;
  record.time = {0, 0x01DD0000};
  record.result = ExitCode::kProblems;
  s.last = record;
  EXPECT_EQ(Panel(s).lines[1].glyph, StatusGlyph::kWarning);
}

TEST(StatusPanelLines, SmartAppControlShowsOnlyWithAFinding) {
  const Snapshot thisPc = ThisPcSnapshot();
  const std::vector<Finding> findings = Evaluate(thisPc);
  ASSERT_TRUE(HasFinding(findings, FindingId::kSacOn));
  for (const Finding& finding : findings) {
    if (finding.id != FindingId::kSacOn) continue;
    EXPECT_TRUE(
        Has(BuildStatusPanel(thisPc, findings).lines[2].text, FindingText(thisPc, finding)));
  }
  EXPECT_FALSE(Has(Panel(MachineInstallSnapshot()).lines[2].text, L"Smart App Control"));
}

TEST(StatusPanelFix, AnEmptySlotIsRepairedByACheck) {
  Snapshot s = MachineInstallSnapshot();
  s.slots[0] = std::nullopt;
  const StatusPanel panel = Panel(s);
  EXPECT_EQ(panel.lines[0].glyph, StatusGlyph::kProblem);
  EXPECT_EQ(panel.fix, FixLink::kFix);
  EXPECT_TRUE(panel.hint.empty());
}

TEST(StatusPanelFix, AnotherHandlersSlotIsTakenBackOnlyOnRequest) {
  Snapshot s = MachineInstallSnapshot();
  s.slots[0] = kOther;
  const StatusPanel panel = Panel(s);
  EXPECT_EQ(panel.fix, FixLink::kTakeBack);
  EXPECT_EQ(panel.takeBack.size(), 1u);
}

TEST(SaveReportText, ASavedCheckBoxSaysSaved) {
  SaveOutcome outcome;
  outcome.checksAsked = true;
  outcome.checks.code = ExitCode::kOk;
  const SaveReport report = DescribeSave(outcome, true);
  EXPECT_EQ(report.note, L"Saved.");
  EXPECT_TRUE(report.problem.empty());
  EXPECT_FALSE(report.busy);
}

TEST(SaveReportText, ARefreshReportsWhatItRefreshed) {
  SaveOutcome outcome;
  outcome.zoomAsked = true;
  outcome.zoom = 130;
  outcome.zoomResult.saved = true;
  outcome.zoomResult.refreshRan = true;
  outcome.zoomResult.counts.refreshed = 93;
  outcome.zoomResult.counts.filesRefreshed = 54;
  EXPECT_EQ(DescribeSave(outcome, true).note, ZoomRefreshedText(130, 93, 54));
}

TEST(SaveReportText, AStoppedRefreshKeepsTheZoom) {
  SaveOutcome outcome;
  outcome.zoomAsked = true;
  outcome.zoom = 130;
  outcome.zoomResult.refreshRan = true;
  outcome.zoomResult.refreshResult = HRESULT_FROM_WIN32(ERROR_CANCELLED);
  outcome.zoomResult.counts.stopped = true;
  outcome.zoomResult.counts.refreshed = 3;
  EXPECT_EQ(DescribeSave(outcome, true).note, ZoomRefreshStoppedText(130, 3));
}

TEST(SaveReportText, ARefusedRefreshStillSavedTheZoom) {
  SaveOutcome outcome;
  outcome.zoomAsked = true;
  outcome.zoom = 150;
  outcome.zoomResult.refusal = RefreshRefusal::kThumbnailsOff;
  EXPECT_EQ(DescribeSave(outcome, true).note, ZoomSavedText(150, true));
}

TEST(SaveReportText, ATakenLockIsBusyAndNotAProblem) {
  SaveOutcome outcome;
  outcome.zoomAsked = true;
  outcome.zoomResult.busy = true;
  const SaveReport report = DescribeSave(outcome, true);
  EXPECT_TRUE(report.busy);
  EXPECT_TRUE(report.problem.empty());
  EXPECT_TRUE(report.note.empty());
}

TEST(SaveReportText, EachFailedPartIsSaid) {
  SaveOutcome outcome;
  outcome.checksAsked = true;
  outcome.checksHr = E_ACCESSDENIED;
  outcome.zoomAsked = true;
  outcome.zoomResult.saveError = E_FAIL;
  const SaveReport report = DescribeSave(outcome, true);
  EXPECT_TRUE(Has(report.problem, L"when the checks run"));
  EXPECT_TRUE(Has(report.problem, L"Cannot save the zoom"));
}

TEST(CheckOutcome, SaysWhatTheCheckFound) {
  CheckResult result;
  result.code = ExitCode::kOk;
  result.summary = L"nothing to fix";
  EXPECT_EQ(CheckOutcomeText(result), L"nothing to fix");
  Finding changed;
  changed.id = FindingId::kPdfCraftChanged;
  changed.from = L"0.14.0";
  changed.to = L"0.15.0";
  result.findings.push_back(changed);
  EXPECT_EQ(CheckOutcomeText(result),
            std::wstring(kPdfCraftProductName) + L" 0.14.0 \u2192 0.15.0 detected; nothing to fix");
}

TEST(CheckOutcome, ABusyLockIsTheBannersToSay) {
  CheckResult result;
  result.busy = true;
  EXPECT_TRUE(CheckOutcomeText(result).empty());
}

TEST(CheckOutcome, ADeclinedPromptIsSaid) {
  CheckResult result;
  result.code = ExitCode::kCancelled;
  EXPECT_TRUE(Has(CheckOutcomeText(result), L"declined"));
}

TEST(CheckOutcome, AnAdministratorStepThatCannotRunIsSaid) {
  CheckResult result;
  result.code = ExitCode::kProblems;
  result.adminStepNeeded = true;
  result.adminStepUnavailable = true;
  EXPECT_TRUE(Has(CheckOutcomeText(result), L"no settings program is installed"));
}

TEST(StatusPanelFix, TheAdministratorPartWins) {
  Snapshot s = MachineInstallSnapshot();
  s.machineClass->dllExists = false;
  s.slots[0] = std::nullopt;
  EXPECT_EQ(Panel(s).fix, FixLink::kFixNeedsAdmin);
}

TEST(StatusPanelFix, WhatOnlyTheUserCanDoIsAHint) {
  const StatusPanel panel = Panel(EmptySnapshot());
  EXPECT_EQ(panel.lines[0].glyph, StatusGlyph::kProblem);
  EXPECT_EQ(panel.fix, FixLink::kNone);
  EXPECT_FALSE(panel.hint.empty());
}

}  // namespace
}  // namespace pdft
