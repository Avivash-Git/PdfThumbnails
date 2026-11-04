// The thumbnail-cache rebuild (rebuild.h). Most of it runs against the fake RebuildSystem
// (fake_rebuild.h), which touches no real process, file, window or clock, so the whole flow is
// exercised safely and the waits are exact: the checks before anything changes (where Windows
// does not restart File Explorer, the start made ready and tried first), ending processes in
// order, deleting, waiting for File Explorer (and starting it where Windows will not), and opening
// the folder windows again. Two pieces touch the real system but only read or use a temp folder:
// listing the cache files of a folder this test makes, and the images the rebuild may end.
// Nothing here ends a real process, deletes a real cache file or restarts File Explorer.
//
// The RefreshProbe tests are disabled: they only read the real system (who holds the real cache
// files, the folder windows, whether the Explorer test leaves this process holding the cache) and
// print counts and image leaf names, for the system-test runner. Run them with
// --gtest_also_run_disabled_tests --gtest_filter=RefreshProbe.*.

#include <windows.h>

#include "rebuild.h"

#include <gtest/gtest.h>
#include <wil/resource.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include "commands.h"
#include "fake_rebuild.h"
#include "live_test.h"
#include "log.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

using std::filesystem::path;
using test::FakePid;
using test::FakeRebuildSystem;

// The fake's processes.
const DWORD kShellPid = FakePid(50);
const DWORD kExplorerPid = FakePid(75);
const DWORD kSurrogatePid = FakePid(100);
const DWORD kSearchPid = FakePid(225);

ProcessContext CleanContext() {
  ProcessContext context;
  context.userAccount = L"PC\\tester";
  context.probe = ContextProbe::kWrite;
  context.virtualization = Virtualization::kNo;
  return context;
}

RebuildOptions Options(ProcessContext context = CleanContext()) {
  RebuildOptions options;
  options.context = std::move(context);
  options.shellWaitMs = 5000;
  return options;
}

std::vector<path> Files(const path& dir, int count) {
  std::vector<path> files;
  for (int i = 0; i < count; ++i)
    files.push_back(dir / (L"thumbcache_" + std::to_wstring(i) + L".db"));
  return files;
}

FolderWindow Window(std::wstring name, ULONG_PTR frame) {
  FolderWindow window;
  window.parsingName = std::move(name);
  window.frame = frame;
  return window;
}

HRESULT RunRebuild(const FakeRebuildSystem& system, const RebuildOptions& options,
                   RebuildResult* result, std::stop_token stop = std::stop_token()) {
  return RebuildThumbnailCache(options, system, {}, stop, result);
}

// Two cache files, the first held by the shell.
void ShellHoldsTheCache(FakeRebuildSystem* fake) {
  fake->cacheFiles = Files(L"C:\\fake-cache", 2);
  fake->SetShell(kShellPid);
  fake->AddHolder(kShellPid, fake->kExplorerImage(), {fake->cacheFiles[0]});
}

bool HasLineStarting(const std::vector<std::wstring>& lines, std::wstring_view start) {
  return std::any_of(lines.begin(), lines.end(),
                     [start](const std::wstring& line) { return line.starts_with(start); });
}

// ---- the whole flow against the fake ----

TEST(RebuildFake, HappyPath) {
  const path dir = L"C:\\fake-cache";
  FakeRebuildSystem fake;
  fake.cacheFiles = Files(dir, 15);
  fake.SetShell(kShellPid);
  fake.AddHolder(kSurrogatePid, fake.kSurrogateImage(), {fake.cacheFiles[10]});
  fake.AddHolder(kExplorerPid, fake.kExplorerImage(), {fake.cacheFiles[5]});
  fake.AddHolder(kShellPid, fake.kExplorerImage(), {fake.cacheFiles[0]});
  fake.windowsBefore = {Window(L"C:\\A", 1), Window(L"C:\\B", 2), Window(L"C:\\C", 3)};
  fake.windowsAfter = {Window(L"C:\\A", 9)};  // one survived the restart
  fake.shellBackByItself = true;
  fake.shellBackAfterMs = 1200;
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
  // The thumbnail process first, then the other File Explorer, then the shell.
  EXPECT_EQ(fake.Ended(), (std::vector<DWORD>{kSurrogatePid, kExplorerPid, kShellPid}));
  EXPECT_EQ(result.ended, 3u);
  EXPECT_EQ(result.deleted, 15u);
  EXPECT_EQ(result.shell, ShellOutcome::kBack);
  // Counted from when the shell had gone.
  EXPECT_EQ(result.shellMs, 1200u);
  EXPECT_EQ(fake.startCalls, 0);
  EXPECT_EQ(result.stillOpen, 1u);
  EXPECT_EQ(result.reopened, 2u);
  EXPECT_EQ(result.reopenFailed, 0u);
  EXPECT_TRUE(HasLineStarting(RebuildReport(result), L"Reopened 2 of 2 folder windows."));
}

