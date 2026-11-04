// What thumbtest's refresh modes (refresh_modes.cpp) share with the one-sitting refresh
// experiment (experiment.cpp).

#pragma once

#include <windows.h>

#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "context.h"
#include "refresh.h"
#include "refresh_judge.h"

namespace thumbtest {

// The app's exit code for "cannot run here" (ExitCode::kWrongContext).
inline constexpr int kRefused = 8;

// Milliseconds from the performance counter: GetTickCount64 moves in steps of about 16 ms.
ULONGLONG PreciseMs() noexcept;

// A whole decimal number from |min| to |max|, digits only.
bool ParseNumber(const std::wstring& text, long min, long max, long* value);

// Why the walk of --refresh --folder would leave |path| out (ClassifyCandidate), from the
// attributes, size and volume Windows reports for it; kNone when it would not. Fails when they
// cannot be read.
HRESULT SkipOf(const std::filesystem::path& path, pdft::CandidateSkip* skip);
const char* SkipText(pdft::CandidateSkip skip);

// Why this process may not use the signed-in user's thumbnail cache and File Explorer, from its
// context with the registry canary (*context), said on the console: std::nullopt when it may,
// else the exit code (see refresh_modes.cpp).
std::optional<int> Refusal(pdft::ProcessContext* context);

// Writes |text| to |file|, replacing it.
bool WriteText(const std::wstring& file, const std::string& text);

// "<n>, min <a> ms, mean <b> ms, p95 <c> ms, max <d> ms" (p95: the nearest rank); "none" when
// there are none.
std::string TimesText(std::vector<DWORD> times);

// Asks |cache| about each of |files| at each of |sizes| with |flags| (a question only: the flags
// must hold WTS_INCACHEONLY) and returns the answers, one record per file and size, each file
// keyed "<label>/<file name>": the label is |label|, or the name of the file's folder. |print|
// prints a line per answer. *failed is set when a file could not be opened.
std::vector<pdft::test::CacheRecord> AskCache(pdft::ThumbnailCacheOps& cache,
                                              std::span<const std::filesystem::path> files,
                                              std::span<const UINT> sizes, DWORD flags,
                                              const std::optional<std::wstring>& label, bool print,
                                              bool* failed);

// The judge's line for one folder's verdict: PASS, FAIL, HELD, CHANGED or INVALID, then the
// counts; |zoomBefore| is the zoom the baseline should show.
std::string VerdictText(const pdft::test::FolderVerdict& verdict, long zoomBefore);

// One thumbcache_*.db file, by its metadata only.
struct CacheFile {
  ULONGLONG size = 0;
  ULONGLONG written = 0;  // last write, as a FILETIME (UTC)
};

// The thumbnail cache's files now, by name (never opened for their data). S_FALSE when there is
// no cache folder; *unreadable receives the files whose metadata could not be read, with the
// error.
HRESULT ListCacheFiles(std::map<std::string, CacheFile>* files,
                       std::map<std::string, DWORD>* unreadable);

// The FILETIME |written| (UTC) as "2026-11-04 18:48:18.000 UTC".
std::string TimeText(ULONGLONG written);

// A cachefiles --record file's text for |files|.
std::string CacheFilesRecord(const std::map<std::string, CacheFile>& files);

// thumbtest experiment <arguments> (experiment.cpp): returns the exit code. Runs on the main
// thread, after RunRefreshMode set up COM.
int RunExperiment(const std::vector<std::wstring>& args);

// The experiment's usage lines.
void PrintExperimentUsage();

}  // namespace thumbtest
