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
  EXPECT_EQ(Panel(s).fix, FixLink::kTakeBack);
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