TEST(RebuildFake, SearchHostKeepsAFileInUse) {
  const path dir = L"C:\\fake-cache";
  FakeRebuildSystem fake;
  fake.cacheFiles = Files(dir, 4);
  fake.SetShell(kShellPid);
  fake.AddHolder(kShellPid, fake.kExplorerImage(), {fake.cacheFiles[0]});
  // A program left alone keeps one file; it is named in the result, never ended.
  fake.AddHolder(kSearchPid, L"C:\\Windows\\SystemApps\\Search\\SearchHost.exe",
                 {fake.cacheFiles[3]});
  fake.inUseFiles.insert(fake.cacheFiles[3].native());
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
  EXPECT_EQ(result.deleted, 3u);
  EXPECT_EQ(result.inUse, 1u);
  ASSERT_EQ(result.inUseBy.size(), 1u);
  EXPECT_EQ(result.inUseBy[0], L"SearchHost.exe");
  EXPECT_EQ(fake.Ended(), (std::vector<DWORD>{kShellPid}));  // SearchHost left alone
}

TEST(RebuildFake, RetriesAFileThatComesFreeAfterAProcessEnds) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.freeAfterRetries[fake.cacheFiles[1].native()] = 2;  // free on the third attempt
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(result.deleted, 2u);
  EXPECT_EQ(result.inUse, 0u);
  EXPECT_EQ(result.retried, 2u);
}

TEST(RebuildFake, CancelledBeforeAnythingChanges) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.Prime();

  std::stop_source stop;
  stop.request_stop();
  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result, stop.get_token()));
  EXPECT_EQ(result.refusal, RebuildRefusal::kCancelled);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kCancelled);
  EXPECT_FALSE(result.committed);
  EXPECT_TRUE(fake.Ended().empty());
  EXPECT_TRUE(fake.Deleted().empty());
}

// A stop that comes while the windows are listed, or while the copy check runs, still stops it.
TEST(RebuildFake, CancelledWhileListingOrChecking) {
  for (const FakeRebuildSystem::StopIn where :
       {FakeRebuildSystem::StopIn::kListing, FakeRebuildSystem::StopIn::kFileOperationCheck}) {
    FakeRebuildSystem fake;
    ShellHoldsTheCache(&fake);
    fake.windowsBefore = {Window(L"C:\\A", 1)};
    std::stop_source stop;
    fake.stop = &stop;
    fake.stopIn = where;
    fake.Prime();

    RebuildResult result;
    ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result, stop.get_token()));
    EXPECT_TRUE(stop.stop_requested());
    EXPECT_EQ(result.refusal, RebuildRefusal::kCancelled);
    EXPECT_EQ(RebuildExitCode(result), ExitCode::kCancelled);
    EXPECT_FALSE(result.committed);
    EXPECT_TRUE(fake.Ended().empty());
    EXPECT_TRUE(fake.Deleted().empty());
  }
}

// Past the point of no return a stop changes nothing: every file goes, the shell is waited for,
// and the windows are opened again.
TEST(RebuildFake, AStopPastThePointOfNoReturnIsIgnored) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.windowsBefore = {Window(L"C:\\A", 1)};
  fake.shellBackAfterMs = 300;
  std::stop_source stop;
  fake.stop = &stop;
  fake.stopIn = FakeRebuildSystem::StopIn::kEnding;
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result, stop.get_token()));
  EXPECT_TRUE(stop.stop_requested());
  EXPECT_TRUE(result.committed);
  EXPECT_EQ(result.refusal, RebuildRefusal::kNone);
  EXPECT_EQ(result.deleted, 2u);
  EXPECT_EQ(result.shell, ShellOutcome::kBack);
  EXPECT_EQ(result.reopened, 1u);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
}

TEST(RebuildFake, RestartManagerFailureStopsBeforeTheCommit) {
  const path dir = L"C:\\fake-cache";
  FakeRebuildSystem fake;
  fake.cacheFiles = Files(dir, 2);
  fake.findHoldersHr = E_FAIL;
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(result.failure, E_FAIL);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kFailed);
  EXPECT_TRUE(fake.Ended().empty());
}

// Reading AutoRestartShell or the images failing is a result too, before anything changes.
TEST(RebuildFake, ReadFailuresStopBeforeTheCommit) {
  FakeRebuildSystem restart;
  ShellHoldsTheCache(&restart);
  restart.autoRestartHr = E_FAIL;
  restart.Prime();
  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(restart, Options(), &result));
  EXPECT_EQ(result.failure, E_FAIL);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kFailed);
  EXPECT_EQ(restart.Calls(), 1);  // nothing after the read

  FakeRebuildSystem images;
  ShellHoldsTheCache(&images);
  images.imagesHr = E_FAIL;
  images.Prime();
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(images, Options(), &result));
  EXPECT_EQ(result.failure, E_FAIL);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kFailed);
  EXPECT_FALSE(result.committed);
  EXPECT_TRUE(images.Ended().empty());
  EXPECT_TRUE(images.Deleted().empty());
}

TEST(RebuildFake, ThisProcessHoldsTheCache) {
  const path dir = L"C:\\fake-cache";
  FakeRebuildSystem fake;
  fake.cacheFiles = Files(dir, 2);
  fake.SetShell(kShellPid);
  fake.AddHolder(GetCurrentProcessId(), fake.kExplorerImage(), {fake.cacheFiles[0]});
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(result.refusal, RebuildRefusal::kOwnProcessHoldsCache);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kFailed);
  EXPECT_TRUE(fake.Ended().empty());
}

TEST(RebuildFake, ACopyInProgressStopsIt) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.fileOperation = true;
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(result.refusal, RebuildRefusal::kFileOperation);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kProblems);
  EXPECT_TRUE(fake.Ended().empty());
}

