// Other processes: which ones have a file open or loaded (Restart Manager), ending one of them
// when it is safe, and this process's parent.
//
// Windows' thumbnail process (dllhost.exe, the COM surrogate) keeps a thumbnail handler's DLLs
// loaded for as long as it lives. Before an install replaces a build, the processes of the
// user's own session that hold the OLD build's DLLs are ended, so the new build is loaded the
// next time Explorer asks for a thumbnail. Only processes that pass every check are ended: the
// image on an allow list (normally just %SystemRoot%\System32\dllhost.exe), the same session,
// the same user, and still the very process that was found (same process id and start time).
// Rebuilding the thumbnail cache allows File Explorer too (GetRebuildImages), and starts it
// again where Windows does not (ProgramStart), never leaving it inside a job.

#pragma once

#include <windows.h>

// userenv.h before wil/resource.h enables WIL's environment block wrapper (ProgramStart).
#include <RestartManager.h>
#include <userenv.h>
#include <wil/resource.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pdft {

// A process that has one of the files open or loaded.
struct Holder {
  DWORD pid = 0;
  FILETIME start = {};  // when it started, to tell it from a later process with the same id
  DWORD session = 0;    // its Terminal Services session
  RM_APP_TYPE type = RmUnknownApp;
  std::wstring app;    // Restart Manager's name for it (a service name, a window title, ...)
  std::wstring image;  // its executable's full path; empty when it cannot be read (another
                       // user's or an elevated process)
};

// Every process that has one of |files| open or loaded, from Restart Manager. A process found
// but gone, or replaced by another one with the same id, by the time it is looked at is left out.
// An empty list of files gives no holders.
[[nodiscard]] HRESULT FindHolders(std::span<const std::filesystem::path> files,
                                  std::vector<Holder>* holders) noexcept;

// The image an install may end a holder of: <System folder>\dllhost.exe, the thumbnail process.
[[nodiscard]] HRESULT SurrogateImagePath(std::wstring* image) noexcept;

// Whether |a| and |b| are the same path (pure): whole paths, ignoring case, after "." and ".."
// are resolved; a relative |a| never matches. No file system access, so a link or a short (8.3)
// name is not the same as where it leads.
bool SameImagePath(std::wstring_view a, std::wstring_view b) noexcept;

// What TerminateOwnSessionHolder knows about a process when it decides.
struct HolderFacts {
  DWORD ownPid = 0;
  DWORD ownSession = 0;
  DWORD session = 0;      // the process's session now
  FILETIME start = {};    // its start time now
  std::wstring image;     // its image now
  bool sameUser = false;  // its token's user is this process's user
};

// Why a holder is not ended.
enum class HolderRefusal {
  kNone,          // it may be ended
  kSelf,          // it is this process
  kReplaced,      // its start time changed: the process found is gone, the id reused
  kOtherSession,  // another session (another signed-in user, or a service)
  kOtherUser,     // another account's process, or its user cannot be read
  kNotAllowed,    // its image is not on the allow list (or was not read)
};

// The decision for |found| (pure): the checks in the order of the enum. Images compare as whole
// paths, ignoring case, after "." and ".." are resolved.
HolderRefusal JudgeHolder(const Holder& found, const HolderFacts& now,
                          std::span<const std::wstring_view> allowedImages) noexcept;

// Ends the process |holder| (FindHolders) after reading its facts again and judging them
// (JudgeHolder), and waits up to 5 s for it to go. S_FALSE when it is gone already (or replaced
// by another process with its id); E_ACCESSDENIED when a check refuses it, and it is left alone.
[[nodiscard]] HRESULT TerminateOwnSessionHolder(
    const Holder& holder, std::span<const std::wstring_view> allowedImages) noexcept;

// ---- Rebuilding the thumbnail cache (rebuild.h) -------------------------------------------

// The images the thumbnail-cache rebuild may end: File Explorer and the COM surrogate (the
// thumbnail process). |surrogateWow64| is empty where Windows has no WOW64.
struct RebuildImages {
  std::wstring explorer;        // <System Windows folder>\explorer.exe
  std::wstring surrogate;       // <System folder>\dllhost.exe
  std::wstring surrogateWow64;  // <SysWOW64>\dllhost.exe
};
[[nodiscard]] HRESULT GetRebuildImages(RebuildImages* images) noexcept;

// Whether a process of |image| (a whole path, compared as SameImagePath does) runs in this
// process's session as this process's user and started after |since| (UTC), from a process
// snapshot. Processes that cannot be read do not count.
bool OwnProcessStartedSince(std::wstring_view image, const FILETIME& since) noexcept;

// ---- Jobs, and starting a program outside them --------------------------------------------

// How a program started from this process leaves this process's job.
enum class BreakawayMode {
  kNotInJob,   // this process is in no job
  kAsk,        // CREATE_BREAKAWAY_FROM_JOB: the job sets JOB_OBJECT_LIMIT_BREAKAWAY_OK, or its
               // limits could not be read
  kSilent,     // only JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK: leaving needs no flag
  kForbidden,  // neither: nothing started from here leaves this job
};

