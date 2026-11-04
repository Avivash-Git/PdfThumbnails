// thumbtest's modes for the thumbnail refresh experiment, on the settings app's core (refresh.h,
// rebuild.h, shell_windows.h). They work on the signed-in user's own thumbnail cache and File
// Explorer, so they are for synthetic PDFs only (thumbtest makepdfs), from a normal (not
// administrator) prompt with no app package above it and a registry that is not virtualized (a
// Command Prompt opened with Win+R): every mode but judge checks that first, with the app's
// registry canary (a volatile key it deletes again), and refuses otherwise (exit 8). incache and
// refresh also refuse files the refresh leaves alone (online-only, on the network, ...).
//
//   thumbtest incache [--plain] [--sizes <list>] [--label <label>] [--record <tsv>]
//                     <file.pdf|dir>...
//       Asks the cache whether it holds each file's thumbnail at each size (WTS_INCACHEONLY, and
//       WTS_IDEALCACHESIZEONLY unless --plain), one line each: "CACHED 181x256 flags=0x2
//       id=<32 hex>" or "MISS hr=0x...". Never makes a thumbnail. --record writes the answers
//       for judge (refresh_judge.h), each file as "<label>/<file name>"; the label is the name
//       of the file's folder unless --label gives one.
//   thumbtest refresh [--probe ideal|plain]
//                     [--extract product|force|slowreclaim|instance|force-then-extract]
//                     [--notify none|item|dir|views|assoc[+...]] [--sizes <list>]
//                     [--cancel-after <ms>] [--timing <tsv>] <file.pdf|dir>...
//       Makes the cached thumbnails of the files again as --refresh --folder does (RefreshFiles),
//       with the experiment's probe, extraction and notification (by default the product's), and
//       prints the counts and how long the extractions took (min, mean, 95th percentile, max).
//       --cancel-after asks it to stop after that many milliseconds and prints how long it took
//       to; --timing writes how long each extraction took.
//   thumbtest judge --before <tsv> --after <tsv> --zoom-before <z> --zoom-after <z>
//                   [--control <label>]...
//       The verdict per folder from two incache records (Judge); exit 0 when every folder did what
//       it should (a control folder stayed old), every --control folder is in the records, and
//       at least one folder is not a control.
//   thumbtest cachefiles [--record <tsv>] [--diff <tsv>]
//       Name, size and last-write time of each thumbcache_*.db (never what is in them); --diff
//       compares them with an earlier --record.
//   thumbtest shellinfo [--windows] [--holders [--after-explorer-test]]
//       What the cache rebuild would find: the shell, its user and integrity, File Explorer
//       processes in this session, AutoRestartShell, SeparateProcess, the number of cache files;
//       --windows counts the folder windows and their frames; --holders the processes that have
//       a cache file open, by image name (Restart Manager, read-only), and whether this one does.
//       --after-explorer-test first runs the app's Explorer test in this process (a probe PDF in
//       %TEMP%, extracted without caching), as the settings window does, so that the last answer
//       shows whether that test leaves a process holding the cache.
//   thumbtest opendialog <folder>
//       Shows the File Open dialog at <folder> in this process, another process than File
//       Explorer with thumbnails of its own, until it is closed.
//   thumbtest experiment ...
//       The one-sitting refresh experiment, which uses the modes' parts (experiment.cpp).
//
// <list> is sizes in pixels separated by commas ("16,32,256"); without --sizes the product's
// (kRefreshSizes). Output names files and folders by name only, apart from what was typed.

#include <windows.h>

#include "refresh_modes.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <tlhelp32.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "com_security.h"
#include "context.h"
#include "crop_math.h"
#include "explorer_settings.h"
#include "live_test.h"
#include "medium_il.h"
#include "paths.h"
#include "processes.h"
#include "rebuild.h"
#include "refresh.h"
#include "refresh_judge.h"
#include "registry_roots.h"
#include "shell_windows.h"
#include "startup.h"
#include "thumbtest.h"
#include "zoom.h"

namespace thumbtest {
namespace {

using pdft::test::CacheRecord;

// How long File Explorer gets to list its windows.
constexpr DWORD kListTimeoutMs = 5000;

// Folder Options > View > "Launch folder windows in a separate process": REG_DWORD 1 under
// kExplorerAdvancedKey.
constexpr wchar_t kSeparateProcessValue[] = L"SeparateProcess";

// Record files are read up to this size: the cache folder has a few dozen files.
constexpr ULONGLONG kMaxTextBytes = 1 << 20;

// --cancel-after takes up to a day.
constexpr long kMaxCancelAfterMs = 24 * 3600 * 1000;

// A --record file: a header, then "<name>\t<size>\t<last write>" (the cache files' names are
// ASCII: IsThumbnailCacheFileName).
constexpr std::string_view kCacheFilesHeader = "name\tsize\twritten";

std::string IdHex(const std::array<BYTE, 16>& id) {
  std::string text;
  for (const BYTE byte : id) {
    char hex[3];
    std::snprintf(hex, sizeof(hex), "%02x", byte);
    text += hex;
  }
  return text;
}

}  // namespace

ULONGLONG PreciseMs() noexcept {
  static const LONGLONG frequency = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart;
  }();
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return static_cast<ULONGLONG>(now.QuadPart / frequency * 1000 +
                                now.QuadPart % frequency * 1000 / frequency);
}

bool ParseNumber(const std::wstring& text, long min, long max, long* value) {
  if (text.empty() || !std::iswdigit(text[0])) return false;
  wchar_t* end = nullptr;
  errno = 0;
  const long number = std::wcstol(text.c_str(), &end, 10);
  if (*end != L'\0' || errno == ERANGE || number < min || number > max) return false;
  *value = number;
  return true;
}

