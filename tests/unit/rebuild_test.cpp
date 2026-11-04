// The pure parts of the thumbnail-cache rebuild (rebuild.h): the cache-file name test, the
// refusal decision, how a start leaves this process's job (processes.h), how a holder is
// classified and in what order holders are ended, the wait for File Explorer, the plan to open
// folder windows again, the exit code, and the report and question text. No Windows state is read
// here.

#include <windows.h>

#include "rebuild.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "strings.h"

namespace pdft {
namespace {

FILETIME Time(ULONGLONG ticks) {
  FILETIME value = {};
  value.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFF);
  value.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return value;
}

RebuildImages Images() {
  RebuildImages images;
  images.explorer = L"C:\\Windows\\explorer.exe";
  images.surrogate = L"C:\\Windows\\System32\\dllhost.exe";
  images.surrogateWow64 = L"C:\\Windows\\SysWOW64\\dllhost.exe";
  return images;
}

Holder Found(DWORD pid, std::wstring image, DWORD session = 7, ULONGLONG start = 1000) {
  Holder holder;
  holder.pid = pid;
  holder.start = Time(start);
  holder.session = session;
  holder.image = std::move(image);
  return holder;
}

// ---- IsThumbnailCacheFileName ----

TEST(ThumbnailCacheName, AcceptsTheCacheFilesOnly) {
  EXPECT_TRUE(IsThumbnailCacheFileName(L"thumbcache_256.db"));
  EXPECT_TRUE(IsThumbnailCacheFileName(L"THUMBCACHE_IDX.DB"));
  EXPECT_TRUE(IsThumbnailCacheFileName(L"thumbcache_wide_alternate.db"));

  EXPECT_FALSE(IsThumbnailCacheFileName(L"iconcache_256.db"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L"thumbcache_.db"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L"thumbcache_256.db.tmp"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L"thumbcache_256.dbx"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L"thumbcache_256.db:x"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L"thumbcache_2 56.db"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L"thumbcache_a\\b.db"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L"thumbcache_a/b.db"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L"..\\thumbcache_1.db"));
  EXPECT_FALSE(IsThumbnailCacheFileName(L""));
}

// ---- DecideRebuildRefusal ----

RebuildConditions Clean() {
  RebuildConditions c;
  c.writesChecked = true;
  c.autoRestartShell = true;
  return c;
}

TEST(RebuildRefusal, OrderAndTruthTable) {
  EXPECT_EQ(DecideRebuildRefusal(Clean()), RebuildRefusal::kNone);

  RebuildConditions elevated = Clean();
  elevated.elevated = true;
  elevated.virtualized = true;  // elevated still wins
  EXPECT_EQ(DecideRebuildRefusal(elevated), RebuildRefusal::kElevated);

  RebuildConditions notChecked = Clean();
  notChecked.writesChecked = false;
  EXPECT_EQ(DecideRebuildRefusal(notChecked), RebuildRefusal::kVirtualized);

  RebuildConditions virtualized = Clean();
  virtualized.virtualized = true;
  virtualized.otherShellUser = true;  // virtualized wins over the shell user
  EXPECT_EQ(DecideRebuildRefusal(virtualized), RebuildRefusal::kVirtualized);

  RebuildConditions otherUser = Clean();
  otherUser.otherShellUser = true;
  EXPECT_EQ(DecideRebuildRefusal(otherUser), RebuildRefusal::kOtherShellUser);

  // A package alone is no refusal; only together with AutoRestartShell off.
  RebuildConditions package = Clean();
  package.belowPackage = true;
  EXPECT_EQ(DecideRebuildRefusal(package), RebuildRefusal::kNone);
  package.autoRestartShell = false;
  EXPECT_EQ(DecideRebuildRefusal(package), RebuildRefusal::kNoRestartBelowPackage);

  // AutoRestartShell off without a package is fine where this program can start File Explorer
  // itself, outside every job.
  RebuildConditions noRestart = Clean();
  noRestart.autoRestartShell = false;
  noRestart.explorerStart = ExplorerStartCheck::kOutsideJobs;
  EXPECT_EQ(DecideRebuildRefusal(noRestart), RebuildRefusal::kNone);
}

// Where Windows does not restart File Explorer, the start this program would make decides: no
// trial yet counts as a File Explorer that would stay in a job.
TEST(RebuildRefusal, TheStartOfFileExplorerWhereWindowsDoesNot) {
  RebuildConditions off = Clean();
  off.autoRestartShell = false;
  off.explorerStart = ExplorerStartCheck::kNotChecked;
  EXPECT_EQ(DecideRebuildRefusal(off), RebuildRefusal::kNoRestartInJob);
  off.explorerStart = ExplorerStartCheck::kInsideJob;
  EXPECT_EQ(DecideRebuildRefusal(off), RebuildRefusal::kNoRestartInJob);
  off.explorerStart = ExplorerStartCheck::kFails;
  EXPECT_EQ(DecideRebuildRefusal(off), RebuildRefusal::kNoRestartStartFails);
  off.explorerStart = ExplorerStartCheck::kOutsideJobs;
  EXPECT_EQ(DecideRebuildRefusal(off), RebuildRefusal::kNone);

  // The package wins over the start.
  RebuildConditions package = off;
  package.belowPackage = true;
  for (const ExplorerStartCheck start :
       {ExplorerStartCheck::kNotChecked, ExplorerStartCheck::kOutsideJobs,
        ExplorerStartCheck::kInsideJob, ExplorerStartCheck::kFails}) {
    package.explorerStart = start;
    EXPECT_EQ(DecideRebuildRefusal(package), RebuildRefusal::kNoRestartBelowPackage)
        << static_cast<int>(start);
  }

  // With AutoRestartShell on, the start is no refusal at all; the context's refusals come first.
  RebuildConditions on = Clean();
  for (const ExplorerStartCheck start :
       {ExplorerStartCheck::kNotChecked, ExplorerStartCheck::kInsideJob,
        ExplorerStartCheck::kFails}) {
    on.explorerStart = start;
    EXPECT_EQ(DecideRebuildRefusal(on), RebuildRefusal::kNone) << static_cast<int>(start);
  }
  RebuildConditions otherUser = off;
  otherUser.explorerStart = ExplorerStartCheck::kFails;
  otherUser.otherShellUser = true;
  EXPECT_EQ(DecideRebuildRefusal(otherUser), RebuildRefusal::kOtherShellUser);
}

TEST(RebuildRefusal, ConditionsFromTheContext) {
  ProcessContext context;
  context.probe = ContextProbe::kWrite;
  context.virtualization = Virtualization::kNo;
  const RebuildConditions clean = RebuildConditionsOf(context, true);
  EXPECT_TRUE(clean.writesChecked);
  EXPECT_FALSE(clean.virtualized);
  EXPECT_TRUE(clean.autoRestartShell);

  context.probe = ContextProbe::kReadOnly;
  context.virtualization = Virtualization::kUnknown;
  context.elevated = true;
  context.differentFromShellUser = true;
  context.packagedAncestor = L"Some.Package_8wekyb3d8bbwe";
  const RebuildConditions dirty =
      RebuildConditionsOf(context, false, ExplorerStartCheck::kInsideJob);
  EXPECT_TRUE(dirty.elevated);
  EXPECT_FALSE(dirty.writesChecked);
  EXPECT_TRUE(dirty.virtualized);
  EXPECT_TRUE(dirty.otherShellUser);
  EXPECT_TRUE(dirty.belowPackage);
  EXPECT_FALSE(dirty.autoRestartShell);
  EXPECT_EQ(dirty.explorerStart, ExplorerStartCheck::kInsideJob);
  EXPECT_EQ(clean.explorerStart, ExplorerStartCheck::kNotChecked);
}

// ---- jobs: ChooseBreakaway, ChildEndsWithJob, JobFactsToken ----

JobFacts InJob(std::optional<DWORD> limitFlags) {
  JobFacts facts;
  facts.inJob = true;
  facts.limitFlags = limitFlags;
  return facts;
}

TEST(Breakaway, TheModeFollowsTheImmediateJobsLimits) {
  EXPECT_EQ(ChooseBreakaway(JobFacts{}), BreakawayMode::kNotInJob);
  EXPECT_EQ(ChooseBreakaway(InJob(0x800)), BreakawayMode::kAsk);
  EXPECT_EQ(ChooseBreakaway(InJob(0x1800)), BreakawayMode::kAsk);
  EXPECT_EQ(ChooseBreakaway(InJob(0x1000)), BreakawayMode::kSilent);
  EXPECT_EQ(ChooseBreakaway(InJob(0x0)), BreakawayMode::kForbidden);
  EXPECT_EQ(ChooseBreakaway(InJob(0x2000)), BreakawayMode::kForbidden);
  // Limits that could not be read: ask, as before; a job that refuses says so.
  EXPECT_EQ(ChooseBreakaway(InJob(std::nullopt)), BreakawayMode::kAsk);
}

TEST(Breakaway, OnlyAJobThatMayRefuseIsBlamed) {
  // Limits not read: access denied is the job's refusal.
  EXPECT_TRUE(JobRefusedStart(InJob(std::nullopt), ERROR_ACCESS_DENIED));
  EXPECT_FALSE(JobRefusedStart(InJob(std::nullopt), ERROR_FILE_NOT_FOUND));
  // Limits that let programs leave: access denied comes from the image, not the job.
  EXPECT_FALSE(JobRefusedStart(InJob(0x800), ERROR_ACCESS_DENIED));
  EXPECT_FALSE(JobRefusedStart(InJob(0x3800), ERROR_ACCESS_DENIED));
  // No breakaway asked for: nothing for the job to refuse.
  EXPECT_FALSE(JobRefusedStart(JobFacts{}, ERROR_ACCESS_DENIED));
  EXPECT_FALSE(JobRefusedStart(InJob(0x1000), ERROR_ACCESS_DENIED));
  EXPECT_FALSE(JobRefusedStart(InJob(0x2000), ERROR_ACCESS_DENIED));
}

TEST(Breakaway, AChildEndsWithAKillOnCloseJob) {
  EXPECT_FALSE(ChildEndsWithJob(JobFacts{}));
  EXPECT_TRUE(ChildEndsWithJob(InJob(0x2000)));
  EXPECT_TRUE(ChildEndsWithJob(InJob(0x2800)));
  EXPECT_FALSE(ChildEndsWithJob(InJob(0x3000)));  // silent breakaway: not in the job
  EXPECT_FALSE(ChildEndsWithJob(InJob(0x800)));
  EXPECT_TRUE(ChildEndsWithJob(InJob(std::nullopt)));
}

TEST(Breakaway, TheLogTokenShowsOnlyTheJobLimitsThatMatter) {
  EXPECT_EQ(JobFactsToken(JobFacts{}), L"none");
  EXPECT_EQ(JobFactsToken(InJob(std::nullopt)), L"unread");
  EXPECT_EQ(JobFactsToken(InJob(0xFFFFFFFF)), L"0x00003800");
  EXPECT_EQ(JobFactsToken(InJob(0x0)), L"0x00000000");
  EXPECT_EQ(JobFactsToken(InJob(0x2008)), L"0x00002000");
}

// ---- ClassifyHolder ----

TEST(ClassifyHolder, EachRole) {
  const RebuildImages images = Images();
  const DWORD ownPid = 100;
  const DWORD ownSession = 7;
  const ShellProcess shell = {200, Time(1000)};

  EXPECT_EQ(ClassifyHolder(Found(ownPid, images.explorer), ownPid, ownSession, shell, images),
            HolderRole::kSelf);
  // The shell: its pid and start.
  Holder shellHolder = Found(200, images.explorer);
  shellHolder.start = Time(1000);
  EXPECT_EQ(ClassifyHolder(shellHolder, ownPid, ownSession, shell, images), HolderRole::kShell);
  // The shell's pid with another start is a different File Explorer process.
  Holder restarted = Found(200, images.explorer);
  restarted.start = Time(2000);
  EXPECT_EQ(ClassifyHolder(restarted, ownPid, ownSession, shell, images),
            HolderRole::kFileExplorer);
  // Another File Explorer process.
  EXPECT_EQ(ClassifyHolder(Found(300, images.explorer), ownPid, ownSession, shell, images),
            HolderRole::kFileExplorer);
  // The thumbnail process, in each of its folders.
  EXPECT_EQ(ClassifyHolder(Found(400, images.surrogate), ownPid, ownSession, shell, images),
            HolderRole::kSurrogate);
  EXPECT_EQ(ClassifyHolder(Found(401, images.surrogateWow64), ownPid, ownSession, shell, images),
            HolderRole::kSurrogate);
  // A whole-path compare: case and "." / ".." do not matter.
  EXPECT_EQ(ClassifyHolder(Found(402, L"C:\\Windows\\System32\\..\\explorer.exe"), ownPid,
                           ownSession, shell, images),
            HolderRole::kFileExplorer);
  EXPECT_EQ(ClassifyHolder(Found(403, L"c:\\windows\\SYSTEM32\\DLLHOST.EXE"), ownPid, ownSession,
                           shell, images),
            HolderRole::kSurrogate);

  // Left alone: another session, no readable image, the 32-bit shell, a dllhost elsewhere.
  EXPECT_EQ(ClassifyHolder(Found(500, images.explorer, 9), ownPid, ownSession, shell, images),
            HolderRole::kLeftAlone);
  EXPECT_EQ(ClassifyHolder(Found(501, L""), ownPid, ownSession, shell, images),
            HolderRole::kLeftAlone);
  EXPECT_EQ(ClassifyHolder(Found(502, L"C:\\Windows\\SysWOW64\\explorer.exe"), ownPid, ownSession,
                           shell, images),
            HolderRole::kLeftAlone);
  EXPECT_EQ(
      ClassifyHolder(Found(503, L"C:\\Tools\\x\\dllhost.exe"), ownPid, ownSession, shell, images),
      HolderRole::kLeftAlone);

  // With no shell at all, an explorer.exe is a File Explorer process (never the shell).
  EXPECT_EQ(ClassifyHolder(Found(200, images.explorer), ownPid, ownSession, std::nullopt, images),
            HolderRole::kFileExplorer);
}

// ---- EndingOrder ----

TEST(EndingOrder, SurrogatesThenExplorersThenShellDeduped) {
  std::vector<Holder> holders = {
      Found(10, L"a"),  // shell
      Found(11, L"b"),  // surrogate
      Found(12, L"c"),  // file explorer
      Found(11, L"b"),  // the same surrogate again (another file)
      Found(13, L"d"),  // left alone
  };
  std::vector<HolderRole> roles = {HolderRole::kShell, HolderRole::kSurrogate,
                                   HolderRole::kFileExplorer, HolderRole::kSurrogate,
                                   HolderRole::kLeftAlone};
  const std::vector<size_t> order = EndingOrder(holders, roles);
  // The surrogate once, then the File Explorer process, then the shell.
  ASSERT_EQ(order.size(), 3u);
  EXPECT_EQ(order[0], 1u);
  EXPECT_EQ(order[1], 2u);
  EXPECT_EQ(order[2], 0u);
}

// ---- NextShellStep ----

ShellWaitState Waiting() {
  ShellWaitState s;
  s.autoRestart = true;
  s.limitMs = 5000;
  return s;
}

TEST(NextShellStep, TheWholeWalk) {
  // Back in time.
  ShellWaitState back = Waiting();
  back.back = true;
  EXPECT_EQ(NextShellStep(back), ShellStep::kDone);

  // Waiting while Windows may still bring it back.
  ShellWaitState early = Waiting();
  early.waitedMs = 1000;
  EXPECT_EQ(NextShellStep(early), ShellStep::kWait);

  // At the deadline with a new File Explorer coming up: extend once.
  ShellWaitState deadline = Waiting();
  deadline.waitedMs = 5000;
  deadline.newExplorerRunning = true;
  EXPECT_EQ(NextShellStep(deadline), ShellStep::kExtend);
  // Still coming up at the end of the longer wait: never a second one next to it.
  deadline.extended = true;
  EXPECT_EQ(NextShellStep(deadline), ShellStep::kGiveUp);
  // The one Windows started went away meanwhile: start it.
  deadline.newExplorerRunning = false;
  EXPECT_EQ(NextShellStep(deadline), ShellStep::kStart);
  // ... except below a package.
  deadline.belowPackage = true;
  EXPECT_EQ(NextShellStep(deadline), ShellStep::kGiveUp);

  // At the deadline, nothing coming: start it.
  ShellWaitState timedOut = Waiting();
  timedOut.waitedMs = 5000;
  EXPECT_EQ(NextShellStep(timedOut), ShellStep::kStart);

  // AutoRestartShell off: straight to start, no wait.
  ShellWaitState off = Waiting();
  off.autoRestart = false;
  EXPECT_EQ(NextShellStep(off), ShellStep::kStart);

  // Below a package: give up instead of starting.
  ShellWaitState package = Waiting();
  package.autoRestart = false;
  package.belowPackage = true;
  EXPECT_EQ(NextShellStep(package), ShellStep::kGiveUp);

  // Started, still waiting, then back.
  ShellWaitState started = Waiting();
  started.startTried = true;
  started.waitedMs = 1000;
  EXPECT_EQ(NextShellStep(started), ShellStep::kWait);
  started.back = true;
  EXPECT_EQ(NextShellStep(started), ShellStep::kDone);

  // Started, then nothing.
  ShellWaitState startedTimedOut = Waiting();
  startedTimedOut.startTried = true;
  startedTimedOut.waitedMs = 5000;
  EXPECT_EQ(NextShellStep(startedTimedOut), ShellStep::kGiveUp);
}

// ---- PlanReopen ----

FolderWindow Window(std::wstring name, ULONG_PTR frame = 0) {
  FolderWindow window;
  window.parsingName = std::move(name);
  window.frame = frame;
  return window;
}

TEST(PlanReopen, SkipsOpenAndCapsAndNotesTabs) {
  const std::vector<FolderWindow> saved = {Window(L"C:\\A", 1), Window(L"C:\\A", 1),
                                           Window(L"C:\\B", 2), Window(L"", 3)};
  // One C:\A is still open, under a different frame.
  const std::vector<FolderWindow> now = {Window(L"c:\\a", 9)};
  const ReopenPlan plan = PlanReopen(saved, now);
  EXPECT_EQ(plan.stillOpen, 1u);  // one C:\A matched
  EXPECT_EQ(plan.unnamed, 1u);    // the nameless one
  ASSERT_EQ(plan.open.size(), 2u);
  EXPECT_EQ(plan.open[0].parsingName, L"C:\\A");
  EXPECT_EQ(plan.open[1].parsingName, L"C:\\B");
  EXPECT_FALSE(plan.tabs);

  // Two tabs of one frame: they come back as separate windows.
  const std::vector<FolderWindow> tabs = {Window(L"C:\\A", 5), Window(L"C:\\B", 5)};
  const ReopenPlan tabPlan = PlanReopen(tabs, {});
  EXPECT_TRUE(tabPlan.tabs);
  EXPECT_EQ(tabPlan.open.size(), 2u);

  // The cap of 25.
  std::vector<FolderWindow> many;
  for (int i = 0; i < 30; ++i) many.push_back(Window(L"C:\\N" + std::to_wstring(i)));
  const ReopenPlan capped = PlanReopen(many, {});
  EXPECT_EQ(capped.open.size(), kMaxReopenedWindows);
  EXPECT_EQ(capped.overCap, 5u);
}

// ---- RebuildExitCode ----

RebuildResult Done() {
  RebuildResult r;
  r.cacheFiles = 10;
  r.deleted = 10;
  return r;
}

TEST(RebuildExitCode, EveryRow) {
  RebuildResult context;
  for (const RebuildRefusal refusal :
       {RebuildRefusal::kElevated, RebuildRefusal::kVirtualized, RebuildRefusal::kOtherShellUser,
        RebuildRefusal::kNoRestartBelowPackage, RebuildRefusal::kNoRestartInJob}) {
    context.refusal = refusal;
    EXPECT_EQ(RebuildExitCode(context), ExitCode::kWrongContext);
  }
  context.refusal = RebuildRefusal::kNoRestartStartFails;
  EXPECT_EQ(RebuildExitCode(context), ExitCode::kFailed);
  context.refusal = RebuildRefusal::kOwnProcessHoldsCache;
  EXPECT_EQ(RebuildExitCode(context), ExitCode::kFailed);
  context.refusal = RebuildRefusal::kFileOperation;
  EXPECT_EQ(RebuildExitCode(context), ExitCode::kProblems);
  context.refusal = RebuildRefusal::kCancelled;
  EXPECT_EQ(RebuildExitCode(context), ExitCode::kCancelled);

  RebuildResult failure;
  failure.failure = E_FAIL;
  EXPECT_EQ(RebuildExitCode(failure), ExitCode::kFailed);

  // An empty cache is success.
  RebuildResult empty;
  EXPECT_EQ(RebuildExitCode(empty), ExitCode::kOk);

  EXPECT_EQ(RebuildExitCode(Done()), ExitCode::kOk);

  // Files left in use by programs left alone are still success.
  RebuildResult leftAlone = Done();
  leftAlone.deleted = 8;
  leftAlone.inUse = 2;
  EXPECT_EQ(RebuildExitCode(leftAlone), ExitCode::kOk);

  RebuildResult notBack = Done();
  notBack.shell = ShellOutcome::kNotBack;
  EXPECT_EQ(RebuildExitCode(notBack), ExitCode::kProblems);

  RebuildResult failed = Done();
  failed.failed = 1;
  EXPECT_EQ(RebuildExitCode(failed), ExitCode::kProblems);

  RebuildResult endFailed = Done();
  endFailed.endFailed = 1;
  EXPECT_EQ(RebuildExitCode(endFailed), ExitCode::kProblems);

  RebuildResult nothingDeleted = Done();
  nothingDeleted.deleted = 0;
  nothingDeleted.inUse = 10;
  EXPECT_EQ(RebuildExitCode(nothingDeleted), ExitCode::kProblems);

  // Reopen failures alone do not spoil it.
  RebuildResult reopenFailed = Done();
  reopenFailed.reopenTried = true;
  reopenFailed.reopenFailed = 3;
  EXPECT_EQ(RebuildExitCode(reopenFailed), ExitCode::kOk);
}

// ---- RebuildReport ----

bool HasLine(const std::vector<std::wstring>& lines, const std::wstring& line) {
  for (const std::wstring& l : lines) {
    if (l == line) return true;
  }
  return false;
}

TEST(RebuildReport, EmptyCache) {
  RebuildResult empty;
  const std::vector<std::wstring> lines = RebuildReport(empty);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], L"The thumbnail cache is already empty; there is nothing to rebuild.");
}

