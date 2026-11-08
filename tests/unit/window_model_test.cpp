// The settings window's rules (window_model.h).

#include "window_model.h"

#include <gtest/gtest.h>

namespace pdft {
namespace {

WindowState Ready(WindowActivity activity = WindowActivity::kIdle, bool dirty = false) {
  WindowState state;
  state.context = WindowContext::kReady;
  state.activity = activity;
  state.dirty = dirty;
  return state;
}

TEST(WindowContextRule, WritesThatGetLostComeFirst) {
  EXPECT_EQ(ClassifyWindowContext(true, true), WindowContext::kSandboxed);
  EXPECT_EQ(ClassifyWindowContext(true, false), WindowContext::kSandboxed);
  EXPECT_EQ(ClassifyWindowContext(false, true), WindowContext::kReady);
  EXPECT_EQ(ClassifyWindowContext(false, false), WindowContext::kNotInstalled);
}

TEST(WindowControls, ReadyAndIdleAllowsEverythingButACleanSave) {
  const ControlStates c = DecideControls(Ready());
  EXPECT_TRUE(c.zoomAndChecks);
  EXPECT_TRUE(c.checkNowAndFix);
  EXPECT_TRUE(c.rebuild);
  EXPECT_TRUE(c.uninstall);
  EXPECT_TRUE(c.openLogAndDiagnostics);
  EXPECT_FALSE(c.save);
  EXPECT_FALSE(c.cancelLink);
}

TEST(WindowControls, SaveNeedsSomethingToSave) {
  EXPECT_TRUE(DecideControls(Ready(WindowActivity::kIdle, true)).save);
}

TEST(WindowControls, OnlyEditingAndTheReadOnlyButtonsWorkWhileGathering) {
  const ControlStates c = DecideControls(Ready(WindowActivity::kGathering, true));
  EXPECT_TRUE(c.zoomAndChecks);
  EXPECT_FALSE(c.checkNowAndFix);
  EXPECT_FALSE(c.rebuild);
  EXPECT_FALSE(c.save);
  EXPECT_TRUE(c.openLogAndDiagnostics);
}

TEST(WindowControls, WorkKeepsTheOtherOperationsOff) {
  for (const WindowActivity activity :
       {WindowActivity::kChecking, WindowActivity::kFixing, WindowActivity::kSaving,
        WindowActivity::kRefreshing, WindowActivity::kStopping, WindowActivity::kChild,
        WindowActivity::kClosing}) {
    const ControlStates c = DecideControls(Ready(activity, true));
    EXPECT_FALSE(c.zoomAndChecks);
    EXPECT_FALSE(c.checkNowAndFix);
    EXPECT_FALSE(c.rebuild);
    EXPECT_FALSE(c.uninstall);
    EXPECT_FALSE(c.save);
    EXPECT_TRUE(c.openLogAndDiagnostics);
  }
}

TEST(WindowControls, OnlyARefreshCanBeCancelled) {
  EXPECT_TRUE(DecideControls(Ready(WindowActivity::kRefreshing)).cancelLink);
  EXPECT_FALSE(DecideControls(Ready(WindowActivity::kStopping)).cancelLink);
  EXPECT_FALSE(DecideControls(Ready(WindowActivity::kChild)).cancelLink);
}

TEST(WindowControls, ABusyLockKeepsEditingButNotSaving) {
  const ControlStates c = DecideControls(Ready(WindowActivity::kExternalBusy, true));
  EXPECT_TRUE(c.zoomAndChecks);
  EXPECT_FALSE(c.save);
  EXPECT_FALSE(c.checkNowAndFix);
}

TEST(WindowControls, AnotherAccountCannotRebuildOrUninstall) {
  WindowState state = Ready(WindowActivity::kIdle, true);
  state.otherUser = true;
  const ControlStates c = DecideControls(state);
  EXPECT_FALSE(c.rebuild);
  EXPECT_FALSE(c.uninstall);
  EXPECT_TRUE(c.zoomAndChecks);
  EXPECT_TRUE(c.checkNowAndFix);
  EXPECT_TRUE(c.save);
}

TEST(WindowControls, NotInstalledOrSandboxedChangesNothing) {
  for (const WindowContext context : {WindowContext::kNotInstalled, WindowContext::kSandboxed}) {
    WindowState state = Ready(WindowActivity::kIdle, true);
    state.context = context;
    const ControlStates c = DecideControls(state);
    EXPECT_FALSE(c.zoomAndChecks);
    EXPECT_FALSE(c.checkNowAndFix);
    EXPECT_FALSE(c.rebuild);
    EXPECT_FALSE(c.uninstall);
    EXPECT_FALSE(c.save);
    EXPECT_TRUE(c.openLogAndDiagnostics);
  }
}

TEST(WindowControls, NothingBeforeTheFirstReading) {
  WindowState state;
  const ControlStates c = DecideControls(state);
  EXPECT_FALSE(c.openLogAndDiagnostics);
  EXPECT_FALSE(c.zoomAndChecks);
}

TEST(WindowClose, ClosesAtOnceWhenNothingIsOpen) {
  EXPECT_EQ(DecideClose(Ready()), CloseAction::kCloseNow);
  EXPECT_EQ(DecideClose(Ready(WindowActivity::kExternalBusy)), CloseAction::kCloseNow);
}

TEST(WindowClose, UnsavedChangesAreAskedAbout) {
  EXPECT_EQ(DecideClose(Ready(WindowActivity::kIdle, true)), CloseAction::kAskSaveChanges);
  EXPECT_EQ(DecideClose(Ready(WindowActivity::kGathering, true)), CloseAction::kAskSaveChanges);
  EXPECT_EQ(DecideClose(Ready(WindowActivity::kExternalBusy, true)), CloseAction::kAskSaveChanges);
}

TEST(WindowClose, ARefreshIsAskedAboutAndAChildGoesOn) {
  EXPECT_EQ(DecideClose(Ready(WindowActivity::kRefreshing, true)), CloseAction::kAskStopRefresh);
  EXPECT_EQ(DecideClose(Ready(WindowActivity::kChild, true)), CloseAction::kCloseNow);
}

TEST(WindowClose, OtherWorkIsWaitedFor) {
  for (const WindowActivity activity :
       {WindowActivity::kChecking, WindowActivity::kFixing, WindowActivity::kSaving,
        WindowActivity::kStopping, WindowActivity::kClosing, WindowActivity::kGathering}) {
    EXPECT_EQ(DecideClose(Ready(activity)), CloseAction::kWaitForWork);
  }
}

TEST(WindowBannerRule, TheMostImportantOneShows) {
  WindowState state = Ready(WindowActivity::kExternalBusy);
  state.otherUser = true;
  state.context = WindowContext::kSandboxed;
  EXPECT_EQ(DecideBanner(state), WindowBanner::kSandboxed);
  state.context = WindowContext::kNotInstalled;
  EXPECT_EQ(DecideBanner(state), WindowBanner::kOtherUser);
  state.otherUser = false;
  EXPECT_EQ(DecideBanner(state), WindowBanner::kExternalBusy);
  state.activity = WindowActivity::kIdle;
  EXPECT_EQ(DecideBanner(state), WindowBanner::kNotInstalled);
  EXPECT_EQ(DecideBanner(Ready()), WindowBanner::kNone);
}

TEST(WindowZoomBox, ClampsANumberAndKeepsTheZoomOtherwise) {
  EXPECT_EQ(ZoomFromBox(L"130", 100), 130);
  EXPECT_EQ(ZoomFromBox(L"50", 130), 100);
  EXPECT_EQ(ZoomFromBox(L"999", 130), 200);
  EXPECT_EQ(ZoomFromBox(L"99999999999999999999", 130), 200);
  EXPECT_EQ(ZoomFromBox(L"", 130), 130);
  EXPECT_EQ(ZoomFromBox(L"1a", 130), 130);
}

TEST(WindowZoom, TicksIncludeTheFirstSquareZoom) {
  EXPECT_EQ(kZoomTicks.front(), 100);
  EXPECT_EQ(kZoomTicks[2], 142);
  EXPECT_EQ(kZoomTicks.back(), 200);
}

TEST(WindowPreview, TheEdgeFollowsTheDpiWithinTheChildsLimits) {
  EXPECT_EQ(PreviewEdgeForDpi(96), 96);
  EXPECT_EQ(PreviewEdgeForDpi(144), 144);
  EXPECT_EQ(PreviewEdgeForDpi(192), 192);
  EXPECT_EQ(PreviewEdgeForDpi(1000), 512);
}

TEST(WindowChild, TheExitCodePicksTheIcon) {
  EXPECT_EQ(OutcomeOfChild(0), ChildOutcome::kInformation);
  EXPECT_EQ(OutcomeOfChild(3), ChildOutcome::kInformation);
  EXPECT_EQ(OutcomeOfChild(1), ChildOutcome::kWarning);
  for (const DWORD code : {2u, 4u, 5u, 6u, 7u, 8u, 0xC0000005u}) {
    EXPECT_EQ(OutcomeOfChild(code), ChildOutcome::kError) << code;
  }
}

}  // namespace
}  // namespace pdft