HRESULT SkipOf(const std::filesystem::path& path, pdft::CandidateSkip* skip) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
    const DWORD error = GetLastError();
    return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
  }
  wchar_t volume[MAX_PATH + 1] = {};
  const UINT driveType = GetVolumePathNameW(path.c_str(), volume, ARRAYSIZE(volume))
                             ? GetDriveTypeW(volume)
                             : DRIVE_UNKNOWN;
  *skip = pdft::ClassifyCandidate(
      path.native(), data.dwFileAttributes,
      (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow, driveType);
  return S_OK;
}

const char* SkipText(pdft::CandidateSkip skip) {
  switch (skip) {
    case pdft::CandidateSkip::kNone:
      return "not left out";
    case pdft::CandidateSkip::kPlaceholder:
      return "online-only (not on this PC)";
    case pdft::CandidateSkip::kOffline:
      return "offline";
    case pdft::CandidateSkip::kNetwork:
      return "on the network";
    case pdft::CandidateSkip::kRecycleBin:
      return "in the recycle bin";
    case pdft::CandidateSkip::kTooLarge:
      return "larger than the handler reads (256 MiB)";
    case pdft::CandidateSkip::kLongPath:
      return "named by a path of MAX_PATH characters or more";
    case pdft::CandidateSkip::kNotPdf:
      return "not a .pdf file";
  }
  return "?";
}

// Not elevated (the cache and File Explorer are the signed-in user's, and the handler does not
// render elevated); not below an app package, even one that leaves the registry alone (a Windows
// Terminal tab): the cache runs in this process, and a cache file it makes could land in the
// package's private copy of the user's folders; and not where its registry writes may be
// swallowed. The walk up the parents can fail, so the canary still decides on its own as well.
std::optional<int> Refusal(pdft::ProcessContext* context) {
  const HRESULT hr = pdft::GetProcessContext(pdft::ContextProbe::kWrite, context);
  if (FAILED(hr)) {
    std::printf("cannot read this process's user and token: 0x%08lX\n",
                static_cast<unsigned long>(hr));
    return 2;
  }
  if (context->elevated) {
    std::printf(
        "refused: running as administrator. The thumbnail cache and File Explorer are the\n"
        "signed-in user's: run thumbtest from a normal (not administrator) prompt.\n");
    return kRefused;
  }
  if (context->packagedAncestor) {
    std::printf(
        "refused: started from the packaged app %s. The thumbnail cache runs in this process,\n"
        "and what it sees of the cache, and the files it makes there, may be that app's private\n"
        "copy: run thumbtest from a Command Prompt opened with Win+R.\n",
        Utf8(*context->packagedAncestor).c_str());
    return kRefused;
  }
  if (context->virtualization == pdft::Virtualization::kUnknown) {
    // No package to leave: the check itself failed (WMI not answering, say).
    std::printf(
        "refused: the registry check (a test value written here and read back through WMI)\n"
        "failed");
    if (FAILED(context->probeError)) {
      std::printf(" with 0x%08lX", static_cast<unsigned long>(context->probeError));
    }
    std::printf(
        ", so it cannot be confirmed that this process sees the signed-in user's own\n"
        "registry and thumbnail cache.\n");
    return kRefused;
  }
  if (pdft::RefusesWrites(context->virtualization)) {
    std::printf(
        "refused: this process's registry writes are swallowed, so what it sees of the thumbnail\n"
        "cache may be a private copy: run thumbtest from a Command Prompt opened with Win+R.\n");
    return kRefused;
  }
  return std::nullopt;
}