TEST(RebuildReport, NamesAreLeavesSortedAndCapped) {
  RebuildResult r;
  r.cacheFiles = 20;
  r.deleted = 13;
  r.inUse = 7;
  // The engine already sorts and deduplicates; the report shows at most four, then "and <m> more".
  r.inUseBy = {L"Five.exe", L"Four.exe", L"One.exe", L"SearchHost.exe", L"Three.exe", L"Two.exe"};
  r.shell = ShellOutcome::kBack;
  r.reopenTried = true;
  r.reopened = 3;
  r.reopenFailed = 1;
  r.tabs = true;
  const std::vector<std::wstring> lines = RebuildReport(r);
  EXPECT_TRUE(HasLine(lines, L"Thumbnail cache cleared: 13 of 20 cache files deleted."));
  EXPECT_TRUE(HasLine(lines,
                      L"7 cache files are in use by programs this program leaves alone (Five.exe, "
                      L"Four.exe, One.exe, SearchHost.exe, and 2 more); their thumbnails update "
                      L"after you next sign in."));
  EXPECT_TRUE(HasLine(lines, L"File Explorer restarted."));
  EXPECT_TRUE(HasLine(lines,
                      L"Reopened 3 of 4 folder windows. Tabs come back as separate windows. 1 "
                      L"could not be reopened."));
  EXPECT_TRUE(HasLine(lines, L"Thumbnails are made again as folders are opened."));
  // No path ever leaks into a line.
  for (const std::wstring& line : lines) {
    EXPECT_EQ(line.find(L"C:\\"), std::wstring::npos) << line;
    EXPECT_EQ(line.find(L":\\"), std::wstring::npos) << line;
  }
}