TEST(RebuildFake, ContextRefusalsTouchNothing) {
  // |calls|: what the engine asked the fake; the context refusals need nothing read.
  const auto scenario = [&](ProcessContext context, bool autoRestart, RebuildRefusal expected,
                            int calls) {
    FakeRebuildSystem fake;
    ShellHoldsTheCache(&fake);
    fake.autoRestart = autoRestart;
    fake.Prime();
    RebuildResult result;
    ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(context), &result));
    EXPECT_EQ(result.refusal, expected);
    EXPECT_EQ(RebuildExitCode(result), ExitCode::kWrongContext);
    EXPECT_FALSE(result.committed);
    EXPECT_EQ(fake.Calls(), calls);
    EXPECT_TRUE(fake.Ended().empty());
    EXPECT_TRUE(fake.Deleted().empty());
  };
  ProcessContext elevated = CleanContext();
  elevated.elevated = true;
  scenario(elevated, true, RebuildRefusal::kElevated, 0);
  ProcessContext virtualized = CleanContext();
  virtualized.virtualization = Virtualization::kYes;
  scenario(virtualized, true, RebuildRefusal::kVirtualized, 0);
  ProcessContext unchecked = CleanContext();
  unchecked.probe = ContextProbe::kNone;
  scenario(unchecked, true, RebuildRefusal::kVirtualized, 0);
  ProcessContext otherUser = CleanContext();
  otherUser.differentFromShellUser = true;
  scenario(otherUser, true, RebuildRefusal::kOtherShellUser, 0);
  // Below a package where Windows does not restart File Explorer: only AutoRestartShell is read.
  ProcessContext packaged = CleanContext();
  packaged.packagedAncestor = L"Some.Terminal_8wekyb3d8bbwe";
  scenario(packaged, false, RebuildRefusal::kNoRestartBelowPackage, 1);
}

TEST(RebuildFake, StartsFileExplorerWhenWindowsDoesNot) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.shellBackByItself = false;   // Windows does not bring it back
  fake.shellBackAfterStart = true;  // but it comes back once this program starts it
  fake.shellBackAfterStartMs = 100;
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(fake.startCalls, 1);
  EXPECT_EQ(result.shell, ShellOutcome::kStarted);
  EXPECT_EQ(result.start, ShellStart::kStarted);
  EXPECT_EQ(result.shellMs, 5100u);  // the wait, then 100 ms after the start
}

TEST(RebuildFake, StartedButNeverBack) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.shellBackByItself = false;
  fake.shellBackAfterStart = false;
  fake.Prime();
  RebuildOptions options = Options();
  options.shellWaitMs = 300;

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, options, &result));
  EXPECT_EQ(fake.startCalls, 1);  // one start only
  EXPECT_EQ(result.shell, ShellOutcome::kNotBack);
  EXPECT_EQ(result.start, ShellStart::kStarted);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kProblems);
}

TEST(RebuildFake, BelowAPackageItDoesNotStartFileExplorer) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.shellBackByItself = false;
  fake.Prime();
  ProcessContext context = CleanContext();
  context.packagedAncestor = L"Some.Terminal_8wekyb3d8bbwe";
  RebuildOptions options = Options(context);
  options.shellWaitMs = 300;

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, options, &result));
  EXPECT_EQ(fake.startCalls, 0);
  EXPECT_EQ(result.shell, ShellOutcome::kNotBack);
  EXPECT_EQ(result.start, ShellStart::kBelowPackage);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kProblems);
}

TEST(RebuildFake, AJobThatForbidsBreakaway) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.shellBackByItself = false;
  fake.startHr = HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  fake.startJobForbids = true;
  fake.Prime();
  RebuildOptions options = Options();
  options.shellWaitMs = 300;

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, options, &result));
  EXPECT_EQ(fake.startCalls, 1);
  EXPECT_EQ(result.shell, ShellOutcome::kNotBack);
  EXPECT_EQ(result.start, ShellStart::kJobForbids);
}

// ---- where Windows does not restart File Explorer: the start is made ready and tried first ----

