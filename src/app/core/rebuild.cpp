#include "rebuild.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <set>
#include <utility>

#include "explorer_settings.h"
#include "log.h"
#include "paths.h"
#include "strings.h"

namespace pdft {
namespace {

constexpr wchar_t kLogComponent[] = L"refresh";

// How often the wait for File Explorer looks again.
constexpr DWORD kShellPollMs = 100;
// The folder-window list is read again this often, and once it has not changed for this long the
// windows are opened again, at most kReopenSettleMaxMs from the first read.
constexpr DWORD kReopenPollMs = 500;
constexpr ULONGLONG kReopenStableMs = 1500;
constexpr ULONGLONG kReopenSettleMaxMs = 5000;
// How many rounds a file still in use is tried again, and the pause between them.
constexpr int kDeleteRetries = 3;
constexpr DWORD kDeleteRetryPauseMs = 50;

// The leaf (file name) of |image|.
std::wstring Leaf(std::wstring_view image) {
  return std::wstring(image.substr(image.find_last_of(L"\\/") + 1));
}

const wchar_t* ShellToken(ShellOutcome outcome) noexcept {
  switch (outcome) {
    case ShellOutcome::kNotEnded:
      return L"not-ended";
    case ShellOutcome::kBack:
      return L"back";
    case ShellOutcome::kStarted:
      return L"started";
    case ShellOutcome::kNotBack:
      return L"not-back";
  }
  return L"not-ended";
}

const wchar_t* StartToken(ShellStart start) noexcept {
  switch (start) {
    case ShellStart::kNotTried:
      return L"not-tried";
    case ShellStart::kStarted:
      return L"started";
    case ShellStart::kBelowPackage:
      return L"package";
    case ShellStart::kJobForbids:
      return L"job";
    case ShellStart::kFailed:
      return L"failed";
  }
  return L"not-tried";
}

// What the trial found: "leaves", "forbidden", "stays"; "not-checked" when it did not run.
const wchar_t* EscapeToken(const std::optional<JobEscape>& escape) noexcept {
  if (!escape) return L"not-checked";
  switch (*escape) {
    case JobEscape::kLeaves:
      return L"leaves";
    case JobEscape::kForbidden:
      return L"forbidden";
    case JobEscape::kStays:
      return L"stays";
  }
  return L"not-checked";
}

// This process's job (JobFactsToken); "not-read" when no start was made ready, since the job is
// read only then.
std::wstring JobToken(const std::optional<JobFacts>& job) {
  return job ? JobFactsToken(*job) : std::wstring(L"not-read");
}

// The refusal line of the log; for the two refusals about starting File Explorer, with what the
// trial found and this process's job.
std::wstring RefusalLogText(RebuildRefusal refusal, const std::optional<JobEscape>& escape,
                            HRESULT startError, const std::optional<JobFacts>& job) {
  std::wstring line = std::wstring(L"refresh full refused=") + RebuildRefusalToken(refusal);
  if (refusal == RebuildRefusal::kNoRestartInJob) {
    line += std::wstring(L" explorer-start=") + EscapeToken(escape) + L" job=" + JobToken(job);
  } else if (refusal == RebuildRefusal::kNoRestartStartFails) {
    line += L" start-hr=" + HResultText(startError) + L" job=" + JobToken(job);
  }
  return line;
}

std::wstring Count(size_t n) { return std::to_wstring(n); }

// "<n> <one>" or "<n> <many>".
std::wstring Counted(size_t n, const wchar_t* one, const wchar_t* many) {
  return Count(n) + L" " + (n == 1 ? one : many);
}

// "<a>, <b>, <c>, <d>, and <m> more": at most four names.
std::wstring Names(const std::vector<std::wstring>& names) {
  std::wstring text;
  const size_t shown = std::min<size_t>(names.size(), 4);
  for (size_t i = 0; i < shown; ++i) {
    if (i > 0) text += L", ";
    text += names[i];
  }
  if (names.size() > shown) text += L", and " + Count(names.size() - shown) + L" more";
  return text;
}

// Whether |image| is one the rebuild ends: File Explorer or the thumbnail process.
bool EndableImage(std::wstring_view image, const RebuildImages& images) noexcept {
  return SameImagePath(image, images.explorer) || SameImagePath(image, images.surrogate) ||
         (!images.surrogateWow64.empty() && SameImagePath(image, images.surrogateWow64));
}

// The folder windows the report and the log count as meant to be opened again: those opened,
// those that failed, and those past kMaxReopenedWindows.
size_t ReopenWanted(const RebuildResult& r) noexcept {
  return r.reopened + r.reopenFailed + r.overCap;
}

// Each line of the log (counts, HRESULTs and milliseconds only).
void LogResult(const RebuildResult& r, ExitCode code) noexcept try {
  const LogLevel level = code == ExitCode::kOk ? LogLevel::kInfo : LogLevel::kWarning;
  if (r.refusal != RebuildRefusal::kNone) {
    Log(level, kLogComponent, RefusalLogText(r.refusal, r.jobEscape, r.startError, r.job));
    return;
  }
  if (FAILED(r.failure)) {
    Log(level, kLogComponent, L"refresh full failed hr=" + HResultText(r.failure));
    return;
  }
  // Below a package this program never starts File Explorer, so no start is made ready there.
  const wchar_t* startReady = r.belowPackage ? L"not-needed" : r.startReady ? L"yes" : L"no";
  Log(level, kLogComponent,
      L"refresh full plan files=" + Count(r.cacheFiles) + L" holders=" + Count(r.holders) +
          L" to-end=" + Count(r.toEnd) + L" shell=" + (r.shellFound ? L"1" : L"0") + L" windows=" +
          Count(r.windows) + L" frames=" + Count(r.frames) + L" autorestart=" +
          (r.autoRestartShell ? L"1" : L"0") + L" packaged=" + (r.belowPackage ? L"yes" : L"no") +
          L" explorer-start=" + EscapeToken(r.jobEscape) + L" job=" + JobToken(r.job) +
          L" start-ready=" + startReady + L" listed=" +
          (r.listing == WindowListing::kListed ? L"yes" : L"no"));
  if (r.cacheFiles == 0) return;
  Log(level, kLogComponent,
      L"refresh full ended=" + Count(r.ended) + L" gone=" + Count(r.gone) + L" refused=" +
          Count(r.endRefused) + L" end-failed=" + Count(r.endFailed) + L" first-end-hr=" +
          HResultText(r.firstEndError));
  Log(level, kLogComponent,
      L"refresh full deleted=" + Count(r.deleted) + L" in-use=" + Count(r.inUse) + L" failed=" +
          Count(r.failed) + L" retried=" + Count(r.retried) + L" delete-ms=" + Count(r.deleteMs) +
          L" first-file-hr=" + HResultText(r.firstFileError));
  Log(level, kLogComponent,
      std::wstring(L"refresh full shell=") + ShellToken(r.shell) + L" shell-ms=" +
          Count(r.shellMs) + L" start=" + StartToken(r.start) + L" start-hr=" +
          HResultText(r.startError));
  if (r.reopenTried || r.listing == WindowListing::kUnanswered) {
    Log(level, kLogComponent,
        L"refresh full reopened=" + Count(r.reopened) + L" of=" + Count(ReopenWanted(r)) +
            L" still-open=" + Count(r.stillOpen) + L" failed=" + Count(r.reopenFailed) +
            L" over-cap=" + Count(r.overCap));
  }
} catch (...) {
  // Logging is best effort.
}

}  // namespace

bool IsThumbnailCacheFileName(std::wstring_view name) noexcept {
  constexpr std::wstring_view kPrefix = L"thumbcache_";
  constexpr std::wstring_view kSuffix = L".db";
  if (name.size() <= kPrefix.size() + kSuffix.size()) return false;
  if (!IStartsWith(name, kPrefix)) return false;
  const std::wstring_view tail = name.substr(name.size() - kSuffix.size());
  if (!IEquals(tail, kSuffix)) return false;
  const std::wstring_view middle =
      name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
  for (const wchar_t c : middle) {
    const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') ||
                    (c >= L'A' && c <= L'Z') || c == L'_';
    if (!ok) return false;
  }
  return true;
}

RebuildConditions RebuildConditionsOf(const ProcessContext& context, bool autoRestartShell,
                                      ExplorerStartCheck explorerStart) noexcept {
  RebuildConditions c;
  c.elevated = context.elevated;
  c.writesChecked = context.probe == ContextProbe::kWrite;
  c.virtualized = RefusesWrites(context.virtualization);
  c.otherShellUser = context.differentFromShellUser;
  c.belowPackage = context.packagedAncestor.has_value();
  c.autoRestartShell = autoRestartShell;
  c.explorerStart = explorerStart;
  return c;
}

RebuildRefusal DecideRebuildRefusal(const RebuildConditions& c) noexcept {
  if (c.elevated) return RebuildRefusal::kElevated;
  if (!c.writesChecked || c.virtualized) return RebuildRefusal::kVirtualized;
  if (c.otherShellUser) return RebuildRefusal::kOtherShellUser;
  if (!c.autoRestartShell) {
    // Only this program would bring File Explorer back, so it must be able to, outside any job.
    if (c.belowPackage) return RebuildRefusal::kNoRestartBelowPackage;
    if (c.explorerStart == ExplorerStartCheck::kFails) return RebuildRefusal::kNoRestartStartFails;
    if (c.explorerStart != ExplorerStartCheck::kOutsideJobs) return RebuildRefusal::kNoRestartInJob;
  }
  return RebuildRefusal::kNone;
}

const wchar_t* RebuildRefusalToken(RebuildRefusal refusal) noexcept {
  switch (refusal) {
    case RebuildRefusal::kElevated:
      return L"elevated";
    case RebuildRefusal::kVirtualized:
      return L"virtualized";
    case RebuildRefusal::kOtherShellUser:
      return L"other-shell-user";
    case RebuildRefusal::kNoRestartBelowPackage:
      return L"no-restart-below-package";
    case RebuildRefusal::kNoRestartInJob:
      return L"no-restart-in-job";
    case RebuildRefusal::kNoRestartStartFails:
      return L"no-restart-start-fails";
    case RebuildRefusal::kOwnProcessHoldsCache:
      return L"own-process-holds-cache";
    case RebuildRefusal::kFileOperation:
      return L"file-operation";
    case RebuildRefusal::kCancelled:
      return L"cancelled";
    case RebuildRefusal::kNone:
      return L"none";
  }
  return L"none";
}

HolderRole ClassifyHolder(const Holder& holder, DWORD ownPid, DWORD ownSession,
                          const std::optional<ShellProcess>& shell,
                          const RebuildImages& images) noexcept {
  if (holder.pid == ownPid) return HolderRole::kSelf;
  if (holder.session != ownSession) return HolderRole::kLeftAlone;
  if (holder.image.empty()) return HolderRole::kLeftAlone;
  if (SameImagePath(holder.image, images.explorer)) {
    if (shell && holder.pid == shell->pid && CompareFileTime(&holder.start, &shell->start) == 0) {
      return HolderRole::kShell;
    }
    return HolderRole::kFileExplorer;
  }
  if (SameImagePath(holder.image, images.surrogate) ||
      (!images.surrogateWow64.empty() && SameImagePath(holder.image, images.surrogateWow64))) {
    return HolderRole::kSurrogate;
  }
  return HolderRole::kLeftAlone;
}

std::vector<size_t> EndingOrder(std::span<const Holder> holders,
                                std::span<const HolderRole> roles) {
  std::vector<size_t> order;
  std::set<std::pair<DWORD, std::pair<DWORD, DWORD>>> seen;
  const auto add = [&](HolderRole want) {
    for (size_t i = 0; i < holders.size() && i < roles.size(); ++i) {
      if (roles[i] != want) continue;
      const std::pair<DWORD, std::pair<DWORD, DWORD>> key = {
          holders[i].pid, {holders[i].start.dwHighDateTime, holders[i].start.dwLowDateTime}};
      if (seen.insert(key).second) order.push_back(i);
    }
  };
  add(HolderRole::kSurrogate);
  add(HolderRole::kFileExplorer);
  add(HolderRole::kShell);
  return order;
}

ShellStep NextShellStep(const ShellWaitState& s) noexcept {
  if (s.back) return ShellStep::kDone;
  if (s.startTried) return s.waitedMs < s.limitMs ? ShellStep::kWait : ShellStep::kGiveUp;
  // Windows has not brought it back, and this program has not started it yet.
  if (s.autoRestart && s.waitedMs < s.limitMs) return ShellStep::kWait;
  // The File Explorer Windows started is still coming up: wait once more, and after that never
  // start a second one next to it.
  if (s.autoRestart && s.newExplorerRunning) {
    return s.extended ? ShellStep::kGiveUp : ShellStep::kExtend;
  }
  if (s.belowPackage) return ShellStep::kGiveUp;
  return ShellStep::kStart;
}

ReopenPlan PlanReopen(std::span<const FolderWindow> saved, std::span<const FolderWindow> now) {
  ReopenPlan plan;
  std::vector<bool> used(now.size(), false);
  std::vector<const FolderWindow*> candidates;
  for (const FolderWindow& window : saved) {
    if (window.parsingName.empty()) {
      ++plan.unnamed;
      continue;
    }
    bool matched = false;
    for (size_t j = 0; j < now.size(); ++j) {
      if (!used[j] && IEquals(window.parsingName, now[j].parsingName)) {
        used[j] = true;
        matched = true;
        break;
      }
    }
    if (matched) {
      ++plan.stillOpen;
    } else {
      candidates.push_back(&window);
    }
  }
  for (const FolderWindow* window : candidates) {
    if (plan.open.size() < kMaxReopenedWindows) {
      plan.open.push_back(*window);
    } else {
      ++plan.overCap;
    }
  }
  std::set<ULONG_PTR> frames;
  for (const FolderWindow& window : plan.open) {
    if (window.frame && !frames.insert(window.frame).second) plan.tabs = true;
  }
  return plan;
}

ExitCode RebuildExitCode(const RebuildResult& r) noexcept {
  switch (r.refusal) {
    case RebuildRefusal::kElevated:
    case RebuildRefusal::kVirtualized:
    case RebuildRefusal::kOtherShellUser:
    case RebuildRefusal::kNoRestartBelowPackage:
    case RebuildRefusal::kNoRestartInJob:
      return ExitCode::kWrongContext;
    case RebuildRefusal::kNoRestartStartFails:
    case RebuildRefusal::kOwnProcessHoldsCache:
      return ExitCode::kFailed;
    case RebuildRefusal::kFileOperation:
      return ExitCode::kProblems;
    case RebuildRefusal::kCancelled:
      return ExitCode::kCancelled;
    case RebuildRefusal::kNone:
      break;
  }
  if (FAILED(r.failure)) return ExitCode::kFailed;
  if (r.shell == ShellOutcome::kNotBack || r.failed > 0 || r.endFailed > 0 ||
      (r.cacheFiles > 0 && r.deleted == 0)) {
    return ExitCode::kProblems;
  }
  return ExitCode::kOk;
}

std::vector<std::wstring> RebuildReport(const RebuildResult& r) {
  std::vector<std::wstring> lines;
  if (r.cacheFiles == 0) {
    lines.push_back(L"The thumbnail cache is already empty; there is nothing to rebuild.");
    return lines;
  }
  if (r.deleted > 0) {
    lines.push_back(L"Thumbnail cache cleared: " + Count(r.deleted) + L" of " +
                    Counted(r.cacheFiles, L"cache file", L"cache files") + L" deleted.");
  } else if (r.cacheFiles == 1) {
    lines.push_back(L"The thumbnail cache was not cleared: its cache file could not be deleted.");
  } else {
    lines.push_back(L"The thumbnail cache was not cleared: none of its " + Count(r.cacheFiles) +
                    L" cache files could be deleted.");
  }
  if (r.endFailed > 0) {
    lines.push_back(Counted(r.endFailed, L"File Explorer or thumbnail process",
                            L"File Explorer or thumbnail processes") +
                    L" could not be ended (" + HResultText(r.firstEndError) + L").");
  }
  if (r.inUse > 0) {
    std::wstring by;
    if (!r.inUseByNotEnded.empty()) {
      by =
          L"File Explorer or thumbnail processes still running (" + Names(r.inUseByNotEnded) + L")";
    }
    // Programs left alone by name, or else as other accounts' (also when none could be named).
    if (!r.inUseBy.empty() || r.inUseByOthers || r.inUseByNotEnded.empty()) {
      if (!by.empty()) by += L" and by ";
      by += L"programs this program leaves alone (" +
            (r.inUseBy.empty() ? std::wstring(L"programs of other accounts or sessions")
                               : Names(r.inUseBy)) +
            L")";
    }
    lines.push_back(Counted(r.inUse, L"cache file is", L"cache files are") + L" in use by " + by +
                    L"; " + (r.inUse == 1 ? L"its" : L"their") +
                    L" thumbnails update after you next sign in.");
  }
  if (r.failed > 0) {
    lines.push_back(Counted(r.failed, L"cache file", L"cache files") + L" could not be deleted (" +
                    HResultText(r.firstFileError) + L").");
  }
  switch (r.shell) {
    case ShellOutcome::kBack:
      lines.push_back(L"File Explorer restarted.");
      break;
    case ShellOutcome::kStarted:
      lines.push_back(L"File Explorer did not start again by itself, so this program started it.");
      break;
    case ShellOutcome::kNotBack: {
      std::wstring line =
          L"File Explorer did not start again. Start it from Task Manager: Run new task, "
          L"explorer.exe.";
      if (r.start == ShellStart::kBelowPackage) {
        line +=
            L" This program does not start it from inside an app package (the terminal it "
            L"runs in).";
      } else if (r.start == ShellStart::kJobForbids) {
        line +=
            L" This program did not start it: from here it would have stayed inside a job of "
            L"the program this runs in.";
      }
      lines.push_back(std::move(line));
      break;
    }
    case ShellOutcome::kNotEnded:
      break;
  }
  if (r.listing == WindowListing::kUnanswered) {
    lines.push_back(
        L"The open folder windows could not be listed (File Explorer did not answer), so they "
        L"were not reopened.");
  } else if (r.reopenTried && ReopenWanted(r) > 0) {
    // Nothing to say when every window was still open (File Explorer opened them itself).
    std::wstring line = L"Reopened " + Count(r.reopened) + L" of " +
                        Counted(ReopenWanted(r), L"folder window.", L"folder windows.");
    if (r.tabs) line += L" Tabs come back as separate windows.";
    if (r.reopenFailed > 0) line += L" " + Count(r.reopenFailed) + L" could not be reopened.";
    if (r.overCap > 0) {
      line += L" Only the first " + Count(kMaxReopenedWindows) + L" were reopened.";
    }
    lines.push_back(std::move(line));
  }
  lines.push_back(L"Thumbnails are made again as folders are opened.");
  return lines;
}

std::wstring RebuildRefusalText(RebuildRefusal refusal, const ProcessContext& context,
                                HRESULT detail) {
  switch (refusal) {
    case RebuildRefusal::kElevated:
      return L"Cannot run here: running as administrator. Rebuilding the thumbnail cache restarts "
             L"File Explorer, which must not run as administrator; run --refresh --full from a "
             L"normal (not administrator) prompt.";
    case RebuildRefusal::kOtherShellUser:
      return L"Cannot run here: this program runs as " + context.userAccount +
             L", but File Explorer on this desktop belongs to another account, and the thumbnail "
             L"cache is each account's own. Run it as the signed-in user.";
    case RebuildRefusal::kNoRestartBelowPackage:
      return L"Cannot run here: Windows is set not to restart File Explorer by itself, and this "
             L"program does not start it from inside an app package (" +
             (context.packagedAncestor
                  ? L"started from the packaged app " + *context.packagedAncestor
                  : std::wstring(L"the terminal it runs in")) +
             L"). Run --refresh --full from a Command Prompt opened with Win+R (type cmd), which "
             L"no app package starts.";
    case RebuildRefusal::kNoRestartInJob:
      return L"Cannot run here: Windows is set not to restart File Explorer by itself, and a File "
             L"Explorer started from here would stay inside a job of the program this runs in (a "
             L"terminal or another tool), so ending that program could end File Explorer and the "
             L"taskbar too. Nothing changed; run --refresh --full from a Command Prompt opened "
             L"with Win+R (type cmd).";
    case RebuildRefusal::kNoRestartStartFails:
      return L"Cannot rebuild the thumbnail cache: Windows is set not to restart File Explorer by "
             L"itself, and this program cannot start File Explorer from here either (" +
             HResultText(detail) + L"), so the taskbar would not come back. Nothing changed.";
    case RebuildRefusal::kOwnProcessHoldsCache:
      return L"Cannot rebuild from this process: it has the thumbnail cache open itself. Nothing "
             L"changed.";
    case RebuildRefusal::kFileOperation:
      return L"File Explorer is copying, moving or deleting files. Nothing changed; run --refresh "
             L"--full again when it has finished.";
    case RebuildRefusal::kVirtualized:
    case RebuildRefusal::kCancelled:
    case RebuildRefusal::kNone:
      // kVirtualized uses the caller's CannotRunHereMessage(context); kCancelled and kNone have
      // no refusal line of their own.
      return {};
  }
  return {};
}

std::wstring RebuildFailureText(HRESULT failure) {
  if (failure == HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK)) {
    return L"Cannot rebuild the thumbnail cache: its folder is reached through a link or junction, "
           L"which this program does not follow. Nothing changed.";
  }
  return L"Cannot rebuild the thumbnail cache: " + HResultText(failure) + L". Nothing changed.";
}