// This process's immediate job, as far as it can be read; the jobs above it cannot be.
struct JobFacts {
  bool inJob = false;
  std::optional<DWORD> limitFlags;  // its LimitFlags; std::nullopt when they could not be read
};

// Reads this process's immediate job. Limits that cannot be read are no failure (std::nullopt).
[[nodiscard]] HRESULT ReadOwnJob(JobFacts* facts) noexcept;

// The mode for |facts| (pure).
BreakawayMode ChooseBreakaway(const JobFacts& facts) noexcept;

// Whether a start from this process that failed with |error| was refused by this process's job
// (pure): ERROR_ACCESS_DENIED where the start asked to leave a job whose limits could not be
// read. A job whose limits let programs leave it cannot refuse: access denied there comes from
// the image (its permissions, or a security product that blocks the start).
bool JobRefusedStart(const JobFacts& facts, DWORD error) noexcept;

// Whether a program started from this process without a breakaway flag (as ShellExecuteExW
// starts one) is ended when this process's job closes (pure): the job sets
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE and not JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK, or its limits
// could not be read. Jobs above it are not seen.
bool ChildEndsWithJob(const JobFacts& facts) noexcept;

// For the log (pure): "none", "unread", or the limits BREAKAWAY_OK, SILENT_BREAKAWAY_OK and
// KILL_ON_JOB_CLOSE of |facts| as 0x........ (the others masked out). May throw std::bad_alloc.
std::wstring JobFactsToken(const JobFacts& facts);

// Where a program started from this process would run, as far as jobs go.
enum class JobEscape {
  kLeaves,     // the trial process was in no job
  kForbidden,  // this process's job lets nothing leave it (nothing started, or the start refused)
  kStays,      // the trial process was in a job: one above this process's own job kept it
};

// A start of a program made ready before it is needed, so that starting it reads and allocates
// nothing more: as this process's user, with a fresh environment from the user's profile (not
// this process's: a terminal's variables must not reach what the user starts from File
// Explorer), nothing inherited, in System32, and outside every job (also one above this
// process's own job) or not at all. The rebuild never uses it elevated or below an app package;
// nothing in it depends on that.
class ProgramStart {
 public:
  ProgramStart(const ProgramStart&) = delete;
  ProgramStart& operator=(const ProgramStart&) = delete;
  ~ProgramStart();

  // Gathers what the start needs: System32, userenv.dll, this process's token and the
  // environment block made from it, one command line for Trial and one for Start
  // ("<image>" <arguments>), and this process's job (ReadOwnJob).
  [[nodiscard]] static HRESULT Prepare(std::wstring image, std::wstring arguments,
                                       std::unique_ptr<ProgramStart>* start) noexcept;

  const JobFacts& Job() const noexcept { return job_; }

  // Whether the start would run outside every job. Unless this process's job lets nothing leave
  // it, the image is started suspended (so it runs no code and opens no window) without the busy
  // cursor; its jobs are read, and it is ended and waited for at once. Fails when that start
  // failed for another reason than the job (JobRefusedStart; a real start would fail too), and
  // when the trial process could not be ended or did not end within 5 s. At most once.
  [[nodiscard]] HRESULT Trial(JobEscape* escape) noexcept;

  // Starts it: created suspended, and resumed only when it is in no job. Otherwise it is ended
  // before it runs, *jobForbids is true, and this fails with
  // HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED); *jobForbids is also true when the job refused the
  // start (JobRefusedStart). Does not wait for it. At most once.
  [[nodiscard]] HRESULT Start(bool* jobForbids) noexcept;

 private:
  ProgramStart() = default;

  std::wstring image_;
  std::filesystem::path system32_;
  wil::unique_environment_block environment_;
  std::wstring trialCommandLine_;
  std::wstring startCommandLine_;
  JobFacts job_;
  BreakawayMode mode_ = BreakawayMode::kNotInJob;
  bool tried_ = false;
  bool started_ = false;
};

// ---- This process's parent ----------------------------------------------------------------

// This process's parent: the process id its creator had, provided that process still exists
// and was created before this one (so the id was not reused). Fails with
// HRESULT_FROM_WIN32(ERROR_NOT_FOUND) otherwise.
[[nodiscard]] HRESULT GetParentProcessId(DWORD* parent) noexcept;

// The same parent, with the handle (PROCESS_QUERY_LIMITED_INFORMATION) its creation time was
// checked through. While |parent| stays open, |parentId| cannot pass to another process, so
// whatever is checked against the id meanwhile is checked against this parent.
[[nodiscard]] HRESULT OpenParentProcess(DWORD* parentId, wil::unique_handle* parent) noexcept;

// ---- Starting a program from outside an app package ---------------------------------------

// Starts |image| (a full path, no arguments) in |folder| through WMI (Win32_Process.Create): the
// WMI service creates it as this user in this session, outside the app package and the jobs this
// process runs in. Runs on a thread of its own and waits at most |timeoutMs|, so a hung WMI cannot
// hang the caller. *pid: the new process.
[[nodiscard]] HRESULT StartThroughWmi(const std::filesystem::path& image,
                                      const std::filesystem::path& folder, DWORD timeoutMs,
                                      DWORD* pid) noexcept;

}  // namespace pdft