TEST(RebuildReport, OtherAccountsAndFailuresAndTheStartedShell) {
  RebuildResult r;
  r.cacheFiles = 4;
  r.deleted = 1;
  r.inUse = 2;
  r.inUseByOthers = true;  // no readable names
  r.failed = 1;
  r.firstFileError = E_ACCESSDENIED;
  r.shell = ShellOutcome::kStarted;
  const std::vector<std::wstring> lines = RebuildReport(r);
  EXPECT_TRUE(HasLine(lines,
                      L"2 cache files are in use by programs this program leaves alone (programs "
                      L"of other accounts or sessions); their thumbnails update after you next "
                      L"sign in."));
  EXPECT_TRUE(HasLine(lines, L"1 cache file could not be deleted (0x80070005)."));
  EXPECT_TRUE(
      HasLine(lines, L"File Explorer did not start again by itself, so this program started it."));
}

TEST(RebuildReport, OneOfEach) {
  RebuildResult r;
  r.cacheFiles = 1;
  r.deleted = 0;
  r.inUse = 1;
  r.inUseBy = {L"SearchHost.exe"};
  r.shell = ShellOutcome::kBack;
  r.reopenTried = true;
  r.reopened = 1;
  const std::vector<std::wstring> lines = RebuildReport(r);
  EXPECT_TRUE(
      HasLine(lines, L"The thumbnail cache was not cleared: its cache file could not be deleted."));
  EXPECT_TRUE(HasLine(lines,
                      L"1 cache file is in use by programs this program leaves alone "
                      L"(SearchHost.exe); its thumbnails update after you next sign in."));
  EXPECT_TRUE(HasLine(lines, L"Reopened 1 of 1 folder window."));
}