bool WriteText(const std::wstring& file, const std::string& text) {
  if (text.size() > MAXDWORD) return false;
  wil::unique_hfile handle(CreateFileW(
      file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (!handle) return false;
  DWORD written = 0;
  return WriteFile(handle.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
         written == text.size();
}

std::string TimesText(std::vector<DWORD> times) {
  if (times.empty()) return "none";
  std::sort(times.begin(), times.end());
  ULONGLONG total = 0;
  for (const DWORD ms : times) total += ms;
  // The nearest rank: the smallest time at least 95 % of them do not exceed.
  const size_t rank = (times.size() * 95 + 99) / 100;
  char text[160];
  std::snprintf(text, sizeof(text), "%zu, min %lu ms, mean %.1f ms, p95 %lu ms, max %lu ms",
                times.size(), times.front(),
                static_cast<double>(total) / static_cast<double>(times.size()), times[rank - 1],
                times.back());
  return text;
}

std::vector<CacheRecord> AskCache(pdft::ThumbnailCacheOps& cache,
                                  std::span<const std::filesystem::path> files,
                                  std::span<const UINT> sizes, DWORD flags,
                                  const std::optional<std::wstring>& label, bool print,
                                  bool* failed) {
  std::vector<CacheRecord> records;
  for (const std::filesystem::path& file : files) {
    const std::wstring folder = label ? *label : file.parent_path().filename().native();
    const std::wstring key = folder + L"/" + file.filename().native();
    const HRESULT selected = cache.Select(file);
    if (FAILED(selected)) {
      std::printf("ERROR        hr=0x%08lX (cannot open the file)  %s\n",
                  static_cast<unsigned long>(selected), Utf8(key).c_str());
      *failed = true;
    }
    for (const UINT size : sizes) {
      CacheRecord entry;
      entry.label = folder;
      entry.key = key;
      entry.size = size;
      pdft::CacheAnswer answer;
      entry.hr = FAILED(selected) ? selected : cache.Get(size, flags, &answer);
      entry.cacheFlags = answer.cacheFlags;
      entry.id = answer.id;
      if (answer.bitmap) {
        entry.w = static_cast<int>(answer.dims.cx);
        entry.h = static_cast<int>(answer.dims.cy);
      }
      entry.cached = entry.hr == S_OK && answer.bitmap && (answer.cacheFlags & WTS_CACHED);
      if (print && SUCCEEDED(selected)) {
        if (entry.cached) {
          std::printf("CACHED %5u  %dx%d flags=0x%lX id=%s  %s\n", size, entry.w, entry.h,
                      static_cast<unsigned long>(entry.cacheFlags), IdHex(entry.id).c_str(),
                      Utf8(key).c_str());
        } else if (answer.bitmap) {
          std::printf("MISS   %5u  hr=0x%08lX flags=0x%lX, a bitmap %dx%d  %s\n", size,
                      static_cast<unsigned long>(entry.hr),
                      static_cast<unsigned long>(entry.cacheFlags), entry.w, entry.h,
                      Utf8(key).c_str());
        } else {
          std::printf("MISS   %5u  hr=0x%08lX  %s\n", size, static_cast<unsigned long>(entry.hr),
                      Utf8(key).c_str());
        }
      }
      records.push_back(std::move(entry));
    }
    if (print) std::fflush(stdout);
  }
  return records;
}

HRESULT ListCacheFiles(std::map<std::string, CacheFile>* files,
                       std::map<std::string, DWORD>* unreadable) {
  files->clear();
  unreadable->clear();
  std::vector<std::filesystem::path> paths;
  const HRESULT hr = pdft::RealRebuildSystem().CacheFiles(&paths);
  if (FAILED(hr)) return hr;
  for (const std::filesystem::path& path : paths) {
    const std::string name = Utf8(path.filename().native());
    // Its attributes only: the file is never opened for its data, so whoever has it open is not
    // disturbed.
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
      (*unreadable)[name] = GetLastError();
      continue;
    }
    (*files)[name] = {(static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                      (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                          data.ftLastWriteTime.dwLowDateTime};
  }
  return hr;
}

std::string VerdictText(const pdft::test::FolderVerdict& v, long zoomBefore) {
  const char* word = !v.baselineValid ? "INVALID"
                     : v.passA        ? (v.control ? "HELD" : "PASS")
                                      : (v.control ? "CHANGED" : "FAIL");
  char text[320];
  std::snprintf(
      text, sizeof(text),
      "%-8s %s%s: %zu cached before (%zu not at %ld %%); after: %zu new, %zu old, %zu gone, %zu "
      "neither; %zu created, %zu under a new ID",
      word, Utf8(v.label).c_str(), v.control ? " (control)" : "", v.baseline, v.baselineNotOld,
      zoomBefore, v.newAfter, v.oldAfter, v.missAfter, v.otherAfter, v.created, v.idChanged);
  return text;
}

std::string TimeText(ULONGLONG written) {
  FILETIME time = {static_cast<DWORD>(written), static_cast<DWORD>(written >> 32)};
  SYSTEMTIME utc = {};
  if (!FileTimeToSystemTime(&time, &utc)) return "?";
  char text[40];
  std::snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u.%03u UTC", utc.wYear, utc.wMonth,
                utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
  return text;
}

std::string CacheFilesRecord(const std::map<std::string, CacheFile>& files) {
  std::string text(kCacheFilesHeader);
  text += '\n';
  for (const auto& [name, file] : files) {
    text += name + "\t" + std::to_string(file.size) + "\t" + std::to_string(file.written) + "\n";
  }
  return text;
}

namespace {

int BadUsage() {
  std::printf("usage:\n");
  PrintRefreshUsage();
  return 2;
}

// "16,32,256": sizes from 1 to the largest the cache keeps.
bool ParseSizes(const std::wstring& text, std::vector<UINT>* sizes) {
  sizes->clear();
  size_t start = 0;
  for (;;) {
    const size_t comma = text.find(L',', start);
    long size = 0;
    if (!ParseNumber(text.substr(start, comma == std::wstring::npos ? comma : comma - start), 1,
                     static_cast<long>(pdft::kMaxEdge), &size)) {
      return false;
    }
    sizes->push_back(static_cast<UINT>(size));
    if (comma == std::wstring::npos) return true;
    start = comma + 1;
  }
}

// The files of |args| (AddFiles), each by its final path (Canonicalize), the form File Explorer
// shows and the shell's notifications and views compare with. A file the refresh leaves alone
// (ClassifyCandidate: online-only, on the network, ...) is refused before the shell sees it,
// checked as named and again where a link leads; until then only its attributes are read
// (Canonicalize opens it for them). False, with a message, when one is refused or cannot be found.
bool FinalFiles(const std::vector<std::wstring>& args, size_t first,
                std::vector<std::filesystem::path>* files) {
  std::vector<std::wstring> named;
  for (size_t i = first; i < args.size(); ++i) {
    if (!AddFiles(args[i], &named)) return false;
  }
  for (const std::wstring& name : named) {
    std::filesystem::path file;
    pdft::CandidateSkip skip = pdft::CandidateSkip::kNone;
    HRESULT hr = SkipOf(name, &skip);
    if (SUCCEEDED(hr) && skip == pdft::CandidateSkip::kNone) hr = pdft::Canonicalize(name, &file);
    if (SUCCEEDED(hr) && skip == pdft::CandidateSkip::kNone) hr = SkipOf(file, &skip);
    if (FAILED(hr)) {
      std::printf("cannot find %s: 0x%08lX\n", Utf8(name).c_str(), static_cast<unsigned long>(hr));
      return false;
    }
    if (skip != pdft::CandidateSkip::kNone) {
      std::printf("refused: %s is %s, and the refresh leaves such files alone\n",
                  Utf8(name).c_str(), SkipText(skip));
      return false;
    }
    files->push_back(std::move(file));
  }
  return true;
}

bool ReadText(const std::wstring& file, std::string* text) {
  wil::unique_hfile handle(CreateFileW(
      file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  LARGE_INTEGER size = {};
  if (!handle || !GetFileSizeEx(handle.get(), &size) || size.QuadPart < 0 ||
      static_cast<ULONGLONG>(size.QuadPart) > kMaxTextBytes) {
    return false;
  }
  text->resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  return text->empty() ||
         (ReadFile(handle.get(), text->data(), static_cast<DWORD>(text->size()), &read, nullptr) &&
          read == text->size());
}

// ---- incache ----------------------------------------------------------------------------------

int RunInCache(const std::vector<std::wstring>& args) {
  bool plain = false;
  std::vector<UINT> sizes(std::begin(pdft::kRefreshSizes), std::end(pdft::kRefreshSizes));
  std::optional<std::wstring> label, record;
  size_t i = 0;
  for (; i < args.size() && args[i].starts_with(L"--"); ++i) {
    const bool value = i + 1 < args.size();
    if (args[i] == L"--plain") {
      plain = true;
    } else if (args[i] == L"--sizes" && value) {
      if (!ParseSizes(args[++i], &sizes)) return BadUsage();
    } else if (args[i] == L"--label" && value && !args[i + 1].empty()) {
      label = args[++i];
    } else if (args[i] == L"--record" && value) {
      record = args[++i];
    } else {
      return BadUsage();
    }
  }
  if (i == args.size()) return BadUsage();
  std::vector<std::filesystem::path> files;
  if (!FinalFiles(args, i, &files)) return 2;
  pdft::ProcessContext context;
  if (const std::optional<int> refused = Refusal(&context)) return *refused;

  std::unique_ptr<pdft::ThumbnailCacheOps> cache;
  HRESULT hr = pdft::MakeThumbnailCacheOps(&cache);
  if (FAILED(hr)) {
    std::printf("cannot create the thumbnail cache: 0x%08lX\n", static_cast<unsigned long>(hr));
    return 2;
  }
  // Never extracts: the question only (refresh.h's probe, or without the ideal size only).
  const DWORD flags = plain ? static_cast<DWORD>(WTS_INCACHEONLY) : pdft::kRefreshProbeFlags;
  bool failed = false;
  const std::vector<CacheRecord> records =
      AskCache(*cache, files, sizes, flags, label, true, &failed);
  int result = failed ? 1 : 0;
  if (record && !pdft::test::WriteRecords(*record, records)) {
    std::printf("cannot write %s (a label or a name it cannot hold?)\n", Utf8(*record).c_str());
    result = 1;
  }
  return result;
}

// ---- refresh ----------------------------------------------------------------------------------

// "item+dir" and so on: RefreshNotify bits; "none" alone is 0.
bool ParseNotify(const std::wstring& text, unsigned* notify) {
  if (text == L"none") {
    *notify = 0;
    return true;
  }
  static const std::map<std::wstring, unsigned> kWords = {{L"item", pdft::kNotifyItems},
                                                          {L"dir", pdft::kNotifyFolders},
                                                          {L"views", pdft::kRefreshViews},
                                                          {L"assoc", pdft::kNotifyAssociations}};
  *notify = 0;
  size_t start = 0;
  for (;;) {
    const size_t plus = text.find(L'+', start);
    const auto word =
        kWords.find(text.substr(start, plus == std::wstring::npos ? plus : plus - start));
    if (word == kWords.end()) return false;
    *notify |= word->second;
    if (plus == std::wstring::npos) return true;
    start = plus + 1;
  }
}

// The extraction variants: the flags of the call that makes a thumbnail again, and of a second,
// plain one that stores it (force-then-extract only).
bool ParseExtract(const std::wstring& text, pdft::RefreshFilesOptions* options) {
  options->storeFlags.reset();
  if (text == L"product") {
    options->extractFlags = pdft::kRefreshExtractFlags;
  } else if (text == L"force") {
    options->extractFlags = WTS_FORCEEXTRACTION;
  } else if (text == L"slowreclaim") {
    options->extractFlags = WTS_FORCEEXTRACTION | WTS_SLOWRECLAIM;
  } else if (text == L"instance") {
    options->extractFlags = WTS_FORCEEXTRACTION | WTS_INSTANCESURROGATE;
  } else if (text == L"force-then-extract") {
    options->extractFlags = WTS_FORCEEXTRACTION;
    options->storeFlags = static_cast<DWORD>(WTS_EXTRACT);
  } else {
    return false;
  }
  return true;
}

void PrintTimes(const char* what, std::vector<DWORD> times) {
  std::printf("%s: %s\n", what, TimesText(std::move(times)).c_str());
}

int RunRefresh(const std::vector<std::wstring>& args) {
  pdft::RefreshFilesOptions options;  // the product's sizes, flags and notification
  std::optional<long> cancelAfter;
  std::optional<std::wstring> timing;
  size_t i = 0;
  for (; i < args.size() && args[i].starts_with(L"--"); ++i) {
    if (i + 1 == args.size()) return BadUsage();
    const std::wstring& name = args[i];
    const std::wstring& value = args[++i];
    bool ok = true;
    if (name == L"--probe") {
      ok = value == L"ideal" || value == L"plain";
      options.probeFlags =
          value == L"plain" ? static_cast<DWORD>(WTS_INCACHEONLY) : pdft::kRefreshProbeFlags;
    } else if (name == L"--extract") {
      ok = ParseExtract(value, &options);
    } else if (name == L"--notify") {
      ok = ParseNotify(value, &options.notify);
    } else if (name == L"--sizes") {
      ok = ParseSizes(value, &options.sizes);
    } else if (name == L"--cancel-after") {
      long ms = 0;
      ok = ParseNumber(value, 0, kMaxCancelAfterMs, &ms);
      cancelAfter = ms;
    } else if (name == L"--timing") {
      timing = value;
    } else {
      ok = false;
    }
    if (!ok) return BadUsage();
  }
  if (i == args.size()) return BadUsage();
  std::vector<std::filesystem::path> files;
  if (!FinalFiles(args, i, &files)) return 2;
  files = pdft::GroupByFolder(std::move(files));
  pdft::ProcessContext context;
  if (const std::optional<int> refused = Refusal(&context)) return *refused;

  std::unique_ptr<pdft::ThumbnailCacheOps> cache;
  HRESULT hr = pdft::MakeThumbnailCacheOps(&cache);
  if (FAILED(hr)) {
    std::printf("cannot create the thumbnail cache: 0x%08lX\n", static_cast<unsigned long>(hr));
    return 2;
  }
  std::vector<DWORD> extractionMs;
  options.extractionMs = &extractionMs;
  options.clock = PreciseMs;

  // --cancel-after: a timer asks the refresh to stop, which it does between requests.
  std::stop_source stop;
  std::atomic<ULONGLONG> stopAskedAt{0};
  const ULONGLONG start = PreciseMs();
  std::jthread timer;
  if (cancelAfter) {
    timer = std::jthread([&stop, &stopAskedAt, ms = *cancelAfter](std::stop_token own) {
      std::mutex lock;
      std::condition_variable_any wake;
      std::unique_lock<std::mutex> held(lock);
      (void)wake.wait_for(held, own, std::chrono::milliseconds(ms), [] { return false; });
      if (own.stop_requested()) return;
      stopAskedAt = PreciseMs();
      stop.request_stop();
    });
  }
  pdft::RefreshCounts counts;
  hr = pdft::RefreshFiles(files, options, *cache, pdft::RealShellNotifier(), nullptr,
                          stop.get_token(), &counts);
  const ULONGLONG end = PreciseMs();
  if (timer.joinable()) {
    timer.request_stop();
    timer.join();
  }

  std::printf("checked %zu files: %zu thumbnails cached, %zu made again in %zu files, %zu failed",
              counts.files, counts.entriesCached, counts.refreshed, counts.filesRefreshed,
              counts.failed);
  if (counts.failed) {
    std::printf(" (first 0x%08lX)", static_cast<unsigned long>(counts.firstFailure));
  }
  std::printf("; %zu without a cached thumbnail, %zu gone\n", counts.notCached, counts.gone);
  std::printf("sizes refused:");
  if (counts.sizesDropped.empty()) std::printf(" none");
  for (const UINT size : counts.sizesDropped) std::printf(" %u", size);
  std::printf("\nms: %llu in all, asking %llu, making again %llu, telling File Explorer %llu\n",
              end - start, counts.probeMs, counts.extractMs, counts.notifyMs);
  PrintTimes("extractions", extractionMs);
  if (const ULONGLONG asked = stopAskedAt; asked != 0) {
    std::printf("stop asked after %llu ms; it stopped %llu ms later (%s)\n", asked - start,
                end > asked ? end - asked : 0, counts.stopped ? "stopped" : "had finished");
  }
  std::printf("result 0x%08lX\n", static_cast<unsigned long>(hr));
  int result = 0;
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    result = 3;
  } else if (hr != S_OK || counts.failed > 0) {
    result = 1;
  }
  if (timing) {
    std::string text = "n\tms\n";
    for (size_t n = 0; n < extractionMs.size(); ++n) {
      text += std::to_string(n + 1) + "\t" + std::to_string(extractionMs[n]) + "\n";
    }
    if (!WriteText(*timing, text)) {
      std::printf("cannot write %s\n", Utf8(*timing).c_str());
      result = std::max(result, 1);
    }
  }
  return result;
}

// ---- judge ------------------------------------------------------------------------------------

int RunJudge(const std::vector<std::wstring>& args) {
  std::optional<std::wstring> beforeFile, afterFile;
  long zoomBefore = 0, zoomAfter = 0;
  std::vector<std::wstring> controls;
  for (size_t i = 0; i < args.size(); i += 2) {
    if (i + 1 == args.size()) return BadUsage();
    const std::wstring& name = args[i];
    const std::wstring& value = args[i + 1];
    bool ok = true;
    if (name == L"--before") {
      beforeFile = value;
    } else if (name == L"--after") {
      afterFile = value;
    } else if (name == L"--zoom-before") {
      ok = ParseNumber(value, pdft::kMinZoom, pdft::kMaxZoom, &zoomBefore);
    } else if (name == L"--zoom-after") {
      ok = ParseNumber(value, pdft::kMinZoom, pdft::kMaxZoom, &zoomAfter);
    } else if (name == L"--control") {
      ok = !value.empty();
      controls.push_back(value);
    } else {
      ok = false;
    }
    if (!ok) return BadUsage();
  }
  if (!beforeFile || !afterFile || zoomBefore == 0 || zoomAfter == 0) return BadUsage();
  const auto read = [](const std::wstring& file, std::vector<CacheRecord>* records) {
    if (pdft::test::ReadRecords(file, records)) return true;
    std::printf("cannot read %s as an incache record\n", Utf8(file).c_str());
    return false;
  };
  std::vector<CacheRecord> before, after;
  if (!read(*beforeFile, &before) || !read(*afterFile, &after)) return 2;

  const std::vector<pdft::test::FolderVerdict> verdicts = pdft::test::Judge(
      before, after, static_cast<int>(zoomBefore), static_cast<int>(zoomAfter), controls);
  bool all = true;
  size_t refreshed = 0;  // folders that are not controls
  for (const pdft::test::FolderVerdict& v : verdicts) {
    std::printf("%s\n", VerdictText(v, zoomBefore).c_str());
    all = all && v.passA;
    if (!v.control) ++refreshed;
  }
  // A control that was never recorded cannot show that File Explorer left the thumbnails alone.
  for (const std::wstring& control : controls) {
    if (std::none_of(verdicts.begin(), verdicts.end(),
                     [&](const pdft::test::FolderVerdict& v) { return v.label == control; })) {
      std::printf("MISSING  %s (control): in neither record\n", Utf8(control).c_str());
      all = false;
    }
  }
  const bool pass = all && refreshed > 0;
  std::printf("Pass A: %s (%zu refreshed folders, %zu control folders)\n", pass ? "yes" : "no",
              refreshed, verdicts.size() - refreshed);
  return pass ? 0 : 1;
}

// ---- cachefiles -------------------------------------------------------------------------------

// A --record file (CacheFilesRecord) read back.
bool ReadCacheFiles(const std::wstring& file, std::map<std::string, CacheFile>* files) {
  std::string text;
  if (!ReadText(file, &text)) return false;
  size_t start = 0;
  bool header = true;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    if (end == std::string::npos) return false;
    std::string_view line(text.data() + start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    start = end + 1;
    if (header) {
      if (line != kCacheFilesHeader) return false;
      header = false;
      continue;
    }
    const size_t tab1 = line.find('\t');
    const size_t tab2 = tab1 == std::string_view::npos ? tab1 : line.find('\t', tab1 + 1);
    if (tab2 == std::string_view::npos || tab1 == 0) return false;
    CacheFile entry;
    const std::string_view size = line.substr(tab1 + 1, tab2 - tab1 - 1);
    const std::string_view written = line.substr(tab2 + 1);
    const auto sizeRead = std::from_chars(size.data(), size.data() + size.size(), entry.size);
    const auto writtenRead =
        std::from_chars(written.data(), written.data() + written.size(), entry.written);
    if (sizeRead.ec != std::errc() || sizeRead.ptr != size.data() + size.size() || size.empty() ||
        writtenRead.ec != std::errc() || written.empty() ||
        writtenRead.ptr != written.data() + written.size()) {
      return false;
    }
    (*files)[std::string(line.substr(0, tab1))] = entry;
  }
  return !header;
}

int RunCacheFiles(const std::vector<std::wstring>& args) {
  std::optional<std::wstring> record, diff;
  for (size_t i = 0; i < args.size(); i += 2) {
    if (i + 1 == args.size()) return BadUsage();
    if (args[i] == L"--record") {
      record = args[i + 1];
    } else if (args[i] == L"--diff") {
      diff = args[i + 1];
    } else {
      return BadUsage();
    }
  }
  std::map<std::string, CacheFile> earlier;
  if (diff && !ReadCacheFiles(*diff, &earlier)) {
    std::printf("cannot read %s as a cachefiles record\n", Utf8(*diff).c_str());
    return 2;
  }
  pdft::ProcessContext context;
  if (const std::optional<int> refused = Refusal(&context)) return *refused;

  std::map<std::string, CacheFile> now;
  std::map<std::string, DWORD> unreadable;
  const HRESULT hr = ListCacheFiles(&now, &unreadable);
  if (FAILED(hr)) {
    std::printf("cannot list the thumbnail cache: 0x%08lX\n", static_cast<unsigned long>(hr));
    return 1;
  }
  if (hr == S_FALSE) std::printf("there is no thumbnail cache folder\n");
  int result = unreadable.empty() ? 0 : 1;
  for (const auto& [name, error] : unreadable) {
    std::printf("%-28s unreadable (error %lu)\n", name.c_str(), error);
  }
  for (const auto& [name, file] : now) {
    std::printf("%-28s %12llu  %s\n", name.c_str(), file.size, TimeText(file.written).c_str());
  }
  std::printf("%zu cache files\n", now.size());
  if (diff) {
    std::set<std::string> names;
    for (const auto& [name, file] : earlier) names.insert(name);
    for (const auto& [name, file] : now) names.insert(name);
    std::printf("since %s:\n", Utf8(*diff).c_str());
    for (const std::string& name : names) {
      const auto was = earlier.find(name);
      const auto is = now.find(name);
      if (was == earlier.end()) {
        std::printf("  NEW      %s\n", name.c_str());
      } else if (is == now.end()) {
        std::printf("  GONE     %s\n", name.c_str());
      } else if (was->second.size != is->second.size || was->second.written != is->second.written) {
        const LONGLONG later = static_cast<LONGLONG>(is->second.written - was->second.written);
        std::printf("  CHANGED  %s  size %llu -> %llu, written %+lld ms\n", name.c_str(),
                    was->second.size, is->second.size, later / 10000);
      } else {
        std::printf("  same     %s\n", name.c_str());
      }
    }
  }
  if (record) {
    if (!WriteText(*record, CacheFilesRecord(now))) {
      std::printf("cannot write %s\n", Utf8(*record).c_str());
      result = 1;
    }
  }
  return result;
}

// ---- shellinfo --------------------------------------------------------------------------------

std::string IntegrityText(DWORD rid) {
  const char* name = rid == pdft::test::kUnknownIntegrity        ? "unknown"
                     : rid >= SECURITY_MANDATORY_SYSTEM_RID      ? "System"
                     : rid >= SECURITY_MANDATORY_HIGH_RID        ? "High"
                     : rid >= SECURITY_MANDATORY_MEDIUM_PLUS_RID ? "Medium Plus"
                     : rid >= SECURITY_MANDATORY_MEDIUM_RID      ? "Medium"
                     : rid >= SECURITY_MANDATORY_LOW_RID         ? "Low"
                                                                 : "Untrusted";
  char text[48];
  std::snprintf(text, sizeof(text), "%s (0x%lX)", name, static_cast<unsigned long>(rid));
  return text;
}

// The integrity of process |pid|'s token; why not when it cannot be read.
std::string ProcessIntegrity(DWORD pid) {
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  wil::unique_handle token;
  if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, token.put())) {
    const DWORD error = GetLastError();
    return "unreadable (error " + std::to_string(error) + ")";
  }
  return IntegrityText(pdft::test::TokenIntegrityRid(token.get()));
}

// explorer.exe processes (by image name) in session |session|; std::nullopt when the processes
// cannot be listed.
std::optional<size_t> CountExplorers(DWORD session) {
  HANDLE rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (rawSnapshot == INVALID_HANDLE_VALUE) return std::nullopt;
  wil::unique_handle snapshot(rawSnapshot);
  PROCESSENTRY32W entry = {sizeof(entry)};
  size_t count = 0;
  for (BOOL more = Process32FirstW(snapshot.get(), &entry); more;
       more = Process32NextW(snapshot.get(), &entry)) {
    DWORD itsSession = 0;
    if (CompareStringOrdinal(entry.szExeFile, -1, L"explorer.exe", -1, TRUE) == CSTR_EQUAL &&
        ProcessIdToSessionId(entry.th32ProcessID, &itsSession) && itsSession == session) {
      ++count;
    }
  }
  return count;
}

const char* RoleText(pdft::HolderRole role) {
  switch (role) {
    case pdft::HolderRole::kShell:
      return "the shell";
    case pdft::HolderRole::kFileExplorer:
      return "File Explorer";
    case pdft::HolderRole::kSurrogate:
      return "the thumbnail process";
    case pdft::HolderRole::kSelf:
      return "this process";
    case pdft::HolderRole::kLeftAlone:
      return "left alone";
  }
  return "?";
}

int RunShellInfo(const std::vector<std::wstring>& args) {
  bool windows = false, holders = false, explorerTest = false;
  for (const std::wstring& arg : args) {
    if (arg == L"--windows") {
      windows = true;
    } else if (arg == L"--holders") {
      holders = true;
    } else if (arg == L"--after-explorer-test") {
      explorerTest = true;
    } else {
      return BadUsage();
    }
  }
  if (explorerTest && !holders) return BadUsage();
  pdft::ProcessContext context;
  if (const std::optional<int> refused = Refusal(&context)) return *refused;
  int result = 0;
  // Before the cache files are listed: the test may be what opens or makes one.
  bool tested = false;
  if (explorerTest) {
    pdft::LiveTest live;
    const HRESULT started =
        pdft::RunLiveTest(pdft::kProbeThumbnailSize, nullptr, pdft::kLiveTestTimeoutMs, &live);
    tested = SUCCEEDED(started);
    if (!tested) {
      std::printf("Explorer test: not run (0x%08lX)\n", static_cast<unsigned long>(started));
      result = 1;
    } else if (live.hr == S_OK) {
      std::printf("Explorer test: %ldx%ld in %lu ms\n", live.size.cx, live.size.cy, live.ms);
    } else {
      std::printf("Explorer test: hr=0x%08lX after %lu ms\n", static_cast<unsigned long>(live.hr),
                  live.ms);
    }
  }
  const pdft::RebuildSystem& system = pdft::RealRebuildSystem();
  DWORD ownSession = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &ownSession);
  std::printf("this process: session %lu, integrity %s\n", ownSession,
              IntegrityText(context.integrityRid).c_str());

  const std::optional<pdft::ShellProcess> shell = system.Shell();
  if (shell) {
    std::printf("shell: pid %lu, %s, integrity %s\n", shell->pid,
                context.differentFromShellUser ? "ANOTHER user's" : "this user's",
                ProcessIntegrity(shell->pid).c_str());
  } else {
    std::printf("shell: none\n");
  }
  if (const std::optional<size_t> explorers = CountExplorers(ownSession)) {
    std::printf("explorer.exe in this session: %zu\n", *explorers);
  } else {
    std::printf("explorer.exe in this session: the processes cannot be listed\n");
    result = 1;
  }
  bool autoRestart = true;
  HRESULT hr = system.AutoRestartShell(&autoRestart);
  if (SUCCEEDED(hr)) {
    std::printf("AutoRestartShell: %s\n", autoRestart ? "on" : "off");
  } else {
    std::printf("AutoRestartShell: unreadable (0x%08lX)\n", static_cast<unsigned long>(hr));
    result = 1;
  }
  std::optional<pdft::RegistryValue> separate;
  hr = pdft::ReadRegistryValue(HKEY_CURRENT_USER, pdft::kExplorerAdvancedKey, kSeparateProcessValue,
                               &separate);
  if (FAILED(hr)) {
    std::printf("SeparateProcess: unreadable (0x%08lX)\n", static_cast<unsigned long>(hr));
    result = 1;
  } else if (!separate) {
    std::printf("SeparateProcess: not set (off)\n");
  } else if (const std::optional<DWORD> value = pdft::RegistryDword(*separate)) {
    std::printf("SeparateProcess: %s (%lu)\n", *value ? "on" : "off", *value);
  } else {
    std::printf("SeparateProcess: not a REG_DWORD (type %lu)\n", separate->type);
  }

  std::vector<std::filesystem::path> files;
  hr = system.CacheFiles(&files);
  const bool filesListed = SUCCEEDED(hr);
  if (!filesListed) {
    std::printf("cache files: unreadable (0x%08lX)\n", static_cast<unsigned long>(hr));
    result = 1;
  } else {
    std::printf("cache files: %zu%s\n", files.size(), hr == S_FALSE ? " (no cache folder)" : "");
  }

  if (windows && !shell) {
    std::printf("folder windows: not listed (no shell window)\n");
  } else if (windows) {
    std::vector<pdft::FolderWindow> list;
    hr = system.ListFolderWindows(kListTimeoutMs, &list);
    if (hr == S_FALSE) {
      std::printf("folder windows: File Explorer did not answer within %lu ms\n", kListTimeoutMs);
    } else if (FAILED(hr)) {
      std::printf("folder windows: unreadable (0x%08lX)\n", static_cast<unsigned long>(hr));
      result = 1;
    } else {
      std::set<ULONG_PTR> frames;
      size_t unnamed = 0;
      for (const pdft::FolderWindow& window : list) {
        frames.insert(window.frame);
        if (window.parsingName.empty()) ++unnamed;
      }
      std::printf("folder windows: %zu entries in %zu frames (%zu without a parsing name)\n",
                  list.size(), frames.size(), unnamed);
    }
  }

  if (holders && files.empty()) {
    std::printf("holders: not asked (%s)\n", filesListed ? "no cache files" : "not listed");
  } else if (holders) {
    std::vector<pdft::Holder> found;
    pdft::RebuildImages images;
    hr = pdft::FindHolders(files, &found);
    if (SUCCEEDED(hr)) hr = pdft::GetRebuildImages(&images);
    if (FAILED(hr)) {
      std::printf("holders: unreadable (0x%08lX)\n", static_cast<unsigned long>(hr));
      result = 1;
    } else {
      // Image leaf names only; Restart Manager's names for the programs are not shown.
      std::map<std::pair<std::string, std::string>, size_t> counts;
      bool self = false;
      for (const pdft::Holder& holder : found) {
        const pdft::HolderRole role =
            pdft::ClassifyHolder(holder, GetCurrentProcessId(), ownSession, shell, images);
        self = self || holder.pid == GetCurrentProcessId();
        const std::string leaf = holder.image.empty()
                                     ? "(image unreadable)"
                                     : Utf8(PathFindFileNameW(holder.image.c_str()));
        ++counts[{leaf, RoleText(role)}];
      }
      std::printf("holders: %zu processes\n", found.size());
      for (const auto& [what, count] : counts) {
        std::printf("  %zu x %s (%s)\n", count, what.first.c_str(), what.second.c_str());
      }
      if (tested) {
        std::printf("this process holds a cache file after the Explorer test: %s\n",
                    self ? "yes" : "no");
      } else {
        std::printf("this process (which has not used the cache) holds a cache file: %s\n",
                    self ? "yes" : "no");
      }
    }
  }
  return result;
}

// ---- opendialog -------------------------------------------------------------------------------

int RunOpenDialog(const std::vector<std::wstring>& args) {
  if (args.size() != 1 || args[0].starts_with(L"--")) return BadUsage();
  std::filesystem::path folder;
  HRESULT hr = pdft::Canonicalize(args[0], &folder);
  const DWORD attributes =
      SUCCEEDED(hr) ? GetFileAttributesW(folder.c_str()) : INVALID_FILE_ATTRIBUTES;
  if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    std::printf("not a folder: %s\n", Utf8(args[0]).c_str());
    return 2;
  }
  pdft::ProcessContext context;
  if (const std::optional<int> refused = Refusal(&context)) return *refused;

  wil::com_ptr_nothrow<IFileOpenDialog> dialog;
  hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(dialog.put()));
  FILEOPENDIALOGOPTIONS options = 0;
  if (SUCCEEDED(hr)) hr = dialog->GetOptions(&options);
  // Nothing chosen here is opened, and nothing is added to the user's recent files.
  if (SUCCEEDED(hr)) {
    hr = dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_NOCHANGEDIR |
                            FOS_DONTADDTORECENT);
  }
  const COMDLG_FILTERSPEC types[] = {{L"PDF files", L"*.pdf"}, {L"All files", L"*.*"}};
  if (SUCCEEDED(hr)) hr = dialog->SetFileTypes(ARRAYSIZE(types), types);
  if (SUCCEEDED(hr)) hr = dialog->SetTitle(L"thumbtest opendialog");
  wil::com_ptr_nothrow<IShellItem> item;
  if (SUCCEEDED(hr)) {
    hr = SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(item.put()));
  }
  if (SUCCEEDED(hr)) hr = dialog->SetFolder(item.get());
  if (FAILED(hr)) {
    std::printf("cannot set up the File Open dialog: 0x%08lX\n", static_cast<unsigned long>(hr));
    return 1;
  }
  std::printf("The File Open dialog is open at %s; it ends when it is closed.\n",
              Utf8(folder.filename().native()).c_str());
  std::fflush(stdout);
  hr = dialog->Show(nullptr);
  if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    std::printf("the File Open dialog failed: 0x%08lX\n", static_cast<unsigned long>(hr));
    return 1;
  }
  std::printf("closed\n");
  return 0;
}

}  // namespace

