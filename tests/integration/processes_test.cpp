// Processes (processes.h) against the real system, on processes this test starts itself: Restart
// Manager finding a child that holds a file under %TEMP%\pdft-tests, ending that child only
// when every check passes (with this test program's own image on the allow list instead of the
// thumbnail process's), a child finding its parent, and this program's own parent found with a
// handle on it. Never ends a process it did not start: each holder is matched to the child's
// process id before anything is ended.
//
// A start made ready (ProgramStart) is tried everywhere only on an image that does not exist, so
// nothing is started. Only on a runner GitHub hosts is it tried for real, from copies of this test
// program put in jobs this test makes, with this test program as the image (never explorer.exe):
// what is started is created suspended, never runs, and is ended by the copy at once.

#include <windows.h>

#include "processes.h"

#include <gtest/gtest.h>
#include <wil/resource.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "child_process.h"
#include "github_runner.h"
#include "medium_il.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

using std::filesystem::path;

bool WriteFileText(const path& file, const std::string& text) {
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
  DWORD written = 0;
  return handle &&
         WriteFile(handle.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
         written == text.size();
}

const Holder* FindPid(const std::vector<Holder>& holders, DWORD pid) {
  for (const Holder& holder : holders) {
    if (holder.pid == pid) return &holder;
  }
  return nullptr;
}

TEST(Holders, NobodyHoldsANewFile) {
  test::TempDir dir(L"holders");
  ASSERT_TRUE(dir.ok());
  const path file = path(dir.path()) / L"free.txt";
  ASSERT_TRUE(WriteFileText(file, "free"));
  std::vector<Holder> holders;
  const path files[] = {file};
  ASSERT_EQ(FindHolders(files, &holders), S_OK);
  // Others may look at a new file for a moment (a virus scanner); this process does not hold it.
  EXPECT_EQ(FindPid(holders, GetCurrentProcessId()), nullptr);
  ASSERT_EQ(FindHolders({}, &holders), S_OK);
  EXPECT_TRUE(holders.empty());
}

TEST(Holders, FindsAndEndsOnlyItsOwnChild) {
  test::TempDir dir(L"holders");
  ASSERT_TRUE(dir.ok());
  const path file = path(dir.path()) / L"held.txt";
  ASSERT_TRUE(WriteFileText(file, "held"));
  test::PipedChild child;
  ASSERT_EQ(child.Start(test::SelfPath(),
                        L"--pdft-child hold-file " + test::QuoteArgument(file.native())),
            S_OK);
  ASSERT_EQ(child.ReadLine(), "held");

  std::vector<Holder> holders;
  const path files[] = {file};
  ASSERT_EQ(FindHolders(files, &holders), S_OK);
  const Holder* found = FindPid(holders, child.Pid());
  ASSERT_NE(found, nullptr) << holders.size() << " holder(s), none of them the child";
  const Holder holder = *found;
  DWORD ownSession = 0;
  ASSERT_TRUE(ProcessIdToSessionId(GetCurrentProcessId(), &ownSession));
  EXPECT_EQ(holder.session, ownSession);
  EXPECT_TRUE(IEquals(holder.image, test::SelfPath())) << WideToUtf8Lossy(holder.image);
  FILETIME created = {};
  FILETIME unused[3] = {};
  ASSERT_TRUE(GetProcessTimes(child.Process(), &created, &unused[0], &unused[1], &unused[2]));
  EXPECT_EQ(CompareFileTime(&created, &holder.start), 0);

  // The thumbnail process's allow list does not cover it.
  std::wstring surrogate;
  ASSERT_EQ(SurrogateImagePath(&surrogate), S_OK);
  const std::wstring_view onlySurrogate[] = {surrogate};
  EXPECT_EQ(TerminateOwnSessionHolder(holder, onlySurrogate), E_ACCESSDENIED);
  EXPECT_EQ(WaitForSingleObject(child.Process(), 0), static_cast<DWORD>(WAIT_TIMEOUT));

  // A holder whose start time no longer matches is someone else now.
  const std::wstring self = test::SelfPath();
  const std::wstring_view ownImage[] = {self};
  Holder stale = holder;
  stale.start.dwLowDateTime ^= 1;
  EXPECT_EQ(TerminateOwnSessionHolder(stale, ownImage), S_FALSE);
  EXPECT_EQ(WaitForSingleObject(child.Process(), 0), static_cast<DWORD>(WAIT_TIMEOUT));

  // With its image allowed, it is ended, and the file is free.
  EXPECT_EQ(TerminateOwnSessionHolder(holder, ownImage), S_OK);
  EXPECT_EQ(child.Wait(5000), 1u);
  ASSERT_EQ(FindHolders(files, &holders), S_OK);
  EXPECT_EQ(FindPid(holders, holder.pid), nullptr);
  EXPECT_EQ(TerminateOwnSessionHolder(holder, ownImage), S_FALSE);  // gone already
}

TEST(Holders, NeverThisProcess) {
  test::TempDir dir(L"holders");
  ASSERT_TRUE(dir.ok());
  const path file = path(dir.path()) / L"mine.txt";
  ASSERT_TRUE(WriteFileText(file, "mine"));
  wil::unique_hfile mine(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(mine);
  std::vector<Holder> holders;
  const path files[] = {file};
  ASSERT_EQ(FindHolders(files, &holders), S_OK);
  const Holder* found = FindPid(holders, GetCurrentProcessId());
  ASSERT_NE(found, nullptr);
  const std::wstring self = test::SelfPath();
  const std::wstring_view ownImage[] = {self};
  EXPECT_EQ(TerminateOwnSessionHolder(*found, ownImage), E_ACCESSDENIED);
}

TEST(Parent, AChildFindsThisProcess) {
  test::PipedChild child;
  ASSERT_EQ(child.Start(test::SelfPath(), L"--pdft-child parent"), S_OK);
  EXPECT_EQ(child.ReadLine(), "parent=" + std::to_string(GetCurrentProcessId()));
  EXPECT_EQ(child.Wait(10000), 0u);
}

// The parent comes with a handle on that very process, which keeps its id from passing to
// another one while it is open.
TEST(Parent, ComesWithAHandleOnIt) {
  DWORD parentId = 0;
  wil::unique_handle parent;
  const HRESULT hr = OpenParentProcess(&parentId, std::addressof(parent));
  if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
    GTEST_SKIP() << "the process that started this test program has ended";
  }
  ASSERT_EQ(hr, S_OK);
  ASSERT_TRUE(parent);
  EXPECT_NE(parentId, 0u);
  EXPECT_EQ(GetProcessId(parent.get()), parentId);
  DWORD found = 0;
  ASSERT_EQ(GetParentProcessId(&found), S_OK);
  EXPECT_EQ(found, parentId);
}

// ---- a start made ready (ProgramStart) ----

bool NotFound(HRESULT hr) {
  return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
         hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

// An image that is not there: the trial and the start fail as a real start would, or, where this
// process's job lets nothing leave it, say so without trying. Nothing is started either way.
TEST(ProgramStart, AStartThatCannotWorkStartsNothing) {
  test::TempDir dir(L"program-start");
  ASSERT_TRUE(dir.ok());
  const std::wstring missing = (path(dir.path()) / L"missing.exe").native();
  JobFacts own;
  ASSERT_EQ(ReadOwnJob(&own), S_OK);
  const bool forbidden = ChooseBreakaway(own) == BreakawayMode::kForbidden;

  std::unique_ptr<ProgramStart> start;
  ASSERT_EQ(ProgramStart::Prepare(missing, L"", &start), S_OK);
  ASSERT_TRUE(start);
  EXPECT_EQ(start->Job().inJob, own.inJob);
  EXPECT_EQ(start->Job().limitFlags, own.limitFlags);
  JobEscape escape = JobEscape::kLeaves;
  const HRESULT trial = start->Trial(&escape);
  if (forbidden) {
    EXPECT_EQ(trial, S_OK);
    EXPECT_EQ(escape, JobEscape::kForbidden);
  } else {
    EXPECT_TRUE(NotFound(trial)) << WideToUtf8Lossy(HResultText(trial));
  }
  EXPECT_EQ(start->Trial(&escape), E_ILLEGAL_METHOD_CALL);  // at most once

  std::unique_ptr<ProgramStart> second;
  ASSERT_EQ(ProgramStart::Prepare(missing, L"--x", &second), S_OK);
  bool jobForbids = !forbidden;
  const HRESULT started = second->Start(&jobForbids);
  if (forbidden) {
    EXPECT_EQ(started, HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    EXPECT_TRUE(jobForbids);
  } else {
    EXPECT_TRUE(NotFound(started)) << WideToUtf8Lossy(HResultText(started));
    EXPECT_FALSE(jobForbids);
  }
  EXPECT_EQ(second->Start(&jobForbids), E_ILLEGAL_METHOD_CALL);  // at most once
}

// Starts made ready in copies of this test program put in nested jobs this test makes, only on a
// runner GitHub hosts. The image started is this test program, suspended, and never runs.
//
// Under ctest this test process is always in a job (ctest's own, which lets the programs started
// from it leave silently). So whether a start must leave every job is decided by where a copy
// started from here lands: in no job, nothing above a job of this test's can keep what leaves it.
class NestedJobs : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!test::OnGitHubHostedRunner()) {
      GTEST_SKIP() << "not on a runner GitHub hosts (GITHUB_ACTIONS=true with "
                      "RUNNER_ENVIRONMENT=github-hosted): these start copies of this test "
                      "program inside jobs";
    }
    BOOL inJob = FALSE;
    ASSERT_TRUE(IsProcessInJob(GetCurrentProcess(), nullptr, &inJob));
    RecordProperty("test-process-in-job", inJob ? "yes" : "no");
    const test::CapturedRun own =
        test::RunCaptured(test::SelfPath(), L"--pdft-child own-job", 30000);
    ASSERT_EQ(own.hr, S_OK);
    ASSERT_EQ(own.out.rfind("in-job=", 0), 0u) << own.out << own.err;
    copyInJob_ = own.out.rfind("in-job=1", 0) == 0;
    RecordProperty("copy-in-job", copyInJob_ ? "yes" : "no");
  }

  // A job of this test's with |limits|; the job is ended with its last handle when it has
  // JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE.
  static wil::unique_handle MakeJob(DWORD limits) {
    wil::unique_handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) return job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = limits;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &info,
                                 sizeof(info))) {
      job.reset();
    }
    return job;
  }

  static JOBOBJECT_BASIC_ACCOUNTING_INFORMATION Accounting(HANDLE job) {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = {};
    (void)QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &info, sizeof(info),
                                    nullptr);
    return info;
  }

  // Whether no process of |job| is left within 2 s.
  static bool Emptied(HANDLE job) {
    for (int i = 0; i < 40; ++i) {
      if (Accounting(job).ActiveProcesses == 0) return true;
      Sleep(50);
    }
    return Accounting(job).ActiveProcesses == 0;
  }

  // Runs this test program's child mode |mode| on this test program, in |jobs| (outermost first).
  static test::CapturedRun RunIn(const wchar_t* mode, std::span<const HANDLE> jobs) {
    return test::RunCapturedInJobs(
        test::SelfPath(),
        L"--pdft-child " + std::wstring(mode) + L" " + test::QuoteArgument(test::SelfPath()), 30000,
        jobs);
  }

  // What to print when a count is off.
  static std::string Counts(HANDLE job, const test::CapturedRun& run) {
    const JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info = Accounting(job);
    return "total=" + std::to_string(info.TotalProcesses) +
           " active=" + std::to_string(info.ActiveProcesses) + " out=" + run.out +
           " err=" + run.err;
  }

  bool copyInJob_ = false;  // a copy of this test program started from here lands in a job
};

