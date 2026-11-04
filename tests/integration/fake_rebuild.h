// A fake RebuildSystem (rebuild.h) for the scenario tests: it touches no real process, file,
// window or clock. The test lays out a world (the cache files, who holds them, the shell, the
// folder windows, and what Windows does when the shell ends), runs RebuildThumbnailCache against
// it, and reads back what the engine asked: the order processes were ended in, the files deleted,
// whether a start of File Explorer was made ready, tried and used, and the windows opened again.
// The clock only moves when the engine calls Wait, so the waits are exact and instant. Holders get
// process ids from FakePid, never this process's own, which the engine (using the real one)
// would take for itself.

#pragma once

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include "processes.h"
#include "rebuild.h"
#include "shell_windows.h"

namespace pdft::test {

// A FILETIME far enough in the past that times the fake makes for "later" never overflow.
inline constexpr ULONGLONG kFakeBaseTicks = 130000000000000000ULL;  // ~ year 2013

// The |n|th process id for a fake process: a valid id (a multiple of 4 away from this process's)
// that is never this process's own.
inline DWORD FakePid(DWORD n) { return GetCurrentProcessId() + 4 * n; }

inline FILETIME TicksToTime(ULONGLONG ticks) {
  FILETIME value = {};
  value.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFF);
  value.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return value;
}

class FakeRebuildSystem final : public RebuildSystem {
 public:
  FakeRebuildSystem() {
    images_.explorer = L"C:\\Windows\\explorer.exe";
    images_.surrogate = L"C:\\Windows\\System32\\dllhost.exe";
    images_.surrogateWow64 = L"C:\\Windows\\SysWOW64\\dllhost.exe";
    (void)ProcessIdToSessionId(GetCurrentProcessId(), &ownSession_);
  }

  // ---- what the test lays out ----

  // AutoRestartShell (and the HRESULT reading it gives).
  bool autoRestart = true;
  HRESULT autoRestartHr = S_OK;

  // The cache files, and the HRESULT listing them gives.
  std::vector<std::filesystem::path> cacheFiles;
  HRESULT cacheFilesHr = S_OK;

  HRESULT findHoldersHr = S_OK;
  HRESULT imagesHr = S_OK;

  // The folder windows before the restart (the first listing) and after it (later listings).
  std::vector<FolderWindow> windowsBefore;
  std::vector<FolderWindow> windowsAfter;
  HRESULT listHr = S_OK;  // the first listing's HRESULT (S_FALSE: the shell did not answer)

  bool fileOperation = false;

  // Per-pid result for EndHolder (default S_OK).
  std::map<DWORD, HRESULT> endResult;

  // Files that a program left alone keeps in use even after the endable holders go (by native
  // path), and files that come free only after a number of delete attempts.
  std::set<std::wstring> inUseFiles;
  std::map<std::wstring, int> freeAfterRetries;
  std::map<std::wstring, ContentDelete> deleteOutcome;  // force kMissing / kNotAFile
  std::map<std::wstring, HRESULT> deleteFailure;

  // Making the start of File Explorer ready, and the trial start before anything changes: this
  // process's job, and what the trial finds. With |trialFailsFrom| n > 0, only the n-th trial and
  // later ones fail (with |trialHr|); with 0, every trial gives |trialHr|.
  HRESULT prepareHr = S_OK;
  JobFacts job;
  JobEscape trialEscape = JobEscape::kLeaves;
  HRESULT trialHr = S_OK;
  int trialFailsFrom = 0;
  mutable int prepareCalls = 0;
  mutable int trialCalls = 0;

  // Starting File Explorer (by the start made ready).
  HRESULT startHr = S_OK;
  bool startJobForbids = false;
  mutable int startCalls = 0;

  // When the shell comes back (milliseconds on the fake clock, after the shell ends, or after
  // this program starts File Explorer).
  bool shellBackByItself = true;
  ULONGLONG shellBackAfterMs = 0;
  bool shellBackAfterStart = true;
  ULONGLONG shellBackAfterStartMs = 0;
  bool explorerStartedSinceFlag = false;
  // The shell crashed before it was ended, and Windows replaced it at once: ending it finds it
  // gone (S_FALSE), and the new shell started before the engine ended the old one.
  bool shellReplacedMeanwhile = false;

  // What OpenFolderWindows reports as not opened.
  size_t openFailed = 0;

  // Asks |stop| to stop from inside a call: the first listing of the folder windows, the check for
  // a copy in progress, or ending the first process (past the point of no return).
  enum class StopIn { kNowhere, kListing, kFileOperationCheck, kEnding };
  StopIn stopIn = StopIn::kNowhere;
  std::stop_source* stop = nullptr;

  // Called on the engine's first question (AutoRestartShell, past the refusals the context
  // decides), for a test that looks at what the engine's caller had done by then.
  std::function<void()> onFirstCall;