bool IsRefreshMode(const std::wstring& mode) {
  return mode == L"incache" || mode == L"refresh" || mode == L"judge" || mode == L"cachefiles" ||
         mode == L"shellinfo" || mode == L"opendialog" || mode == L"experiment";
}

int RunRefreshMode(int argc, wchar_t** argv) {
  // As the app: DLLs loaded by name (the delay-loaded ones) only from System32.
  if (FAILED(pdft::HardenProcess())) {
    std::printf("cannot limit where DLLs load from\n");
    return 2;
  }
  const std::wstring mode = argv[1];
  const std::vector<std::wstring> args(argv + 2, argv + argc);
  if (mode == L"judge") return RunJudge(args);  // reads two files, nothing else

  // COM as the app sets it up: the registry canary reads through WMI.
  const HRESULT com = pdft::InitializeComSecurity();
  if (FAILED(com)) {
    std::printf("cannot set up COM: 0x%08lX\n", static_cast<unsigned long>(com));
    return 2;
  }
  int result = 2;
  if (mode == L"incache") {
    result = RunInCache(args);
  } else if (mode == L"refresh") {
    result = RunRefresh(args);
  } else if (mode == L"cachefiles") {
    result = RunCacheFiles(args);
  } else if (mode == L"shellinfo") {
    result = RunShellInfo(args);
  } else if (mode == L"opendialog") {
    result = RunOpenDialog(args);
  } else if (mode == L"experiment") {
    result = RunExperiment(args);
  }
  CoUninitialize();
  return result;
}