// AutoRestartShell 0 without a package: before anything changes, the start of File Explorer is
// made ready and tried. One that would stay in a job, or cannot be made, is refused with nothing
// ended or deleted; one that leaves every job is what starts File Explorer right after the shell
// ends.
TEST(RebuildFake, NoRestartChecksTheStartFirst) {
  for (const JobEscape escape : {JobEscape::kStays, JobEscape::kForbidden}) {
    FakeRebuildSystem fake;
    ShellHoldsTheCache(&fake);
    fake.autoRestart = false;
    fake.trialEscape = escape;
    fake.Prime();
    RebuildResult result;
    ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
    EXPECT_EQ(result.refusal, RebuildRefusal::kNoRestartInJob);
    EXPECT_EQ(RebuildExitCode(result), ExitCode::kWrongContext);
    EXPECT_EQ(result.explorerStart, ExplorerStartCheck::kInsideJob);
    ASSERT_TRUE(result.jobEscape);
    EXPECT_EQ(*result.jobEscape, escape);
    EXPECT_EQ(fake.Calls(), 3);  // AutoRestartShell, made ready, tried
    EXPECT_FALSE(result.committed);
    EXPECT_TRUE(fake.Ended().empty());
    EXPECT_TRUE(fake.Deleted().empty());
    EXPECT_EQ(fake.startCalls, 0);
  }

  // The trial fails: this program could not start File Explorer either.
  {
    FakeRebuildSystem fake;
    ShellHoldsTheCache(&fake);
    fake.autoRestart = false;
    fake.trialHr = E_FAIL;
    fake.Prime();
    RebuildResult result;
    ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
    EXPECT_EQ(result.refusal, RebuildRefusal::kNoRestartStartFails);
    EXPECT_EQ(RebuildExitCode(result), ExitCode::kFailed);
    EXPECT_EQ(result.startError, E_FAIL);
    EXPECT_EQ(fake.trialCalls, 1);
    EXPECT_FALSE(result.committed);
    EXPECT_TRUE(fake.Ended().empty());
    EXPECT_TRUE(fake.Deleted().empty());
  }

  // It cannot even be made ready: the same, without a trial.
  {
    FakeRebuildSystem fake;
    ShellHoldsTheCache(&fake);
    fake.autoRestart = false;
    fake.prepareHr = E_FAIL;
    fake.Prime();
    RebuildResult result;
    ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
    EXPECT_EQ(result.refusal, RebuildRefusal::kNoRestartStartFails);
    EXPECT_EQ(RebuildExitCode(result), ExitCode::kFailed);
    EXPECT_EQ(result.startError, E_FAIL);
    EXPECT_EQ(fake.prepareCalls, 1);
    EXPECT_EQ(fake.trialCalls, 0);
    EXPECT_TRUE(fake.Ended().empty());
    EXPECT_TRUE(fake.Deleted().empty());
  }

  // It leaves every job: the rebuild goes on, and File Explorer is started at once.
  {
    FakeRebuildSystem fake;
    ShellHoldsTheCache(&fake);
    fake.autoRestart = false;
    fake.trialEscape = JobEscape::kLeaves;
    fake.shellBackByItself = false;
    fake.shellBackAfterStartMs = 100;
    fake.Prime();
    RebuildResult result;
    ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
    EXPECT_EQ(result.refusal, RebuildRefusal::kNone);
    EXPECT_EQ(result.explorerStart, ExplorerStartCheck::kOutsideJobs);
    EXPECT_TRUE(result.startReady);
    EXPECT_EQ(fake.prepareCalls, 1);
    EXPECT_EQ(fake.trialCalls, 1);
    EXPECT_EQ(fake.startCalls, 1);
    EXPECT_EQ(result.shell, ShellOutcome::kStarted);
    EXPECT_EQ(result.start, ShellStart::kStarted);
    EXPECT_EQ(result.shellMs, 100u);  // no wait for Windows first
    EXPECT_EQ(result.deleted, 2u);
    EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
  }
}

// Where Windows restarts File Explorer by itself, the start is made ready as the fallback, never
// tried.
TEST(RebuildFake, AutoRestartOnNeverTries) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.shellBackAfterMs = 300;
  fake.Prime();
  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
  EXPECT_EQ(fake.prepareCalls, 1);
  EXPECT_EQ(fake.trialCalls, 0);
  EXPECT_EQ(fake.startCalls, 0);
  EXPECT_TRUE(result.startReady);
  EXPECT_EQ(result.explorerStart, ExplorerStartCheck::kNotChecked);
  EXPECT_FALSE(result.jobEscape);
  EXPECT_EQ(result.shell, ShellOutcome::kBack);
}

// A start that could not be made ready is no refusal where Windows restarts File Explorer; if
// Windows does not bring it back, nothing is tried and the report says so.
TEST(RebuildFake, ReadyFailsWithAutoRestartOn) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.prepareHr = E_FAIL;
  fake.shellBackByItself = false;
  fake.Prime();
  RebuildOptions options = Options();
  options.shellWaitMs = 300;
  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, options, &result));
  EXPECT_EQ(result.refusal, RebuildRefusal::kNone);
  EXPECT_TRUE(result.committed);
  EXPECT_FALSE(result.startReady);
  EXPECT_EQ(result.deleted, 2u);
  EXPECT_EQ(result.shell, ShellOutcome::kNotBack);
  EXPECT_EQ(result.start, ShellStart::kFailed);
  EXPECT_EQ(result.startError, E_FAIL);
  EXPECT_EQ(fake.startCalls, 0);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kProblems);
}

// Below a package this program never starts File Explorer, so nothing is made ready.
TEST(RebuildFake, BelowPackageNeverPrepares) {
  for (const bool autoRestart : {true, false}) {
    FakeRebuildSystem fake;
    ShellHoldsTheCache(&fake);
    fake.autoRestart = autoRestart;
    fake.Prime();
    ProcessContext context = CleanContext();
    context.packagedAncestor = L"Some.Terminal_8wekyb3d8bbwe";
    RebuildResult result;
    ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(context), &result));
    EXPECT_EQ(result.refusal,
              autoRestart ? RebuildRefusal::kNone : RebuildRefusal::kNoRestartBelowPackage);
    EXPECT_EQ(fake.prepareCalls, 0);
    EXPECT_EQ(fake.trialCalls, 0);
    EXPECT_FALSE(result.startReady);
  }
}