std::wstring RebuildQuestion() {
  return L"Rebuilding the thumbnail cache restarts File Explorer: its windows close and open "
         L"again, the taskbar disappears for a few seconds, and a copy or move File Explorer is "
         L"doing would stop. Rebuild it now?";
}

std::wstring RebuildProgressText(RebuildPhase phase) {
  switch (phase) {
    case RebuildPhase::kEnding:
      return L"Restarting File Explorer and clearing the thumbnail cache...";
    case RebuildPhase::kDeletingOnly:
      return L"Clearing the thumbnail cache...";
    case RebuildPhase::kWaitingForExplorer:
    case RebuildPhase::kReopening:
      return {};
  }
  return {};
}

HRESULT ListThumbnailCacheFiles(const std::filesystem::path& folder,
                                const std::filesystem::path& stopAt,
                                std::vector<std::filesystem::path>* files) noexcept try {
  files->clear();
  RETURN_IF_FAILED(CheckNoReparsePoints(folder, stopAt));
  WIN32_FIND_DATAW data = {};
  wil::unique_hfind find(FindFirstFileExW((folder / L"*").c_str(), FindExInfoBasic, &data,
                                          FindExSearchNameMatch, nullptr, 0));
  if (!find) {
    const DWORD error = GetLastError();
    // No cache folder: nothing to delete, not a failure.
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return S_FALSE;
    return HRESULT_FROM_WIN32(error);
  }
  do {
    // The long name is checked, so an 8.3 alias never matches.
    if ((data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
        IsThumbnailCacheFileName(data.cFileName)) {
      files->push_back(folder / data.cFileName);
    }
  } while (FindNextFileW(find.get(), &data));
  const DWORD error = GetLastError();
  RETURN_HR_IF(HRESULT_FROM_WIN32(error), error != ERROR_NO_MORE_FILES);
  return files->empty() ? S_FALSE : S_OK;
}
CATCH_RETURN();

namespace {

// Whether |now| is a shell other than |old|, the one that was ended (another process id or start
// time), whenever it started: Windows may have replaced a shell that crashed before it was ended.
bool NewShell(const std::optional<ShellProcess>& now, const ShellProcess& old) noexcept {
  return now && (now->pid != old.pid || CompareFileTime(&now->start, &old.start) != 0);
}

// The pids of the File Explorer processes being ended, for the copy-in-progress check.
std::vector<DWORD> ExplorerPids(std::span<const Holder> holders, std::span<const HolderRole> roles,
                                std::span<const size_t> order) {
  std::vector<DWORD> pids;
  for (const size_t i : order) {
    if (roles[i] == HolderRole::kShell || roles[i] == HolderRole::kFileExplorer) {
      pids.push_back(holders[i].pid);
    }
  }
  return pids;
}

// What ending the holders did to File Explorer.
struct Ending {
  bool explorerEnded = false;  // a File Explorer process was ended: its windows closed
  bool shellGone = false;      // the shell went: ended, gone already, or ended but slow to go
  FILETIME tEnd = {};          // just before the shell was ended (UTC)
  ULONGLONG t0 = 0;            // when it had gone (NowMs)
};

// Ends the holders in |order|. A holder that a check refused, or that could not be ended, still
// runs: a shell like that is neither waited for nor started again.
void EndHolders(const RebuildSystem& system, const RebuildImages& images,
                std::span<const Holder> holders, std::span<const HolderRole> roles,
                std::span<const size_t> order, RebuildResult* result, Ending* ending) noexcept {
  for (const size_t i : order) {
    const bool shell = roles[i] == HolderRole::kShell;
    const FILETIME before = system.SystemTime();
    const HRESULT hr = system.EndHolder(holders[i], images);
    // Terminated, but still going after the wait: it goes all the same.
    const bool slow = hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (hr == S_OK) {
      ++result->ended;
    } else if (hr == S_FALSE) {
      ++result->gone;
    } else if (hr == E_ACCESSDENIED) {
      ++result->endRefused;
    } else {
      ++result->endFailed;
      if (SUCCEEDED(result->firstEndError)) result->firstEndError = hr;
    }
    if ((hr == S_OK || slow) && roles[i] != HolderRole::kSurrogate) ending->explorerEnded = true;
    if (shell && (hr == S_OK || hr == S_FALSE || slow)) {
      ending->shellGone = true;
      ending->tEnd = before;
      ending->t0 = system.NowMs();
    }
  }
}

// Deletes |files|; the indexes of those still in use go to |stillInUse|, which already has room
// for every file, so that nothing here allocates past the point of no return.
void DeleteFiles(const RebuildSystem& system, std::span<const std::filesystem::path> files,
                 RebuildResult* result, std::vector<size_t>* stillInUse) noexcept {
  const ULONGLONG start = system.NowMs();
  const auto failedWith = [result](HRESULT hr) {
    ++result->failed;
    if (SUCCEEDED(result->firstFileError)) result->firstFileError = hr;
  };
  for (size_t i = 0; i < files.size(); ++i) {
    ContentDelete outcome = ContentDelete::kMissing;
    const HRESULT hr = system.DeleteCacheFile(files[i], &outcome);
    if (FAILED(hr)) {
      failedWith(hr);
      continue;
    }
    switch (outcome) {
      case ContentDelete::kDeleted:
        ++result->deleted;
        break;
      case ContentDelete::kInUse:
        stillInUse->push_back(i);
        break;
      case ContentDelete::kMissing:
        break;
      case ContentDelete::kNotAFile:
      case ContentDelete::kMismatch:
        failedWith(E_UNEXPECTED);
        break;
    }
  }
  // A process was ended: the kernel may release a mapped view shortly after the process goes.
  if (result->ended > 0) {
    for (int round = 0; round < kDeleteRetries && !stillInUse->empty(); ++round) {
      system.Wait(kDeleteRetryPauseMs);
      ++result->retried;
      size_t kept = 0;
      for (size_t j = 0; j < stillInUse->size(); ++j) {
        const size_t i = (*stillInUse)[j];
        ContentDelete outcome = ContentDelete::kInUse;
        const HRESULT hr = system.DeleteCacheFile(files[i], &outcome);
        if (FAILED(hr)) {
          failedWith(hr);
        } else if (outcome == ContentDelete::kDeleted) {
          ++result->deleted;
        } else if (outcome == ContentDelete::kInUse) {
          (*stillInUse)[kept++] = i;
        } else if (outcome != ContentDelete::kMissing) {  // kMissing: gone meanwhile
          failedWith(E_UNEXPECTED);
        }
      }
      stillInUse->resize(kept);
    }
  }
  result->inUse = stillInUse->size();
  result->deleteMs = system.NowMs() - start;
}

// Who keeps the files still in use: this session's programs by image leaf name (apart, those of
// the images the rebuild ends), others only as a count. Never logged.
void NameHolders(const RebuildSystem& system, const RebuildImages& images,
                 std::span<const std::filesystem::path> files, std::span<const size_t> stillInUse,
                 RebuildResult* result) noexcept try {
  if (stillInUse.empty()) return;
  std::vector<std::filesystem::path> inUse;
  for (const size_t i : stillInUse) inUse.push_back(files[i]);
  DWORD ownSession = 0;
  (void)ProcessIdToSessionId(GetCurrentProcessId(), &ownSession);
  std::vector<Holder> holders;
  if (FAILED(system.FindHolders(inUse, &holders))) return;
  std::set<std::wstring> leftAlone;
  std::set<std::wstring> notEnded;
  for (const Holder& holder : holders) {
    if (holder.session == ownSession && !holder.image.empty()) {
      (EndableImage(holder.image, images) ? notEnded : leftAlone).insert(Leaf(holder.image));
    } else {
      result->inUseByOthers = true;
    }
  }
  result->inUseBy.assign(leftAlone.begin(), leftAlone.end());
  result->inUseByNotEnded.assign(notEnded.begin(), notEnded.end());
} catch (...) {
  // Out of memory: the report names nobody.
}

// Waits for File Explorer to come back after |oldShell| went, starting it where Windows will not
// (NextShellStep) with |prepared|, the start made ready before anything changed (null when that
// failed). The wait counts from when the shell had gone.
void WaitForShell(const RebuildOptions& options, const RebuildSystem& system,
                  PreparedExplorerStart* prepared, const ShellProcess& oldShell,
                  const Ending& ending, RebuildResult* result) noexcept {
  ShellWaitState state;
  state.autoRestart = result->autoRestartShell;
  state.belowPackage = result->belowPackage;
  state.limitMs = options.shellWaitMs;
  ULONGLONG phaseStart = ending.t0;
  for (;;) {
    state.back = NewShell(system.Shell(), oldShell);
    state.waitedMs = system.NowMs() - phaseStart;
    // At the end of the wait, and again at the end of the longer one: is one coming up?
    if (!state.startTried && state.autoRestart && state.waitedMs >= state.limitMs) {
      state.newExplorerRunning = system.ExplorerStartedSince(ending.tEnd);
    }
    switch (NextShellStep(state)) {
      case ShellStep::kDone:
        result->shell = state.startTried ? ShellOutcome::kStarted : ShellOutcome::kBack;
        result->shellMs = system.NowMs() - ending.t0;
        return;
      case ShellStep::kGiveUp:
        result->shell = ShellOutcome::kNotBack;
        // Not started: below a package, or because the one Windows started is still coming up.
        if (!state.startTried && state.belowPackage && !state.newExplorerRunning) {
          result->start = ShellStart::kBelowPackage;
        }
        result->shellMs = system.NowMs() - ending.t0;
        return;
      case ShellStep::kWait:
        system.Wait(kShellPollMs);
        break;
      case ShellStep::kExtend:
        state.extended = true;
        phaseStart = system.NowMs();
        break;
      case ShellStep::kStart: {
        // One last look before starting a second File Explorer.
        if (NewShell(system.Shell(), oldShell)) break;
        if (!prepared) {
          // It could not be made ready before anything changed (result->startError says why), so
          // nothing is tried now: the same end as a start that failed.
          result->shell = ShellOutcome::kNotBack;
          result->start = ShellStart::kFailed;
          result->shellMs = system.NowMs() - ending.t0;
          return;
        }
        bool jobForbids = false;
        const HRESULT hr = prepared->Start(&jobForbids);
        state.startTried = true;
        phaseStart = system.NowMs();
        if (FAILED(hr)) {
          result->shell = ShellOutcome::kNotBack;
          result->start = jobForbids ? ShellStart::kJobForbids : ShellStart::kFailed;
          result->startError = hr;
          result->shellMs = system.NowMs() - ending.t0;
          return;
        }
        result->start = ShellStart::kStarted;
        break;
      }
    }
  }
}

// Opens the folder windows again once the list has settled.
void Reopen(const RebuildOptions& options, const RebuildSystem& system,
            std::span<const FolderWindow> saved, RebuildResult* result) noexcept try {
  result->reopenTried = true;
  // Until they are opened, they count as not reopened (also when memory runs out on the way).
  result->reopenFailed = saved.size();
  const ULONGLONG start = system.NowMs();
  std::vector<FolderWindow> now;
  std::vector<std::wstring> lastNames;
  ULONGLONG stableSince = start;
  for (;;) {
    std::vector<FolderWindow> listed;
    if (system.ListFolderWindows(options.shellWaitMs, &listed) == S_OK) {
      std::vector<std::wstring> names;
      for (const FolderWindow& window : listed) names.push_back(window.parsingName);
      std::sort(names.begin(), names.end());
      if (names != lastNames) {
        lastNames = std::move(names);
        stableSince = system.NowMs();
      }
      now = std::move(listed);
    }
    const ULONGLONG elapsed = system.NowMs() - start;
    if (system.NowMs() - stableSince >= kReopenStableMs || elapsed >= kReopenSettleMaxMs) break;
    system.Wait(kReopenPollMs);
  }
  const ReopenPlan plan = PlanReopen(saved, now);
  result->stillOpen = plan.stillOpen;
  result->overCap = plan.overCap;
  result->tabs = plan.tabs;
  size_t opened = 0;
  size_t failed = plan.open.size();
  if (!plan.open.empty()) {
    (void)system.OpenFolderWindows(plan.open, options.shellWaitMs, &opened, &failed);
  }
  result->reopened = opened;
  // Windows without a parsing name could not be opened either.
  result->reopenFailed = failed + plan.unnamed;
} catch (...) {
  // Out of memory: the windows not opened by then count as not reopened.
}

// Does a visible top-level window of class "OperationStatusWindow" (File Explorer's copy/move/
// delete progress) belong to one of the pids.
struct FileOperationSearch {
  std::span<const DWORD> pids;
  bool found = false;
};

BOOL CALLBACK FileOperationWindow(HWND window, LPARAM parameter) {
  auto* search = reinterpret_cast<FileOperationSearch*>(parameter);
  if (!IsWindowVisible(window)) return TRUE;
  DWORD pid = 0;
  if (!GetWindowThreadProcessId(window, &pid) || !pid) return TRUE;
  if (std::find(search->pids.begin(), search->pids.end(), pid) == search->pids.end()) return TRUE;
  wchar_t className[64] = {};
  if (GetClassNameW(window, className, ARRAYSIZE(className)) == 0) return TRUE;
  if (IEquals(className, L"OperationStatusWindow")) {
    search->found = true;
    return FALSE;
  }
  return TRUE;
}

// A ProgramStart of File Explorer.
class RealPreparedExplorerStart final : public PreparedExplorerStart {
 public:
  explicit RealPreparedExplorerStart(std::unique_ptr<ProgramStart> start) noexcept
      : start_(std::move(start)) {}
  const JobFacts& Job() const noexcept override { return start_->Job(); }
  HRESULT Trial(JobEscape* escape) noexcept override { return start_->Trial(escape); }
  HRESULT Start(bool* jobForbids) noexcept override { return start_->Start(jobForbids); }

 private:
  std::unique_ptr<ProgramStart> start_;
};

// The real answers from Windows.
class RealRebuildSystemImpl final : public RebuildSystem {
 public:
  HRESULT AutoRestartShell(bool* on) const noexcept override {
    return ReadAutoRestartShell(RegistryRoots{}, on);
  }
  HRESULT CacheFiles(std::vector<std::filesystem::path>* files) const noexcept override try {
    files->clear();
    wil::unique_cotaskmem_string localAppData;
    RETURN_IF_FAILED(
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr, &localAppData));
    const std::filesystem::path base(localAppData.get());
    const std::filesystem::path folder = base / L"Microsoft" / L"Windows" / L"Explorer";
    return ListThumbnailCacheFiles(folder, base, files);
  }
  CATCH_RETURN();
  HRESULT FindHolders(std::span<const std::filesystem::path> files,
                      std::vector<Holder>* holders) const noexcept override {
    return pdft::FindHolders(files, holders);
  }
  HRESULT Images(RebuildImages* images) const noexcept override { return GetRebuildImages(images); }
  std::optional<ShellProcess> Shell() const noexcept override {
    HWND window = GetShellWindow();
    if (!window) return std::nullopt;
    DWORD pid = 0;
    if (!GetWindowThreadProcessId(window, &pid) || !pid) return std::nullopt;
    wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return std::nullopt;
    FILETIME start = {};
    FILETIME unused[3] = {};
    if (!GetProcessTimes(process.get(), &start, &unused[0], &unused[1], &unused[2])) {
      return std::nullopt;
    }
    return ShellProcess{pid, start};
  }
  bool ExplorerStartedSince(const FILETIME& since) const noexcept override {
    RebuildImages images;
    if (FAILED(GetRebuildImages(&images))) return false;
    return OwnProcessStartedSince(images.explorer, since);
  }
  FILETIME SystemTime() const noexcept override {
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    return now;
  }
  HRESULT ListFolderWindows(DWORD timeoutMs,
                            std::vector<FolderWindow>* windows) const noexcept override {
    return pdft::ListFolderWindows(timeoutMs, windows);
  }
  bool FileOperationShown(std::span<const DWORD> explorerPids) const noexcept override {
    if (explorerPids.empty()) return false;
    FileOperationSearch search{explorerPids, false};
    EnumWindows(FileOperationWindow, reinterpret_cast<LPARAM>(&search));
    return search.found;
  }
  HRESULT EndHolder(const Holder& holder, const RebuildImages& images) const noexcept override try {
    std::vector<std::wstring_view> allowed;
    allowed.push_back(images.explorer);
    allowed.push_back(images.surrogate);
    if (!images.surrogateWow64.empty()) allowed.push_back(images.surrogateWow64);
    return TerminateOwnSessionHolder(holder, allowed);
  }
  CATCH_RETURN();
  HRESULT DeleteCacheFile(const std::filesystem::path& file,
                          ContentDelete* result) const noexcept override {
    return DeleteRegularFile(file, result);
  }
  HRESULT PrepareExplorerStart(
      std::unique_ptr<PreparedExplorerStart>* start) const noexcept override try {
    start->reset();
    RebuildImages images;
    RETURN_IF_FAILED(GetRebuildImages(&images));
    std::unique_ptr<ProgramStart> program;
    RETURN_IF_FAILED(ProgramStart::Prepare(std::move(images.explorer), {}, &program));
    *start = std::make_unique<RealPreparedExplorerStart>(std::move(program));
    return S_OK;
  }
  CATCH_RETURN();
  HRESULT OpenFolderWindows(std::span<const FolderWindow> windows, DWORD timeoutMs, size_t* opened,
                            size_t* failed) const noexcept override {
    return pdft::OpenFolderWindows(windows, timeoutMs, opened, failed);
  }
  void Wait(DWORD ms) const noexcept override { Sleep(ms); }
  ULONGLONG NowMs() const noexcept override { return GetTickCount64(); }
};

}  // namespace

