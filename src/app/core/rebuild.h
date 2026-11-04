// Rebuilding the thumbnail cache (--refresh --full, --uninstall --rebuild-thumbnails): File
// Explorer and the thumbnail processes of this session and user that hold the cache are ended,
// %LOCALAPPDATA%\Microsoft\Windows\Explorer\thumbcache_*.db are deleted, Windows starts File
// Explorer again, and the folder windows that were open are opened again. Everything before the
// first process is ended (or the first file deleted) only reads; after that the run finishes.
//
// Refused (exit 8) elevated, where the registry may be virtualized, where File Explorer on this
// desktop belongs to another account (the cache, the windows and the File Explorer that Windows
// restarts are that account's), and below an app package where Windows does not restart File
// Explorer by itself (this program never starts it from inside a package); and, where Windows
// does not restart File Explorer by itself, where a File Explorer started from here would stay
// inside a job (8) or cannot be started from here at all (5): checked by starting explorer.exe
// suspended and ending it before it runs. A process that has the cache open itself cannot free it
// (exit 5).
//
// What may be ended: a process that holds a cache file, of this session and this user, whose
// image is <Windows>\explorer.exe or the thumbnail process (<System32>\dllhost.exe,
// <SysWOW64>\dllhost.exe), still the very process Restart Manager found; checked from Restart
// Manager's record (ClassifyHolder), then again through a handle on the process
// (TerminateOwnSessionHolder). Thumbnail processes go first, then File Explorer processes that are
// not the shell, then the shell, and the files are deleted at once. Programs left alone (Windows
// Search, file dialogs of other programs, other accounts) may keep cache files; those stay until
// the next sign-in.
//
// What is logged: counts, HRESULTs and milliseconds. Never a path, a folder window, an image
// name or Restart Manager's name for a program; the console shows image leaf names only.

#pragma once

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "context.h"
#include "exit_code.h"
#include "file_ops.h"
#include "processes.h"
#include "shell_windows.h"