// No file deleted (exit 1): the first line says the cache was not cleared, never "cleared".
TEST(RebuildReport, NothingDeleted) {
  RebuildResult r;
  r.cacheFiles = 3;
  r.deleted = 0;
  r.inUse = 2;
  r.inUseBy = {L"SearchHost.exe"};
  r.failed = 1;
  r.firstFileError = E_ACCESSDENIED;
  ASSERT_EQ(RebuildExitCode(r), ExitCode::kProblems);
  EXPECT_EQ(RebuildReport(r),
            (std::vector<std::wstring>{
                L"The thumbnail cache was not cleared: none of its 3 cache files could be deleted.",
                L"2 cache files are in use by programs this program leaves alone "
                L"(SearchHost.exe); their thumbnails update after you next sign in.",
                L"1 cache file could not be deleted (0x80070005).",
                L"Thumbnails are made again as folders are opened."}));
}

// A File Explorer or thumbnail process that could not be ended (exit 1) has a line of its own, and
// the cache files it keeps are its, never those of a program this program leaves alone.
TEST(RebuildReport, AProcessNotEndedIsSaidAndNamedApart) {
  RebuildResult r;
  r.cacheFiles = 2;
  r.deleted = 1;
  r.inUse = 1;
  r.endFailed = 1;
  r.firstEndError = E_FAIL;
  r.inUseByNotEnded = {L"explorer.exe"};
  r.reopenTried = true;
  r.reopened = 1;
  ASSERT_EQ(RebuildExitCode(r), ExitCode::kProblems);
  const std::vector<std::wstring> lines = RebuildReport(r);
  EXPECT_EQ(lines, (std::vector<std::wstring>{
                       L"Thumbnail cache cleared: 1 of 2 cache files deleted.",
                       L"1 File Explorer or thumbnail process could not be ended (0x80004005).",
                       L"1 cache file is in use by File Explorer or thumbnail processes still "
                       L"running (explorer.exe); its thumbnails update after you next sign in.",
                       L"Reopened 1 of 1 folder window.",
                       L"Thumbnails are made again as folders are opened."}));

  // Next to programs left alone, by name or as other accounts'.
  RebuildResult both = r;
  both.cacheFiles = 4;
  both.inUse = 3;
  both.endFailed = 2;
  both.firstEndError = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  both.inUseByNotEnded = {L"dllhost.exe", L"explorer.exe"};
  both.inUseBy = {L"SearchHost.exe"};
  const std::vector<std::wstring> bothLines = RebuildReport(both);
  EXPECT_TRUE(HasLine(bothLines,
                      L"2 File Explorer or thumbnail processes could not be ended (0x800705B4)."));
  EXPECT_TRUE(HasLine(bothLines,
                      L"3 cache files are in use by File Explorer or thumbnail processes still "
                      L"running (dllhost.exe, explorer.exe) and by programs this program leaves "
                      L"alone (SearchHost.exe); their thumbnails update after you next sign in."));
  RebuildResult others = r;
  others.inUseByOthers = true;
  EXPECT_TRUE(HasLine(RebuildReport(others),
                      L"1 cache file is in use by File Explorer or thumbnail processes still "
                      L"running (explorer.exe) and by programs this program leaves alone "
                      L"(programs of other accounts or sessions); its thumbnails update after you "
                      L"next sign in."));
}