// The check before the question passed, but the engine's own check, later, fails: the engine
// refuses, and its result names the error of its own trial.
TEST(RebuildFake, TheEngineMayOnlyRefuse) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.autoRestart = false;
  fake.trialFailsFrom = 2;
  fake.trialHr = E_FAIL;
  fake.Prime();
  const ProcessContext context = CleanContext();

  RebuildPrecheck check;
  std::unique_ptr<PreparedExplorerStart> kept;
  ASSERT_HRESULT_SUCCEEDED(PrecheckRebuild(context, fake, &check, &kept));
  EXPECT_EQ(check.refusal, RebuildRefusal::kNone);
  ASSERT_TRUE(check.escape);
  EXPECT_EQ(*check.escape, JobEscape::kLeaves);
  EXPECT_TRUE(kept);  // the start made ready and tried, kept for the run
  EXPECT_EQ(fake.trialCalls, 1);

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(context), &result));
  EXPECT_EQ(fake.trialCalls, 2);
  EXPECT_EQ(result.refusal, RebuildRefusal::kNoRestartStartFails);
  EXPECT_EQ(result.startError, E_FAIL);
  EXPECT_FALSE(result.committed);
  EXPECT_TRUE(fake.Ended().empty());
  const std::wstring message =
      RefreshFullRefusalMessage(result.refusal, context, result.startError);
  EXPECT_NE(message.find(L"(0x80004005)"), std::wstring::npos) << WideToUtf8Lossy(message);
}

// The check alone: the context's refusals read nothing, and nothing is kept after a refusal.
TEST(RebuildFake, ThePrecheckKeepsOnlyAStartItAllows) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  ProcessContext otherUser = CleanContext();
  otherUser.differentFromShellUser = true;
  RebuildPrecheck check;
  std::unique_ptr<PreparedExplorerStart> kept;
  ASSERT_HRESULT_SUCCEEDED(PrecheckRebuild(otherUser, fake, &check, &kept));
  EXPECT_EQ(check.refusal, RebuildRefusal::kOtherShellUser);
  EXPECT_EQ(fake.Calls(), 0);
  EXPECT_FALSE(kept);

  fake.autoRestart = false;
  fake.trialEscape = JobEscape::kStays;
  ASSERT_HRESULT_SUCCEEDED(PrecheckRebuild(CleanContext(), fake, &check, &kept));
  EXPECT_EQ(check.refusal, RebuildRefusal::kNoRestartInJob);
  EXPECT_FALSE(kept);

  // Without |keep|, AutoRestartShell on makes nothing ready.
  fake.autoRestart = true;
  const int prepared = fake.prepareCalls;
  ASSERT_HRESULT_SUCCEEDED(PrecheckRebuild(CleanContext(), fake, &check));
  EXPECT_EQ(check.refusal, RebuildRefusal::kNone);
  EXPECT_EQ(fake.prepareCalls, prepared);

  fake.autoRestartHr = E_ACCESSDENIED;
  ASSERT_HRESULT_SUCCEEDED(PrecheckRebuild(CleanContext(), fake, &check, &kept));
  EXPECT_EQ(check.failure, E_ACCESSDENIED);
  EXPECT_EQ(RebuildExitCode(RebuildNotStarted(check)), ExitCode::kFailed);
  EXPECT_FALSE(kept);
}

// The log names what the trial found and this process's job, never a path.
TEST(RebuildFake, TheLogNamesWhatTheTrialFound) {
  const path dir = L"C:\\very-private-folder\\sub";
  test::TempDir temp(L"rebuild-log");
  ASSERT_TRUE(temp.ok());
  const path log = path(temp.path()) / L"rebuild.log";
  SetLogFileForTesting(log, kLogRotateBytes);
  for (const JobEscape escape : {JobEscape::kLeaves, JobEscape::kStays}) {
    FakeRebuildSystem fake;
    fake.cacheFiles = Files(dir, 2);
    fake.SetShell(kShellPid);
    fake.AddHolder(kShellPid, fake.kExplorerImage(), {fake.cacheFiles[0]});
    fake.autoRestart = false;
    fake.trialEscape = escape;
    fake.shellBackByItself = false;
    fake.Prime();
    RebuildResult result;
    EXPECT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  }
  // A start that could not be made ready: the job was never read.
  {
    FakeRebuildSystem fake;
    fake.cacheFiles = Files(dir, 2);
    fake.SetShell(kShellPid);
    fake.AddHolder(kShellPid, fake.kExplorerImage(), {fake.cacheFiles[0]});
    fake.autoRestart = false;
    fake.prepareHr = E_FAIL;
    fake.Prime();
    RebuildResult result;
    EXPECT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  }
  SetLogFileForTesting({}, 0);

  std::ifstream stream(log, std::ios::binary);
  std::stringstream text;
  text << stream.rdbuf();
  const std::string lines = text.str();
  EXPECT_NE(lines.find("explorer-start=leaves job=none start-ready=yes"), std::string::npos)
      << lines;
  EXPECT_NE(lines.find("refresh full refused=no-restart-in-job explorer-start=stays job=none"),
            std::string::npos)
      << lines;
  EXPECT_NE(lines.find("refresh full refused=no-restart-start-fails start-hr=0x80004005 "
                       "job=not-read"),
            std::string::npos)
      << lines;
  EXPECT_EQ(lines.find("very-private-folder"), std::string::npos) << lines;
  EXPECT_EQ(lines.find("thumbcache_"), std::string::npos) << lines;
}