  // ---- the shell and the holders ----

  void SetShell(DWORD pid) {
    shellPid_ = pid;
    shell_ = ShellProcess{pid, TicksToTime(kFakeBaseTicks)};
  }

  // Adds a process that holds |files|, with |image| at |pid| in this session (so it is this
  // user's). For the desktop's shell use its pid as SetShell's.
  void AddHolder(DWORD pid, const std::wstring& image, std::vector<std::filesystem::path> files) {
    Held held;
    held.holder.pid = pid;
    held.holder.start = TicksToTime(kFakeBaseTicks);
    held.holder.session = ownSession_;
    held.holder.image = image;
    held.files = std::move(files);
    held_.push_back(std::move(held));
  }

  // Adds a process of another session or account (left alone, named only as a count).
  void AddForeignHolder(DWORD pid, const std::wstring& image,
                        std::vector<std::filesystem::path> files) {
    Held held;
    held.holder.pid = pid;
    held.holder.start = TicksToTime(kFakeBaseTicks);
    held.holder.session = ownSession_ + 1;
    held.holder.image = image;
    held.files = std::move(files);
    held_.push_back(std::move(held));
  }

  // ---- what the engine did ----

  const std::vector<DWORD>& Ended() const { return ended_; }
  // How many calls the engine made (the clock aside).
  int Calls() const { return calls_; }
  const std::vector<std::wstring>& Deleted() const { return deleted_; }
  bool ShellEnded() const { return shellEnded_; }
  ULONGLONG NowMs() const noexcept override { return ms_; }

  // The standard images, so a test holder is classified as the engine expects.
  std::wstring kExplorerImage() const { return images_.explorer; }
  std::wstring kSurrogateImage() const { return images_.surrogate; }
  std::wstring kSurrogateWow64Image() const { return images_.surrogateWow64; }

  // ---- RebuildSystem ----

  HRESULT AutoRestartShell(bool* on) const noexcept override {
    if (calls_++ == 0 && onFirstCall) onFirstCall();
    *on = autoRestart;
    return autoRestartHr;
  }
  HRESULT CacheFiles(std::vector<std::filesystem::path>* files) const noexcept override {
    ++calls_;
    *files = cacheFiles;
    if (FAILED(cacheFilesHr)) return cacheFilesHr;
    return cacheFiles.empty() ? S_FALSE : S_OK;
  }
  HRESULT FindHolders(std::span<const std::filesystem::path> files,
                      std::vector<Holder>* holders) const noexcept override {
    ++calls_;
    holders->clear();
    if (FAILED(findHoldersHr)) return findHoldersHr;
    for (const Held& held : held_) {
      if (Intersects(held.files, files)) holders->push_back(held.holder);
    }
    return S_OK;
  }
  HRESULT Images(RebuildImages* images) const noexcept override {
    ++calls_;
    *images = images_;
    return imagesHr;
  }
  std::optional<ShellProcess> Shell() const noexcept override {
    ++calls_;
    if (replaced_) return ShellProcess{shellPid_ + 2000, replacedStart_};
    if (!shellEnded_) return shell_;
    if (shellBackByItself && ms_ >= shellEndMs_ + shellBackAfterMs) return NewShell();
    if (startCalls > 0 && shellBackAfterStart && ms_ >= startMs_ + shellBackAfterStartMs) {
      return NewShell();
    }
    return std::nullopt;
  }
  bool ExplorerStartedSince(const FILETIME&) const noexcept override {
    ++calls_;
    return explorerStartedSinceFlag;
  }
  FILETIME SystemTime() const noexcept override {
    ++calls_;
    return TicksToTime(kFakeBaseTicks + ms_ * 10000);
  }
  HRESULT ListFolderWindows(DWORD, std::vector<FolderWindow>* windows) const noexcept override {
    ++calls_;
    const bool first = listCalls_++ == 0;
    if (first) StopIf(StopIn::kListing);
    if (first && listHr == S_FALSE) {
      windows->clear();
      return S_FALSE;
    }
    *windows = first ? windowsBefore : windowsAfter;
    return S_OK;
  }
  bool FileOperationShown(std::span<const DWORD>) const noexcept override {
    ++calls_;
    StopIf(StopIn::kFileOperationCheck);
    return fileOperation;
  }
  HRESULT EndHolder(const Holder& holder, const RebuildImages&) const noexcept override {
    ++calls_;
    if (endCalls_++ == 0) StopIf(StopIn::kEnding);
    if (holder.pid == shellPid_ && shellReplacedMeanwhile) {
      replaced_ = true;
      replacedStart_ = SystemTime();
      return S_FALSE;
    }
    const auto found = endResult.find(holder.pid);
    const HRESULT hr = found == endResult.end() ? S_OK : found->second;
    if (hr == S_OK) {
      ended_.push_back(holder.pid);
      for (const Held& held : held_) {
        if (held.holder.pid == holder.pid) {
          for (const std::filesystem::path& file : held.files) freed_.insert(file.native());
        }
      }
    }
    if (holder.pid == shellPid_ && hr == S_OK) {
      shellEnded_ = true;
      shellEndMs_ = ms_;
    }
    return hr;
  }
  HRESULT DeleteCacheFile(const std::filesystem::path& file,
                          ContentDelete* result) const noexcept override {
    ++calls_;
    const std::wstring key = file.native();
    const auto fail = deleteFailure.find(key);
    if (fail != deleteFailure.end()) return fail->second;
    const auto forced = deleteOutcome.find(key);
    if (forced != deleteOutcome.end()) {
      *result = forced->second;
      if (*result == ContentDelete::kDeleted) deleted_.push_back(key);
      return S_OK;
    }
    const auto retries = retriesLeft_.find(key);
    if (retries != retriesLeft_.end() && retries->second > 0) {
      --retries->second;
      *result = ContentDelete::kInUse;
      return S_OK;
    }
    if (inUseFiles.count(key) && !freed_.count(key)) {
      *result = ContentDelete::kInUse;
      return S_OK;
    }
    *result = ContentDelete::kDeleted;
    deleted_.push_back(key);
    return S_OK;
  }
  HRESULT PrepareExplorerStart(
      std::unique_ptr<PreparedExplorerStart>* start) const noexcept override {
    ++calls_;
    ++prepareCalls;
    start->reset();
    if (FAILED(prepareHr)) return prepareHr;
    *start = std::make_unique<FakeStart>(this);
    return S_OK;
  }
  HRESULT OpenFolderWindows(std::span<const FolderWindow> windows, DWORD, size_t* opened,
                            size_t* failed) const noexcept override {
    ++calls_;
    const size_t fail = std::min(openFailed, windows.size());
    *opened = windows.size() - fail;
    *failed = fail;
    return S_OK;
  }
  void Wait(DWORD ms) const noexcept override {
    ++calls_;
    ms_ += ms;
  }