TEST(RebuildReport, NothingToReopen) {
  // Every window was open again already: no line about reopening.
  RebuildResult r = {};
  r.cacheFiles = 2;
  r.deleted = 2;
  r.shell = ShellOutcome::kBack;
  r.reopenTried = true;
  r.stillOpen = 3;
  for (const std::wstring& line : RebuildReport(r)) {
    EXPECT_FALSE(line.starts_with(L"Reopened")) << WideToUtf8Lossy(line);
  }
}

TEST(RebuildReport, ShellDidNotComeBackAndListingFailed) {
  RebuildResult belowPackage;
  belowPackage.cacheFiles = 1;
  belowPackage.deleted = 1;
  belowPackage.shell = ShellOutcome::kNotBack;
  belowPackage.start = ShellStart::kBelowPackage;
  belowPackage.listing = WindowListing::kUnanswered;
  const std::vector<std::wstring> lines = RebuildReport(belowPackage);
  EXPECT_TRUE(HasLine(lines,
                      L"File Explorer did not start again. Start it from Task Manager: Run new "
                      L"task, explorer.exe. This program does not start it from inside an app "
                      L"package (the terminal it runs in)."));
  EXPECT_TRUE(HasLine(lines,
                      L"The open folder windows could not be listed (File Explorer did not "
                      L"answer), so they were not reopened."));

  RebuildResult jobForbids = belowPackage;
  jobForbids.start = ShellStart::kJobForbids;
  jobForbids.listing = WindowListing::kNotNeeded;
  const std::vector<std::wstring> jobLines = RebuildReport(jobForbids);
  EXPECT_TRUE(HasLine(jobLines,
                      L"File Explorer did not start again. Start it from Task Manager: Run new "
                      L"task, explorer.exe. This program did not start it: from here it would "
                      L"have stayed inside a job of the program this runs in."));

  // A start that could not be made ready before anything changed: the plain line.
  RebuildResult notReady = jobForbids;
  notReady.start = ShellStart::kFailed;
  notReady.startError = E_FAIL;
  EXPECT_TRUE(HasLine(RebuildReport(notReady),
                      L"File Explorer did not start again. Start it from Task Manager: Run new "
                      L"task, explorer.exe."));
}