void PrintRefreshUsage() {
  std::printf(
      "  thumbtest incache [--plain] [--sizes <list>] [--label <label>] [--record <tsv>]\n"
      "                    <file.pdf|dir>...\n"
      "  thumbtest refresh [--probe ideal|plain]\n"
      "                    [--extract product|force|slowreclaim|instance|force-then-extract]\n"
      "                    [--notify none|item|dir|views|assoc[+...]] [--sizes <list>]\n"
      "                    [--cancel-after <ms>] [--timing <tsv>] <file.pdf|dir>...\n"
      "  thumbtest judge --before <tsv> --after <tsv> --zoom-before <z> --zoom-after <z>\n"
      "                  [--control <label>]...\n"
      "  thumbtest cachefiles [--record <tsv>] [--diff <tsv>]\n"
      "  thumbtest shellinfo [--windows] [--holders [--after-explorer-test]]\n"
      "  thumbtest opendialog <folder>\n");
  PrintExperimentUsage();
  std::printf(
      "The refresh modes use the signed-in user's own thumbnail cache and File Explorer (judge\n"
      "reads only its two files): synthetic PDFs only, from a normal prompt with no app package\n"
      "above it (a Command Prompt opened with Win+R); elsewhere they refuse (exit 8).\n"
      "<list>: sizes such as 16,32,256.\n");
}

}  // namespace thumbtest