TEST(RebuildFake, ANewFileExplorerComingUpExtendsTheWait) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.shellBackByItself = true;
  fake.shellBackAfterMs = 300;           // past the first wait
  fake.explorerStartedSinceFlag = true;  // a new File Explorer is already starting
  fake.Prime();
  RebuildOptions options = Options();
  options.shellWaitMs = 200;

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, options, &result));
  EXPECT_EQ(fake.startCalls, 0);  // it waited rather than start a second one
  EXPECT_EQ(result.shell, ShellOutcome::kBack);
}

// The File Explorer Windows started never shows a desktop: still no second one.
TEST(RebuildFake, NoSecondFileExplorerNextToOneStillStarting) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.shellBackByItself = false;
  fake.explorerStartedSinceFlag = true;
  fake.Prime();
  RebuildOptions options = Options();
  options.shellWaitMs = 200;

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, options, &result));
  EXPECT_EQ(fake.startCalls, 0);
  EXPECT_EQ(result.shell, ShellOutcome::kNotBack);
  EXPECT_EQ(result.start, ShellStart::kNotTried);
  EXPECT_EQ(result.shellMs, 400u);  // the wait and the longer one
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kProblems);
}

// The shell crashed and Windows replaced it before this program ended it: the new shell counts
// as back, although it started before the old one was looked for.
TEST(RebuildFake, AShellReplacedBeforeItWasEndedCountsAsBack) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.windowsBefore = {Window(L"C:\\A", 1)};
  fake.shellBackByItself = false;
  fake.shellReplacedMeanwhile = true;
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(result.gone, 1u);
  EXPECT_EQ(fake.startCalls, 0);
  EXPECT_EQ(result.shell, ShellOutcome::kBack);
  EXPECT_EQ(result.reopened, 1u);  // its windows went with it
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
}

// A shell that a check refuses, or that could not be ended, still runs: it is neither waited for
// nor started a second time, and the windows of the File Explorer that was ended open in it.
TEST(RebuildFake, AShellNotEndedIsNeitherWaitedForNorStarted) {
  for (const HRESULT endHr : {E_ACCESSDENIED, E_FAIL}) {
    FakeRebuildSystem fake;
    fake.cacheFiles = Files(L"C:\\fake-cache", 2);
    fake.SetShell(kShellPid);
    fake.AddHolder(kShellPid, fake.kExplorerImage(), {fake.cacheFiles[0]});
    fake.AddHolder(kExplorerPid, fake.kExplorerImage(), {fake.cacheFiles[1]});
    fake.endResult[kShellPid] = endHr;
    fake.windowsBefore = {Window(L"C:\\A", 1), Window(L"C:\\B", 2)};
    fake.windowsAfter = {Window(L"C:\\A", 1)};  // the shell's own window is still open
    fake.Prime();

    RebuildResult result;
    ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
    EXPECT_EQ(fake.Ended(), (std::vector<DWORD>{kExplorerPid}));
    EXPECT_EQ(fake.startCalls, 0);
    EXPECT_EQ(result.shell, ShellOutcome::kNotEnded);
    EXPECT_EQ(result.start, ShellStart::kNotTried);
    EXPECT_EQ(result.endRefused, endHr == E_ACCESSDENIED ? 1u : 0u);
    EXPECT_EQ(result.endFailed, endHr == E_ACCESSDENIED ? 0u : 1u);
    EXPECT_EQ(result.stillOpen, 1u);
    EXPECT_EQ(result.reopened, 1u);
    EXPECT_EQ(RebuildExitCode(result),
              endHr == E_ACCESSDENIED ? ExitCode::kOk : ExitCode::kProblems);
  }
}

TEST(RebuildFake, OnlyANonShellFileExplorerEndedStillReopens) {
  const path dir = L"C:\\fake-cache";
  FakeRebuildSystem fake;
  fake.cacheFiles = Files(dir, 2);
  fake.SetShell(kShellPid);  // the shell holds no cache file, so it is not ended
  fake.AddHolder(kExplorerPid, fake.kExplorerImage(), {fake.cacheFiles[0]});
  fake.windowsBefore = {Window(L"C:\\A", 1), Window(L"C:\\B", 2)};
  fake.windowsAfter = {};
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_FALSE(fake.ShellEnded());
  EXPECT_EQ(result.shell, ShellOutcome::kNotEnded);
  EXPECT_EQ(result.reopened, 2u);  // both reopened, no shell wait was needed
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
}

TEST(RebuildFake, OnlyTheThumbnailProcessMeansNoShellWaitAndNoWindows) {
  const path dir = L"C:\\fake-cache";
  FakeRebuildSystem fake;
  fake.cacheFiles = Files(dir, 2);
  fake.SetShell(kShellPid);
  fake.AddHolder(kSurrogatePid, fake.kSurrogateImage(), {fake.cacheFiles[0]});
  fake.windowsBefore = {Window(L"C:\\A", 1)};
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(fake.Ended(), (std::vector<DWORD>{kSurrogatePid}));
  EXPECT_EQ(result.shell, ShellOutcome::kNotEnded);
  EXPECT_FALSE(result.reopenTried);
  EXPECT_EQ(result.listing, WindowListing::kNotNeeded);
}