TEST(RebuildReport, CappedReopen) {
  RebuildResult r;
  r.cacheFiles = 1;
  r.deleted = 1;
  r.shell = ShellOutcome::kBack;
  r.reopenTried = true;
  r.reopened = 25;
  r.reopenFailed = 0;
  r.overCap = 4;
  const std::vector<std::wstring> lines = RebuildReport(r);
  EXPECT_TRUE(
      HasLine(lines, L"Reopened 25 of 29 folder windows. Only the first 25 were reopened."));
}

// ---- a rebuild the check before the question refused ----

TEST(RebuildRefusal, Tokens) {
  EXPECT_STREQ(RebuildRefusalToken(RebuildRefusal::kNoRestartInJob), L"no-restart-in-job");
  EXPECT_STREQ(RebuildRefusalToken(RebuildRefusal::kNoRestartStartFails),
               L"no-restart-start-fails");
  EXPECT_STREQ(RebuildRefusalToken(RebuildRefusal::kNoRestartBelowPackage),
               L"no-restart-below-package");
}

TEST(RebuildNotStarted, CopiesWhatTheCheckFoundAndCommitsNothing) {
  RebuildPrecheck check;
  check.conditions = Clean();
  check.conditions.autoRestartShell = false;
  check.conditions.explorerStart = ExplorerStartCheck::kFails;
  check.refusal = RebuildRefusal::kNoRestartStartFails;
  check.job = JobFacts{true, 0x2000};
  check.startError = E_ACCESSDENIED;
  const RebuildResult result = RebuildNotStarted(check);
  EXPECT_EQ(result.refusal, RebuildRefusal::kNoRestartStartFails);
  EXPECT_EQ(result.failure, S_OK);
  EXPECT_EQ(result.startError, E_ACCESSDENIED);
  EXPECT_FALSE(result.autoRestartShell);
  EXPECT_EQ(result.explorerStart, ExplorerStartCheck::kFails);
  ASSERT_TRUE(result.job);
  EXPECT_TRUE(result.job->inJob);
  EXPECT_FALSE(result.jobEscape);
  EXPECT_FALSE(result.committed);
  EXPECT_EQ(result.ended, 0u);
  EXPECT_EQ(result.shell, ShellOutcome::kNotEnded);
  EXPECT_EQ(RebuildExitCode(result), ExitCode::kFailed);
  EXPECT_EQ(RebuildPrecheckLogText(check),
            L"refresh full refused=no-restart-start-fails start-hr=0x80070005 job=0x00002000");

  // The start could not be made ready, so the job was never read: not "none" (no job).
  RebuildPrecheck notReady = check;
  notReady.job.reset();
  EXPECT_FALSE(RebuildNotStarted(notReady).job);
  EXPECT_EQ(RebuildPrecheckLogText(notReady),
            L"refresh full refused=no-restart-start-fails start-hr=0x80070005 job=not-read");

  RebuildPrecheck stays = check;
  stays.conditions.explorerStart = ExplorerStartCheck::kInsideJob;
  stays.refusal = RebuildRefusal::kNoRestartInJob;
  stays.escape = JobEscape::kStays;
  stays.startError = S_OK;
  stays.job = JobFacts{true, std::nullopt};
  const RebuildResult inJob = RebuildNotStarted(stays);
  ASSERT_TRUE(inJob.jobEscape);
  EXPECT_EQ(*inJob.jobEscape, JobEscape::kStays);
  EXPECT_EQ(RebuildExitCode(inJob), ExitCode::kWrongContext);
  EXPECT_EQ(RebuildPrecheckLogText(stays),
            L"refresh full refused=no-restart-in-job explorer-start=stays job=unread");

  RebuildPrecheck failed;
  failed.failure = E_FAIL;
  const RebuildResult notRead = RebuildNotStarted(failed);
  EXPECT_EQ(notRead.failure, E_FAIL);
  EXPECT_FALSE(notRead.committed);
  EXPECT_EQ(RebuildExitCode(notRead), ExitCode::kFailed);
  EXPECT_EQ(RebuildPrecheckLogText(failed), L"refresh full failed hr=0x80004005");

  // Every other refusal keeps the plain line.
  RebuildPrecheck elevated;
  elevated.refusal = RebuildRefusal::kElevated;
  EXPECT_EQ(RebuildPrecheckLogText(elevated), L"refresh full refused=elevated");
}