namespace pdft {

// How long Windows gets to bring File Explorer back, and File Explorer to answer.
inline constexpr DWORD kShellWaitMs = 5000;

// At most this many folder windows are opened again.
inline constexpr size_t kMaxReopenedWindows = 25;

// thumbcache_<letters, digits or _>.db, ignoring case (pure); never iconcache_*, never a
// longer extension or a stream name.
bool IsThumbnailCacheFileName(std::wstring_view name) noexcept;

// Why the rebuild did not start (nothing changed).
enum class RebuildRefusal {
  kNone,
  kElevated,               // 8
  kVirtualized,            // 8: or the registry check did not run
  kOtherShellUser,         // 8: File Explorer on this desktop is another account's
  kNoRestartBelowPackage,  // 8: below an app package, and Windows does not restart Explorer
  kNoRestartInJob,         // 8: a File Explorer started from here would stay in a job, and
                           //    Windows does not restart it
  kNoRestartStartFails,    // 5: ... and this program cannot start it either
  kOwnProcessHoldsCache,   // 5
  kFileOperation,          // 1: File Explorer shows a copy, move or delete in progress
  kCancelled,              // 3: stopped before anything changed
};

// Whether a File Explorer started from this process would run outside every job: checked only
// where Windows does not restart File Explorer by itself and no app package is above.
enum class ExplorerStartCheck {
  kNotChecked,
  kOutsideJobs,  // the trial process was in no job
  kInsideJob,    // it stayed in a job, or this process's job lets nothing leave it
  kFails,        // the start could not be made ready, or the trial failed
};

struct RebuildConditions {
  bool elevated = false;
  bool writesChecked = false;   // the registry canary ran (ContextProbe::kWrite)
  bool virtualized = false;     // RefusesWrites(context.virtualization)
  bool otherShellUser = false;  // context.differentFromShellUser
  bool belowPackage = false;    // context.packagedAncestor
  bool autoRestartShell = true;
  ExplorerStartCheck explorerStart = ExplorerStartCheck::kNotChecked;
};

// The conditions of |context| (pure).
RebuildConditions RebuildConditionsOf(
    const ProcessContext& context, bool autoRestartShell,
    ExplorerStartCheck explorerStart = ExplorerStartCheck::kNotChecked) noexcept;

// The first refusal in this order (pure): elevated, virtualized or not checked, other shell user;
// then, where Windows will not restart File Explorer: below a package, then the start could not
// be made ready or tried, then File Explorer would stay in a job (kNotChecked counts as that: the
// trial must come first).
RebuildRefusal DecideRebuildRefusal(const RebuildConditions& c) noexcept;

// The log's word for |refusal| (pure): "elevated", "other-shell-user" and so on; "none" for kNone.
const wchar_t* RebuildRefusalToken(RebuildRefusal refusal) noexcept;

// The desktop's shell (the process that owns GetShellWindow).
struct ShellProcess {
  DWORD pid = 0;
  FILETIME start = {};
};

enum class HolderRole {
  kShell,         // File Explorer, the desktop's shell
  kFileExplorer,  // File Explorer, another process of it (folder windows of their own)
  kSurrogate,     // the thumbnail process
  kSelf,          // this process
  kLeftAlone,     // anything else: another session, an unreadable or another image
};

// What a holder is to the rebuild, from Restart Manager's record only (pure); images compare as
// whole paths (SameImagePath). TerminateOwnSessionHolder checks again through a handle.
HolderRole ClassifyHolder(const Holder& holder, DWORD ownPid, DWORD ownSession,
                          const std::optional<ShellProcess>& shell,
                          const RebuildImages& images) noexcept;

// Indexes of the holders to end, each process once (process id and start time): thumbnail
// processes, then File Explorer processes, then the shell (pure). |roles| goes with |holders|.
// May throw std::bad_alloc.
std::vector<size_t> EndingOrder(std::span<const Holder> holders, std::span<const HolderRole> roles);

// What became of the shell.
enum class ShellOutcome {
  kNotEnded,  // it was not ended (none, a check refused it, or ending it failed): it still runs
  kBack,      // Windows started it again
  kStarted,   // this program started it
  kNotBack,   // there is no shell now
};

// Whether this program started File Explorer.
enum class ShellStart {
  kNotTried,
  kStarted,
  kBelowPackage,  // not from inside an app package
  kJobForbids,    // it would have stayed inside a job (this process's, or one above it): ended
  kFailed,        // also: the start could not be made ready before anything changed
};

// Where the wait for the shell stands.
struct ShellWaitState {
  bool autoRestart = true;  // Windows restarts File Explorer by itself
  bool belowPackage = false;
  DWORD limitMs = kShellWaitMs;  // one wait
  // Since the shell ended, since the wait was extended, or since this program started File
  // Explorer.
  ULONGLONG waitedMs = 0;
  bool back = false;                // a new shell is there
  bool newExplorerRunning = false;  // a File Explorer of this user started since, no shell yet
  bool extended = false;            // the wait was extended once
  bool startTried = false;
};

enum class ShellStep {
  kDone,    // the shell is back
  kWait,    // look again shortly
  kExtend,  // a new File Explorer is still starting: wait once more, never start a second one
  kStart,   // start File Explorer
  kGiveUp,  // no shell, and this program starts none (below a package, or one is still starting)
};

// The next step (pure).
ShellStep NextShellStep(const ShellWaitState& s) noexcept;

struct ReopenPlan {
  std::vector<FolderWindow> open;  // to open, at most kMaxReopenedWindows
  size_t stillOpen = 0;            // saved windows that are open now
  size_t overCap = 0;              // past kMaxReopenedWindows
  size_t unnamed = 0;              // saved without a parsing name: cannot be opened
  bool tabs = false;  // some of |open| were tabs of one frame: they come back as windows
};

// The saved windows minus those open now (by parsing name, ignoring case, each window open now
// matching one saved window), at most kMaxReopenedWindows (pure). May throw std::bad_alloc.
ReopenPlan PlanReopen(std::span<const FolderWindow> saved, std::span<const FolderWindow> now);

// Whether the folder windows were listed before File Explorer was ended.
enum class WindowListing {
  kNotNeeded,  // no File Explorer process is ended, or there was no shell
  kListed,
  kUnanswered,  // the shell did not answer in time (or the listing failed): not reopened
};

struct RebuildResult {
  RebuildRefusal refusal = RebuildRefusal::kNone;
  // What kept it from starting (reading AutoRestartShell, the cache folder, Restart Manager, the
  // images): nothing changed (exit 5).
  HRESULT failure = S_OK;
  // Past the point of no return: it went on to end a process or delete a file.
  bool committed = false;