// Skips the test where a job of this test's cannot be nested here; fails it where the copy did
// not run.
#define SKIP_UNLESS_ASSIGNED(run)                                  \
  if (!(run).assigned) {                                           \
    GTEST_SKIP() << "a job of this test's cannot be nested here: " \
                 << WideToUtf8Lossy(HResultText((run).hr));        \
  }                                                                \
  ASSERT_EQ((run).hr, S_OK)

TEST_F(NestedJobs, NothingLeavesAJobThatForbidsIt) {
  const wil::unique_handle j1 = MakeJob(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
  ASSERT_TRUE(j1) << "cannot make a job: " << GetLastError();
  const HANDLE jobs[] = {j1.get()};
  const test::CapturedRun run = RunIn(L"job-escape", jobs);
  SKIP_UNLESS_ASSIGNED(run);
  EXPECT_EQ(run.out.rfind("escape=forbidden hr=0x00000000", 0), 0u) << Counts(j1.get(), run);
  // No trial: the copy was the only process in the job.
  EXPECT_EQ(Accounting(j1.get()).TotalProcesses, 1u) << Counts(j1.get(), run);
}

TEST_F(NestedJobs, AJobAboveKeepsWhatLeavesTheImmediateOne) {
  const wil::unique_handle j0 = MakeJob(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
  const wil::unique_handle j1 = MakeJob(JOB_OBJECT_LIMIT_BREAKAWAY_OK);
  ASSERT_TRUE(j0 && j1) << "cannot make a job: " << GetLastError();
  const HANDLE jobs[] = {j0.get(), j1.get()};
  const test::CapturedRun run = RunIn(L"job-escape", jobs);
  SKIP_UNLESS_ASSIGNED(run);
  EXPECT_EQ(run.out.rfind("escape=stays hr=0x00000000", 0), 0u) << Counts(j0.get(), run);
  // The trial ran in the job above, and was ended.
  EXPECT_EQ(Accounting(j0.get()).TotalProcesses, 2u) << Counts(j0.get(), run);
  EXPECT_TRUE(Emptied(j0.get())) << Counts(j0.get(), run);
}

TEST_F(NestedJobs, SilentBreakawayStaysInTheJobAbove) {
  const wil::unique_handle j0 = MakeJob(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
  const wil::unique_handle j1 = MakeJob(JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK);
  ASSERT_TRUE(j0 && j1) << "cannot make a job: " << GetLastError();
  const HANDLE jobs[] = {j0.get(), j1.get()};
  const test::CapturedRun run = RunIn(L"job-escape", jobs);
  SKIP_UNLESS_ASSIGNED(run);
  EXPECT_EQ(run.out.rfind("escape=stays hr=0x00000000", 0), 0u) << Counts(j0.get(), run);
  EXPECT_TRUE(Emptied(j0.get())) << Counts(j0.get(), run);
}

TEST_F(NestedJobs, LeavesAJobThatLetsIt) {
  const wil::unique_handle j1 =
      MakeJob(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK);
  ASSERT_TRUE(j1) << "cannot make a job: " << GetLastError();
  const HANDLE jobs[] = {j1.get()};
  const test::CapturedRun run = RunIn(L"job-escape", jobs);
  SKIP_UNLESS_ASSIGNED(run);
  if (copyInJob_) {
    // The job such a copy lands in is above this test's job, and may keep it: only recorded.
    RecordProperty("escape-breakaway-ok", run.out);
  } else {
    EXPECT_EQ(run.out.rfind("escape=leaves hr=0x00000000", 0), 0u) << Counts(j1.get(), run);
  }
  EXPECT_TRUE(Emptied(j1.get())) << Counts(j1.get(), run);
}

TEST_F(NestedJobs, OutsideEveryJobItLeaves) {
  const test::CapturedRun run = test::RunCaptured(
      test::SelfPath(), L"--pdft-child job-escape " + test::QuoteArgument(test::SelfPath()), 30000);
  ASSERT_EQ(run.hr, S_OK);
  if (copyInJob_) {
    // Not outside every job after all: whatever its answer, the trial must not fail.
    RecordProperty("escape-no-test-job", run.out);
    EXPECT_NE(run.out.find(" hr=0x00000000"), std::string::npos) << run.out << run.err;
  } else {
    EXPECT_EQ(run.out.rfind("escape=leaves hr=0x00000000", 0), 0u) << run.out << run.err;
  }
}

// The real start never leaves what it started inside a job: it is ended before it runs.
TEST_F(NestedJobs, TheStartNeverLeavesItInAJob) {
  const wil::unique_handle j0 = MakeJob(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
  const wil::unique_handle j1 = MakeJob(JOB_OBJECT_LIMIT_BREAKAWAY_OK);
  ASSERT_TRUE(j0 && j1) << "cannot make a job: " << GetLastError();
  const HANDLE jobs[] = {j0.get(), j1.get()};
  const test::CapturedRun run = RunIn(L"start-outside", jobs);
  SKIP_UNLESS_ASSIGNED(run);
  EXPECT_EQ(run.out.rfind("forbids=1 hr=0x80070005", 0), 0u) << Counts(j0.get(), run);
  EXPECT_TRUE(Emptied(j0.get())) << Counts(j0.get(), run);
}

// What CREATE_BREAKAWAY_FROM_JOB gives where the immediate job forbids leaving, and where it only
// lets programs leave silently: recorded only.
TEST_F(NestedJobs, TheBreakawayFlagIsRecorded) {
  {
    const wil::unique_handle j1 = MakeJob(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
    ASSERT_TRUE(j1) << "cannot make a job: " << GetLastError();
    const HANDLE jobs[] = {j1.get()};
    const test::CapturedRun run = RunIn(L"breakaway-flag", jobs);
    SKIP_UNLESS_ASSIGNED(run);
    RecordProperty("breakaway-flag-kill", run.out);
    std::printf("breakaway flag in a kill-on-close job: %s", run.out.c_str());
  }
  {
    const wil::unique_handle j0 = MakeJob(JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE);
    const wil::unique_handle j1 = MakeJob(JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK);
    ASSERT_TRUE(j0 && j1) << "cannot make a job: " << GetLastError();
    const HANDLE jobs[] = {j0.get(), j1.get()};
    const test::CapturedRun run = RunIn(L"breakaway-flag", jobs);
    SKIP_UNLESS_ASSIGNED(run);
    RecordProperty("breakaway-flag-silent", run.out);
    std::printf("breakaway flag in a silent-breakaway job: %s", run.out.c_str());
  }
}

}  // namespace
}  // namespace pdft