const RebuildSystem& RealRebuildSystem() noexcept {
  static const RealRebuildSystemImpl system;
  return system;
}

HRESULT PrecheckRebuild(const ProcessContext& context, const RebuildSystem& system,
                        RebuildPrecheck* check,
                        std::unique_ptr<PreparedExplorerStart>* keep) noexcept try {
  *check = {};
  if (keep) keep->reset();

  // The refusals that the context alone decides.
  check->conditions = RebuildConditionsOf(context, true);
  check->refusal = DecideRebuildRefusal(check->conditions);
  if (check->refusal != RebuildRefusal::kNone) return S_OK;

  // Whether Windows restarts File Explorer by itself.
  bool autoRestart = true;
  if (const HRESULT hr = system.AutoRestartShell(&autoRestart); FAILED(hr)) {
    check->failure = hr;
    return S_OK;
  }
  check->conditions.autoRestartShell = autoRestart;
  // This program never starts File Explorer below a package, so nothing is made ready there.
  if (check->conditions.belowPackage) {
    check->refusal = DecideRebuildRefusal(check->conditions);
    return S_OK;
  }

  // The start, made ready where it may be needed: only this program brings File Explorer back
  // where Windows does not, and it is the fallback where Windows does.
  std::unique_ptr<PreparedExplorerStart> start;
  if (!autoRestart || keep) {
    HRESULT hr = system.PrepareExplorerStart(&start);
    if (SUCCEEDED(hr) && !start) hr = E_UNEXPECTED;
    if (FAILED(hr)) {
      start.reset();
      check->startError = hr;
      if (!autoRestart) check->conditions.explorerStart = ExplorerStartCheck::kFails;
    } else {
      check->job = start->Job();
    }
  }
  // Tried where Windows does not restart File Explorer: would it run outside every job?
  if (!autoRestart && start) {
    JobEscape escape = JobEscape::kForbidden;
    if (const HRESULT hr = start->Trial(&escape); FAILED(hr)) {
      check->startError = hr;
      check->conditions.explorerStart = ExplorerStartCheck::kFails;
    } else {
      check->escape = escape;
      check->conditions.explorerStart = escape == JobEscape::kLeaves
                                            ? ExplorerStartCheck::kOutsideJobs
                                            : ExplorerStartCheck::kInsideJob;
    }
  }
  check->refusal = DecideRebuildRefusal(check->conditions);
  if (keep && check->refusal == RebuildRefusal::kNone && start) *keep = std::move(start);
  return S_OK;
}
CATCH_RETURN();