  // What it found before it changed anything.
  bool autoRestartShell = true;
  bool belowPackage = false;
  ExplorerStartCheck explorerStart = ExplorerStartCheck::kNotChecked;
  std::optional<JobEscape> jobEscape;  // the trial's answer, when it ran
  std::optional<JobFacts> job;         // this process's job, read when a start was made ready
  bool startReady = false;             // a start of File Explorer was made ready
  size_t cacheFiles = 0;
  size_t holders = 0;  // processes that had a cache file open
  size_t toEnd = 0;    // ... of them, the ones it ends
  bool shellFound = false;
  WindowListing listing = WindowListing::kNotNeeded;
  size_t windows = 0;  // folder windows listed (each tab counts)
  size_t frames = 0;   // ... in that many top-level windows

  // Ending.
  size_t ended = 0;
  size_t gone = 0;        // ended on its own meanwhile, or replaced
  size_t endRefused = 0;  // a check refused it on the second look: left alone
  size_t endFailed = 0;
  HRESULT firstEndError = S_OK;

  // Deleting.
  size_t deleted = 0;
  size_t inUse = 0;    // still in use after the retries
  size_t failed = 0;   // not a regular file, or another error
  size_t retried = 0;  // rounds of retries for files in use
  HRESULT firstFileError = S_OK;
  ULONGLONG deleteMs = 0;
  // Image leaf names ("SearchHost.exe") of this session's programs that keep cache files in use,
  // sorted and each once: for the console, never the log. Those of the images the rebuild ends
  // (File Explorer and the thumbnail process: one it could not end, one a check refused, or one
  // started since) are in inUseByNotEnded, the others in inUseBy.
  std::vector<std::wstring> inUseBy;
  std::vector<std::wstring> inUseByNotEnded;
  bool inUseByOthers = false;  // ... and programs of other accounts or sessions

  // File Explorer.
  ShellOutcome shell = ShellOutcome::kNotEnded;
  ShellStart start = ShellStart::kNotTried;
  // Why the start failed; before anything changed, why it could not be made ready or tried
  // (kNoRestartStartFails, or no refusal where Windows restarts File Explorer by itself).
  HRESULT startError = S_OK;
  ULONGLONG shellMs = 0;