// A File Explorer process that ended on its own meanwhile (its last window closed) is not ended
// by this program, and its windows are not opened again.
TEST(RebuildFake, AHolderGoneMeanwhileCountsAsGoneAndIsNotReopened) {
  const path dir = L"C:\\fake-cache";
  FakeRebuildSystem fake;
  fake.cacheFiles = Files(dir, 2);
  fake.SetShell(kShellPid);
  fake.AddHolder(kExplorerPid, fake.kExplorerImage(), {fake.cacheFiles[0]});
  fake.endResult[kExplorerPid] = S_FALSE;  // ended on its own meanwhile, or the id was reused
  fake.windowsBefore = {Window(L"C:\\A", 1)};
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(result.gone, 1u);
  EXPECT_EQ(result.ended, 0u);
  EXPECT_FALSE(result.reopenTried);
}

TEST(RebuildFake, TheWindowListTimingOutSkipsReopening) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.listHr = S_FALSE;  // File Explorer did not answer the listing
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(result.listing, WindowListing::kUnanswered);
  EXPECT_FALSE(result.reopenTried);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
}

TEST(RebuildFake, NoFolderWindowsMeansNothingToReopen) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.Prime();

  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(RunRebuild(fake, Options(), &result));
  EXPECT_EQ(result.listing, WindowListing::kListed);
  EXPECT_FALSE(result.reopenTried);
  EXPECT_FALSE(HasLineStarting(RebuildReport(result), L"Reopened"));
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kOk);
}

TEST(RebuildFake, TheProgressPhasesAreReported) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.windowsBefore = {Window(L"C:\\A", 1)};
  fake.windowsAfter = {};
  fake.shellBackByItself = true;
  fake.shellBackAfterMs = 100;
  fake.Prime();

  std::vector<RebuildPhase> phases;
  const RebuildProgressFn progress = [&](RebuildPhase phase) { phases.push_back(phase); };
  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(
      RebuildThumbnailCache(Options(), fake, progress, std::stop_token(), &result));
  EXPECT_EQ(phases,
            (std::vector<RebuildPhase>{RebuildPhase::kEnding, RebuildPhase::kWaitingForExplorer,
                                       RebuildPhase::kReopening}));
}

// A progress callback that throws past the point of no return does not cut the run short.
TEST(RebuildFake, AThrowingProgressCallbackDoesNotStopIt) {
  FakeRebuildSystem fake;
  ShellHoldsTheCache(&fake);
  fake.windowsBefore = {Window(L"C:\\A", 1)};
  fake.shellBackAfterMs = 100;
  fake.Prime();

  const RebuildProgressFn progress = [](RebuildPhase) { throw std::runtime_error("progress"); };
  RebuildResult result;
  ASSERT_HRESULT_SUCCEEDED(
      RebuildThumbnailCache(Options(), fake, progress, std::stop_token(), &result));
  EXPECT_EQ(result.deleted, 2u);
  EXPECT_EQ(result.shell, ShellOutcome::kBack);
  EXPECT_EQ(result.reopened, 1u);
}

TEST(RebuildFake, TheLogKeepsCountsNotPaths) {
  const path dir = L"C:\\very-private-folder\\sub";
  FakeRebuildSystem fake;
  fake.cacheFiles = Files(dir, 3);
  fake.SetShell(kShellPid);
  fake.AddHolder(kShellPid, L"C:\\private\\explorer.exe", {fake.cacheFiles[0]});  // not matched
  fake.AddHolder(kShellPid, fake.kExplorerImage(), {fake.cacheFiles[0]});
  fake.shellBackByItself = true;
  fake.shellBackAfterMs = 100;
  fake.Prime();

  test::TempDir temp(L"rebuild-log");
  ASSERT_TRUE(temp.ok());
  const path log = path(temp.path()) / L"rebuild.log";
  SetLogFileForTesting(log, kLogRotateBytes);
  RebuildResult result;
  const HRESULT hr = RunRebuild(fake, Options(), &result);
  SetLogFileForTesting({}, 0);
  ASSERT_HRESULT_SUCCEEDED(hr);

  std::ifstream stream(log, std::ios::binary);
  std::stringstream text;
  text << stream.rdbuf();
  const std::string lines = text.str();
  EXPECT_NE(lines.find("refresh full plan files=3"), std::string::npos) << lines;
  EXPECT_EQ(lines.find("very-private-folder"), std::string::npos) << lines;
  EXPECT_EQ(lines.find("explorer.exe"), std::string::npos) << lines;
  EXPECT_EQ(lines.find("thumbcache_"), std::string::npos) << lines;
}

// ---- the real system, read-only or a temp folder only ----

TEST(RebuildReal, ImagesAreUnderWindows) {
  RebuildImages images;
  ASSERT_HRESULT_SUCCEEDED(GetRebuildImages(&images));
  EXPECT_TRUE(IEquals(path(images.explorer).filename().native(), L"explorer.exe"))
      << WideToUtf8Lossy(images.explorer);
  EXPECT_TRUE(IEquals(path(images.surrogate).filename().native(), L"dllhost.exe"))
      << WideToUtf8Lossy(images.surrogate);
  // System32, so a whole-path compare tells it from SysWOW64\dllhost.exe.
  EXPECT_FALSE(SameImagePath(images.surrogate, images.surrogateWow64));
}