  // Call once the world is laid out: turns freeAfterRetries into the mutable countdown.
  void Prime() { retriesLeft_ = freeAfterRetries; }

 private:
  // The start made ready: it reads the fake's fields and counts in its calls.
  class FakeStart final : public PreparedExplorerStart {
   public:
    explicit FakeStart(const FakeRebuildSystem* system) : system_(system) {}
    const JobFacts& Job() const noexcept override { return system_->job; }
    HRESULT Trial(JobEscape* escape) noexcept override {
      ++system_->calls_;
      const int trial = ++system_->trialCalls;
      *escape = JobEscape::kForbidden;
      if (system_->trialFailsFrom == 0 || trial >= system_->trialFailsFrom) {
        if (FAILED(system_->trialHr)) return system_->trialHr;
      }
      *escape = system_->trialEscape;
      return S_OK;
    }
    HRESULT Start(bool* jobForbids) noexcept override {
      ++system_->calls_;
      ++system_->startCalls;
      system_->startMs_ = system_->ms_;
      *jobForbids = system_->startJobForbids;
      return system_->startHr;
    }

   private:
    const FakeRebuildSystem* system_;
  };

  struct Held {
    Holder holder;
    std::vector<std::filesystem::path> files;
  };

  static bool Intersects(const std::vector<std::filesystem::path>& a,
                         std::span<const std::filesystem::path> b) {
    for (const std::filesystem::path& x : a) {
      for (const std::filesystem::path& y : b) {
        if (x == y) return true;
      }
    }
    return false;
  }
  ShellProcess NewShell() const {
    return ShellProcess{shellPid_ + 1000, SystemTime()};  // another process, started later
  }
  void StopIf(StopIn where) const {
    if (stopIn == where && stop) stop->request_stop();
  }

  RebuildImages images_;
  DWORD ownSession_ = 0;
  DWORD shellPid_ = 0;
  std::optional<ShellProcess> shell_;
  std::vector<Held> held_;

  mutable ULONGLONG ms_ = 0;
  mutable int calls_ = 0;
  mutable int listCalls_ = 0;
  mutable int endCalls_ = 0;
  mutable std::vector<DWORD> ended_;
  mutable std::vector<std::wstring> deleted_;
  mutable std::set<std::wstring> freed_;
  mutable std::map<std::wstring, int> retriesLeft_;
  mutable bool shellEnded_ = false;
  mutable ULONGLONG shellEndMs_ = 0;
  mutable ULONGLONG startMs_ = 0;
  mutable bool replaced_ = false;
  mutable FILETIME replacedStart_ = {};
};

}  // namespace pdft::test