RebuildResult RebuildNotStarted(const RebuildPrecheck& check) {
  RebuildResult result;
  result.refusal = check.refusal;
  result.failure = check.failure;
  result.autoRestartShell = check.conditions.autoRestartShell;
  result.belowPackage = check.conditions.belowPackage;
  result.explorerStart = check.conditions.explorerStart;
  result.jobEscape = check.escape;
  result.job = check.job;
  result.startError = check.startError;
  return result;
}

std::wstring RebuildPrecheckLogText(const RebuildPrecheck& check) {
  if (FAILED(check.failure)) return L"refresh full failed hr=" + HResultText(check.failure);
  return RefusalLogText(check.refusal, check.escape, check.startError, check.job);
}

HRESULT RebuildThumbnailCache(const RebuildOptions& options, const RebuildSystem& system,
                              const RebuildProgressFn& progress, std::stop_token stop,
                              RebuildResult* result) noexcept try {
  *result = {};
  // A progress line is not worth stopping for once processes are being ended.
  const auto report = [&progress](RebuildPhase phase) noexcept {
    if (!progress) return;
    try {
      progress(phase);
    } catch (...) {
    }
  };
  const auto finish = [result]() noexcept {
    LogResult(*result, RebuildExitCode(*result));
    return S_OK;
  };
  const auto failWith = [result, &finish](HRESULT hr) noexcept {
    result->failure = hr;
    return finish();
  };

  // The refusals before anything else (PrecheckRebuild), checked again here: the caller may have
  // waited at its question meanwhile. The start made ready here is the one File Explorer is
  // started with after the point of no return, if it must be.
  RebuildPrecheck check;
  std::unique_ptr<PreparedExplorerStart> prepared;
  RETURN_IF_FAILED(PrecheckRebuild(options.context, system, &check, &prepared));
  *result = RebuildNotStarted(check);
  result->startReady = prepared != nullptr;
  if (FAILED(result->failure) || result->refusal != RebuildRefusal::kNone) return finish();

  // The cache files.
  std::vector<std::filesystem::path> files;
  const HRESULT cacheHr = system.CacheFiles(&files);
  if (FAILED(cacheHr)) return failWith(cacheHr);
  result->cacheFiles = files.size();
  if (files.empty()) return finish();

  // Who holds them, and this process among them.
  std::vector<Holder> holders;
  const HRESULT holdersHr = system.FindHolders(files, &holders);
  if (FAILED(holdersHr)) return failWith(holdersHr);
  result->holders = holders.size();

  const DWORD ownPid = GetCurrentProcessId();
  DWORD ownSession = 0;
  (void)ProcessIdToSessionId(ownPid, &ownSession);
  const std::optional<ShellProcess> shell = system.Shell();
  result->shellFound = shell.has_value();
  RebuildImages images;
  const HRESULT imagesHr = system.Images(&images);
  if (FAILED(imagesHr)) return failWith(imagesHr);

  std::vector<HolderRole> roles(holders.size(), HolderRole::kLeftAlone);
  for (size_t i = 0; i < holders.size(); ++i) {
    roles[i] = ClassifyHolder(holders[i], ownPid, ownSession, shell, images);
    if (roles[i] == HolderRole::kSelf) {
      result->refusal = RebuildRefusal::kOwnProcessHoldsCache;
      return finish();
    }
  }

  // Which to end, and in what order.
  const std::vector<size_t> order = EndingOrder(holders, roles);
  result->toEnd = order.size();
  const std::vector<DWORD> explorerPids = ExplorerPids(holders, roles, order);
  const bool endsExplorer = !explorerPids.empty();

  // The folder windows, only when a File Explorer process is ended and there is a shell.
  std::vector<FolderWindow> windows;
  if (endsExplorer && shell) {
    const HRESULT listHr = system.ListFolderWindows(options.shellWaitMs, &windows);
    if (listHr == S_OK) {
      result->listing = WindowListing::kListed;
      result->windows = windows.size();
      std::set<ULONG_PTR> frames;
      for (const FolderWindow& window : windows) frames.insert(window.frame);
      result->frames = frames.size();
    } else {
      result->listing = WindowListing::kUnanswered;
    }
  }

  // A copy or move in progress in a File Explorer that would be ended.
  if (endsExplorer && system.FileOperationShown(explorerPids)) {
    result->refusal = RebuildRefusal::kFileOperation;
    return finish();
  }

  // The last chance to stop.
  if (stop.stop_requested()) {
    result->refusal = RebuildRefusal::kCancelled;
    return finish();
  }

  // Room for every file still in use, so that deleting allocates nothing.
  std::vector<size_t> stillInUse;
  stillInUse.reserve(files.size());

  // The point of no return: from here the run finishes whatever the stop token says, and nothing
  // below throws.
  result->committed = true;
  Ending ending;
  if (!order.empty()) {
    report(RebuildPhase::kEnding);
    EndHolders(system, images, holders, roles, order, result, &ending);
  } else {
    report(RebuildPhase::kDeletingOnly);
  }

  DeleteFiles(system, files, result, &stillInUse);
  NameHolders(system, images, files, stillInUse, result);

  if (ending.shellGone) {
    report(RebuildPhase::kWaitingForExplorer);
    WaitForShell(options, system, prepared.get(), *shell, ending, result);
  }

  // Open the windows again when File Explorer closed them, and a shell is there to open them.
  const bool shellPresent = !ending.shellGone || result->shell == ShellOutcome::kBack ||
                            result->shell == ShellOutcome::kStarted;
  if ((ending.explorerEnded || ending.shellGone) && options.reopenWindows &&
      result->listing == WindowListing::kListed && !windows.empty() && shellPresent) {
    report(RebuildPhase::kReopening);
    Reopen(options, system, windows, result);
  }
  return finish();
}
CATCH_RETURN();

}  // namespace pdft