// ---- text ----

TEST(RebuildText, QuestionHasNoBracket) {
  const std::wstring question = RebuildQuestion();
  EXPECT_EQ(question.find(L"[y/N]"), std::wstring::npos);
  EXPECT_NE(question.find(L"Rebuild it now?"), std::wstring::npos);
}

TEST(RebuildText, RefusalsAndFailures) {
  ProcessContext context;
  context.userAccount = L"PC\\tester";
  EXPECT_NE(RebuildRefusalText(RebuildRefusal::kElevated, context).find(L"administrator"),
            std::wstring::npos);
  EXPECT_NE(RebuildRefusalText(RebuildRefusal::kOtherShellUser, context).find(L"PC\\tester"),
            std::wstring::npos);
  EXPECT_FALSE(RebuildRefusalText(RebuildRefusal::kNoRestartBelowPackage, context).empty());
  EXPECT_EQ(RebuildRefusalText(RebuildRefusal::kNoRestartInJob, context),
            L"Cannot run here: Windows is set not to restart File Explorer by itself, and a File "
            L"Explorer started from here would stay inside a job of the program this runs in (a "
            L"terminal or another tool), so ending that program could end File Explorer and the "
            L"taskbar too. Nothing changed; run --refresh --full from a Win+R prompt.");
  EXPECT_EQ(RebuildRefusalText(RebuildRefusal::kNoRestartStartFails, context,
                               HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)),
            L"Cannot rebuild the thumbnail cache: Windows is set not to restart File Explorer by "
            L"itself, and this program cannot start File Explorer from here either (0x80070005), "
            L"so the taskbar would not come back. Nothing changed.");
  EXPECT_FALSE(RebuildRefusalText(RebuildRefusal::kOwnProcessHoldsCache, context).empty());
  EXPECT_FALSE(RebuildRefusalText(RebuildRefusal::kFileOperation, context).empty());
  // The caller supplies the virtualized and cancelled text.
  EXPECT_TRUE(RebuildRefusalText(RebuildRefusal::kVirtualized, context).empty());
  EXPECT_TRUE(RebuildRefusalText(RebuildRefusal::kCancelled, context).empty());

  EXPECT_NE(RebuildFailureText(HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK)).find(L"link"),
            std::wstring::npos);
  EXPECT_NE(RebuildFailureText(E_FAIL).find(L"0x80004005"), std::wstring::npos);
}

TEST(RebuildText, ProgressLines) {
  EXPECT_EQ(RebuildProgressText(RebuildPhase::kEnding),
            L"Restarting File Explorer and clearing the thumbnail cache...");
  EXPECT_EQ(RebuildProgressText(RebuildPhase::kDeletingOnly), L"Clearing the thumbnail cache...");
  EXPECT_TRUE(RebuildProgressText(RebuildPhase::kWaitingForExplorer).empty());
  EXPECT_TRUE(RebuildProgressText(RebuildPhase::kReopening).empty());
}

}  // namespace
}  // namespace pdft