bool WriteFileText(const path& file, const char* text) {
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) return false;
  DWORD written = 0;
  return WriteFile(handle.get(), text, static_cast<DWORD>(std::char_traits<char>::length(text)),
                   &written, nullptr) != FALSE;
}

TEST(RebuildReal, ListsOnlyTheCacheFiles) {
  test::TempDir dir(L"cache-list");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"thumbcache_32.db", "a"));
  ASSERT_TRUE(WriteFileText(root / L"thumbcache_idx.db", "b"));
  const path readOnly = root / L"thumbcache_256.db";
  ASSERT_TRUE(WriteFileText(readOnly, "c"));
  ASSERT_TRUE(SetFileAttributesW(readOnly.c_str(), FILE_ATTRIBUTE_READONLY));
  // Decoys.
  ASSERT_TRUE(WriteFileText(root / L"iconcache_32.db", "d"));
  ASSERT_TRUE(WriteFileText(root / L"thumbcache_256.db.tmp", "e"));
  ASSERT_TRUE(CreateDirectoryW((root / L"thumbcache_x.db").c_str(), nullptr));

  std::vector<path> files;
  ASSERT_EQ(ListThumbnailCacheFiles(root, root, &files), S_OK);
  EXPECT_EQ(files.size(), 3u);
  for (const path& file : files) {
    EXPECT_TRUE(IsThumbnailCacheFileName(file.filename().native()))
        << WideToUtf8Lossy(file.native());
  }
  (void)SetFileAttributesW(readOnly.c_str(), FILE_ATTRIBUTE_NORMAL);

  // A folder that is not there: nothing, not a failure.
  std::vector<path> none;
  EXPECT_EQ(ListThumbnailCacheFiles(root / L"missing", root, &none), S_FALSE);
  EXPECT_TRUE(none.empty());
}

// ---- read-only probes of the real system (disabled; see the top of this file) ----

// The image leaf name of |holder|, or what kind of holder it is when that is not to be shown.
std::string HolderName(const Holder& holder, DWORD ownSession) {
  if (holder.session != ownSession) return "(another session)";
  if (holder.image.empty()) return "(unreadable)";
  return WideToUtf8Lossy(path(holder.image).filename().native());
}

// Who holds each real cache file: the file's leaf name, and the holders' image leaf names.
TEST(RefreshProbe, DISABLED_ListsTheCacheHolders) {
  std::vector<path> files;
  ASSERT_HRESULT_SUCCEEDED(RealRebuildSystem().CacheFiles(&files));
  DWORD ownSession = 0;
  ASSERT_TRUE(ProcessIdToSessionId(GetCurrentProcessId(), &ownSession));
  std::printf("cache files: %zu\n", files.size());
  std::map<std::string, size_t> filesByHolder;
  for (const path& file : files) {
    const path one[] = {file};
    std::vector<Holder> holders;
    ASSERT_HRESULT_SUCCEEDED(FindHolders(one, &holders));
    std::string names;
    for (const Holder& holder : holders) {
      const std::string name = HolderName(holder, ownSession);
      names += (names.empty() ? "" : ", ") + name;
      ++filesByHolder[name];
    }
    std::printf("%s: %zu holders%s%s\n", WideToUtf8Lossy(file.filename().native()).c_str(),
                holders.size(), names.empty() ? "" : ": ", names.c_str());
  }
  for (const auto& [name, count] : filesByHolder) {
    std::printf("%s holds %zu cache files\n", name.c_str(), count);
  }
}

// How File Explorer's folder windows and tabs appear: entries, and the frames they are in.
TEST(RefreshProbe, DISABLED_CountsFolderWindowsAndTabs) {
  if (!GetShellWindow()) GTEST_SKIP() << "no shell (Explorer) on this desktop";
  std::vector<FolderWindow> windows;
  ASSERT_EQ(ListFolderWindows(kShellWaitMs, &windows), S_OK);
  std::set<ULONG_PTR> frames;
  for (const FolderWindow& window : windows) frames.insert(window.frame);
  std::printf("folder windows: %zu entries in %zu frames\n", windows.size(), frames.size());
}

// Whether this process holds cache files after the Explorer test ran in it (the settings window
// runs that test in its own process).
TEST(RefreshProbe, DISABLED_OwnProcessAfterTheExplorerTest) {
  LiveTest live;
  ASSERT_HRESULT_SUCCEEDED(RunLiveTest(kProbeThumbnailSize, nullptr, kLiveTestTimeoutMs, &live));
  std::printf("explorer test: hr=%s\n", WideToUtf8Lossy(HResultText(live.hr)).c_str());
  std::vector<path> files;
  ASSERT_HRESULT_SUCCEEDED(RealRebuildSystem().CacheFiles(&files));
  std::vector<Holder> holders;
  ASSERT_HRESULT_SUCCEEDED(FindHolders(files, &holders));
  const DWORD self = GetCurrentProcessId();
  const bool holds = std::any_of(holders.begin(), holders.end(),
                                 [self](const Holder& holder) { return holder.pid == self; });
  std::printf("this process holds the thumbnail cache after the Explorer test: %s\n",
              holds ? "yes" : "no");
}

}  // namespace
}  // namespace pdft