  // Opening the folder windows again.
  bool reopenTried = false;
  size_t stillOpen = 0;
  size_t reopened = 0;
  size_t reopenFailed = 0;  // the shell did not open them, or they had no parsing name
  size_t overCap = 0;
  bool tabs = false;
};

// The exit code (pure): 8 for the context refusals and a File Explorer that would stay in a job;
// 5 for a failure, a File Explorer this program cannot start where Windows does not, or this
// process holding the cache; 3 when stopped; 1 for a copy in progress, a shell that is not back,
// files or processes that failed, or no file deleted of those there were; 0 otherwise (files kept
// by programs left alone, windows not reopened and an empty cache included).
ExitCode RebuildExitCode(const RebuildResult& r) noexcept;

// What the rebuild did, line by line (pure), for a result that is neither a refusal nor a
// failure (those have RebuildRefusalText and RebuildFailureText), with a line for each reason it
// exits 1. Programs by image leaf name only. May throw std::bad_alloc.
std::vector<std::wstring> RebuildReport(const RebuildResult& r);

// Why it was refused (pure), for standard error; |context| names the account and the package,
// |detail| why File Explorer cannot be started from here (kNoRestartStartFails). Empty for kNone.
// May throw std::bad_alloc.
std::wstring RebuildRefusalText(RebuildRefusal refusal, const ProcessContext& context,
                                HRESULT detail = S_OK);

// Why it could not start (pure): a link on the way to the cache folder, or another failure. May
// throw std::bad_alloc.
std::wstring RebuildFailureText(HRESULT failure);

// The question before a rebuild, shared with the settings window (pure; without " [y/N]").
std::wstring RebuildQuestion();

// Where a rebuild is, past the point of no return.
enum class RebuildPhase {
  kEnding,              // ending File Explorer and thumbnail processes, then deleting
  kDeletingOnly,        // deleting; nothing is ended
  kWaitingForExplorer,  // waiting for File Explorer to come back
  kReopening,           // opening the folder windows again
};

// The progress line for |phase| (pure); empty for a phase that has none. May throw
// std::bad_alloc.
std::wstring RebuildProgressText(RebuildPhase phase);

// A start of File Explorer made ready before anything changes (ProgramStart; the tests' fake).
class PreparedExplorerStart {
 public:
  virtual ~PreparedExplorerStart() = default;
  // ProgramStart::Job.
  virtual const JobFacts& Job() const noexcept = 0;
  // ProgramStart::Trial.
  [[nodiscard]] virtual HRESULT Trial(JobEscape* escape) noexcept = 0;
  // ProgramStart::Start.
  [[nodiscard]] virtual HRESULT Start(bool* jobForbids) noexcept = 0;
};

// What the rebuild asks Windows: the real one (RealRebuildSystem) and the tests' fake.
class RebuildSystem {
 public:
  virtual ~RebuildSystem() = default;
  // ReadAutoRestartShell.
  [[nodiscard]] virtual HRESULT AutoRestartShell(bool* on) const noexcept = 0;
  // The cache folder's thumbcache files (ListThumbnailCacheFiles): S_FALSE when the folder is
  // missing; a failure when it or a folder above it up to %LOCALAPPDATA% is a link.
  [[nodiscard]] virtual HRESULT CacheFiles(
      std::vector<std::filesystem::path>* files) const noexcept = 0;
  // FindHolders (Restart Manager).
  [[nodiscard]] virtual HRESULT FindHolders(std::span<const std::filesystem::path> files,
                                            std::vector<Holder>* holders) const noexcept = 0;
  // GetRebuildImages.
  [[nodiscard]] virtual HRESULT Images(RebuildImages* images) const noexcept = 0;
  // The desktop's shell now; std::nullopt when there is none (or it cannot be read).
  virtual std::optional<ShellProcess> Shell() const noexcept = 0;
  // Whether a File Explorer process of this session and user started after |since|.
  virtual bool ExplorerStartedSince(const FILETIME& since) const noexcept = 0;
  // The time now (UTC), as process start times are given.
  virtual FILETIME SystemTime() const noexcept = 0;
  // ListFolderWindows.
  [[nodiscard]] virtual HRESULT ListFolderWindows(
      DWORD timeoutMs, std::vector<FolderWindow>* windows) const noexcept = 0;
  // Whether one of |explorerPids| shows a copy, move or delete in progress.
  virtual bool FileOperationShown(std::span<const DWORD> explorerPids) const noexcept = 0;
  // TerminateOwnSessionHolder with |images| as the images allowed.
  [[nodiscard]] virtual HRESULT EndHolder(const Holder& holder,
                                          const RebuildImages& images) const noexcept = 0;
  // DeleteRegularFile.
  [[nodiscard]] virtual HRESULT DeleteCacheFile(const std::filesystem::path& file,
                                                ContentDelete* result) const noexcept = 0;
  // ProgramStart::Prepare of <Windows>\explorer.exe (GetRebuildImages), without arguments.
  [[nodiscard]] virtual HRESULT PrepareExplorerStart(
      std::unique_ptr<PreparedExplorerStart>* start) const noexcept = 0;
  // OpenFolderWindows.
  [[nodiscard]] virtual HRESULT OpenFolderWindows(std::span<const FolderWindow> windows,
                                                  DWORD timeoutMs, size_t* opened,
                                                  size_t* failed) const noexcept = 0;
  virtual void Wait(DWORD ms) const noexcept = 0;
  // A clock in milliseconds (GetTickCount64).
  virtual ULONGLONG NowMs() const noexcept = 0;
};

// The real answers: the functions of the same names.
const RebuildSystem& RealRebuildSystem() noexcept;

// What the rebuild decides before its question and before anything changes.
struct RebuildPrecheck {
  RebuildConditions conditions;
  RebuildRefusal refusal = RebuildRefusal::kNone;
  HRESULT failure = S_OK;           // AutoRestartShell could not be read: exit 5
  std::optional<JobFacts> job;      // this process's job, read when a start was made ready
  std::optional<JobEscape> escape;  // the trial's answer, when it ran
  // Why the start could not be made ready or tried: with AutoRestartShell 0
  // kNoRestartStartFails, otherwise no refusal (the fallback reports it).
  HRESULT startError = S_OK;
};

// The context's refusals (nothing is read for them); then AutoRestartShell; then, with no app
// package above, the start is made ready where AutoRestartShell is 0 or |keep| is given, and
// tried where it is 0. S_OK with *check whenever it got that far; fails only when out of memory.
// With |keep| and no refusal, the start made ready goes there.
[[nodiscard]] HRESULT PrecheckRebuild(
    const ProcessContext& context, const RebuildSystem& system, RebuildPrecheck* check,
    std::unique_ptr<PreparedExplorerStart>* keep = nullptr) noexcept;

// The result of a rebuild that |check| refused or could not finish, with nothing asked again
// (pure): refusal, failure, the conditions, job, escape and startError copied; nothing committed.
RebuildResult RebuildNotStarted(const RebuildPrecheck& check);

// The log line for a refusal or failure |check| found (pure; tokens and HRESULTs only). May throw
// std::bad_alloc.
std::wstring RebuildPrecheckLogText(const RebuildPrecheck& check);

// The thumbcache files in |folder| (IsThumbnailCacheFileName on the long name; no folder, no
// link), after CheckNoReparsePoints(|folder|, |stopAt|). S_FALSE with none when |folder| does not
// exist. Read-only.
[[nodiscard]] HRESULT ListThumbnailCacheFiles(const std::filesystem::path& folder,
                                              const std::filesystem::path& stopAt,
                                              std::vector<std::filesystem::path>* files) noexcept;

struct RebuildOptions {
  ProcessContext context;  // the canary's (ContextProbe::kWrite)
  DWORD shellWaitMs = kShellWaitMs;
  bool reopenWindows = true;
};

using RebuildProgressFn = std::function<void(RebuildPhase)>;

// Rebuilds the thumbnail cache (see the top of this file). The caller holds the operation lock
// and has not used the thumbnail cache in this process (the Explorer test may leave a process
// holding it: kOwnProcessHoldsCache). |stop| is honoured only before the point of no return
// (kCancelled); after it the run finishes. |progress| (optional) hears each phase past that
// point; what it throws is ignored. S_OK whenever it reached a result (*result, refusals and
// failures included); a failure only when out of memory before anything changed. Logs its
// result (component "refresh").
[[nodiscard]] HRESULT RebuildThumbnailCache(const RebuildOptions& options,
                                            const RebuildSystem& system,
                                            const RebuildProgressFn& progress, std::stop_token stop,
                                            RebuildResult* result) noexcept;

}  // namespace pdft
