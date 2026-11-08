#include "refresh.h"

#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <wil/com.h>
#include <wil/resource.h>
#include <wil/result_macros.h>

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

#include "explorer_settings.h"
#include "live_test.h"
#include "log.h"
#include "paths.h"
#include "processes.h"
#include "project_config.h"
#include "registration.h"
#include "registry_roots.h"
#include "search_index.h"
#include "settings.h"
#include "shell_windows.h"
#include "strings.h"

namespace pdft {
namespace {

static_assert((kRefreshExtractFlags & (WTS_EXTRACTINPROC | WTS_EXTRACTDONOTCACHE)) == 0,
              "a refresh must never extract in this process, nor without storing the result");
static_assert((kRefreshExtractFlags & WTS_FORCEEXTRACTION) != 0,
              "a refresh must extract again, never answer from the cache");
static_assert((kRefreshProbeFlags & WTS_INCACHEONLY) != 0 &&
                  (kRefreshProbeFlags & (WTS_FORCEEXTRACTION | WTS_EXTRACTINPROC)) == 0,
              "asking whether a thumbnail is cached must never extract it");
static_assert(kRefreshProbeFlags == WTS_INCACHEONLY);
static_assert(kRefreshExtractFlags == (WTS_FORCEEXTRACTION | WTS_REQUIRESURROGATE));
static_assert(std::tuple_size_v<decltype(RefreshCounts::skipped)> ==
                  static_cast<size_t>(CandidateSkip::kNotPdf),
              "one count per reason to skip");

constexpr wchar_t kLogComponent[] = L"refresh";

constexpr HRESULT kCancelled = HRESULT_FROM_WIN32(ERROR_CANCELLED);

// The cache refused every size (E_INVALIDARG), so nothing more could be asked.
constexpr HRESULT kNoSizeLeft = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

// The largest size IThumbnailCache::GetThumbnail is documented to take; a larger one it refuses
// is no surprise.
constexpr UINT kLargestDocumentedSize = 1024;

// Windows paths cannot be longer than this (UNICODE_STRING counts bytes in 16 bits).
constexpr size_t kMaxPathChars = 32768;

// Progress is reported at most this often between phases.
constexpr ULONGLONG kProgressEveryMs = 250;

// The handler reads a whole PDF into memory and leaves larger ones the plain icon (its own
// kMaxFileBytes).
constexpr ULONGLONG kMaxPdfBytes = 256ull << 20;

// How much of a folder's listing one call reads.
constexpr size_t kListingBytes = 64 * 1024;

// How long File Explorer gets to refresh its open views.
constexpr DWORD kViewRefreshTimeoutMs = 5000;

// Not on this PC, or not yet: an online-only file or folder of a cloud provider, or offline.
constexpr DWORD kNotHere =
    FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_OFFLINE;

// Ordinal and ignoring case, like IEquals, so it orders what IEquals calls equal together.
struct IgnoreCaseLess {
  bool operator()(const std::wstring& a, const std::wstring& b) const noexcept {
    if (a.empty() || b.empty()) return a.empty() && !b.empty();
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                                static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
  }
};

bool IsSeparator(wchar_t c) noexcept { return c == L'\\' || c == L'/'; }

// \\server\share, or a device path: not a file on a local drive.
bool StartsWithTwoSeparators(std::wstring_view path) noexcept {
  return path.size() >= 2 && IsSeparator(path[0]) && IsSeparator(path[1]);
}

bool InRecycleBin(std::wstring_view path) noexcept {
  constexpr std::wstring_view kRecycleBin = L"$Recycle.Bin";
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = start;
    while (end < path.size() && !IsSeparator(path[end])) ++end;
    if (IEquals(path.substr(start, end - start), kRecycleBin)) return true;
    start = end + 1;
  }
  return false;
}

// The name (after the last separator) ends in ".pdf", ignoring case.
bool HasPdfExtension(std::wstring_view path) noexcept {
  const size_t separator = path.find_last_of(L"\\/");
  const std::wstring_view name =
      separator == std::wstring_view::npos ? path : path.substr(separator + 1);
  const size_t dot = name.rfind(L'.');
  return dot != std::wstring_view::npos && IEquals(name.substr(dot), L".pdf");
}

// The folder of |file|, as files are grouped by it.
std::wstring FolderOf(const std::filesystem::path& file) {
  std::filesystem::path folder = file.parent_path();
  folder.make_preferred();
  return folder.native();
}

// A link, a junction or a mount point: a reparse point that redirects the name elsewhere.
bool IsLink(DWORD attributes, DWORD reparseTag) noexcept {
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(reparseTag);
}

ULONGLONG Now(const std::function<ULONGLONG()>& clock) {
  return clock ? clock() : GetTickCount64();
}

ULONGLONG Elapsed(ULONGLONG start, ULONGLONG now) noexcept { return now > start ? now - start : 0; }

// Gone between being found and being asked about.
bool IsGone(HRESULT hr) noexcept {
  return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
         hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
         hr == HRESULT_FROM_WIN32(ERROR_INVALID_DRIVE);
}

void RecordMs(std::vector<DWORD>* times, ULONGLONG ms) noexcept {
  if (!times) return;
  try {
    times->push_back(static_cast<DWORD>(std::min<ULONGLONG>(ms, MAXDWORD)));
  } catch (...) {
    // The timings are for a look only.
  }
}

// The refreshing fields of |counts| back to nothing (RefreshFiles fills them).
void ResetRefreshing(RefreshCounts* counts) noexcept {
  counts->files = 0;
  counts->gone = 0;
  counts->notCached = 0;
  counts->otherSize = 0;
  counts->entriesCached = 0;
  counts->refreshed = 0;
  counts->extracted = 0;
  counts->superseded = 0;
  counts->failed = 0;
  counts->firstFailure = S_OK;
  counts->filesRefreshed = 0;
  counts->sizesDropped.clear();
  counts->stopped = false;
  counts->probeMs = 0;
  counts->extractMs = 0;
  counts->notifyMs = 0;
}

// Somewhere to look, a size to refresh, and flags AreSafeRefreshFlags allows.
bool AreUsableOptions(const PdfRefreshOptions& options, bool hasSources) noexcept {
  const RefreshFilesOptions& files = options.files;
  return (!options.folders.empty() || hasSources) && !files.sizes.empty() &&
         AreSafeRefreshFlags(files.probeFlags, files.extractFlags, files.storeFlags.value_or(0));
}

std::wstring Count(ULONGLONG n) { return std::to_wstring(n); }

// "<n> <one>" or "<n> <many>".
std::wstring Counted(ULONGLONG n, const wchar_t* one, const wchar_t* many) {
  return Count(n) + L" " + (n == 1 ? one : many);
}

const wchar_t* YesNo(bool value) noexcept { return value ? L"yes" : L"no"; }

void LogFailure(HRESULT hr) noexcept try {
  Log(LogLevel::kWarning, kLogComponent, L"refresh failed hr=" + HResultText(hr));
} catch (...) {
  // Logging is best effort.
}

void LogStart(const PdfRefreshOptions& options) noexcept try {
  Settings settings;
  const bool read = SUCCEEDED(LoadSettings(RegistryRoots{}, &settings));
  Log(LogLevel::kInfo, kLogComponent,
      std::wstring(L"refresh start source=") + (options.folders.empty() ? L"default" : L"folders") +
          L" zoom=" + (read ? std::to_wstring(settings.zoom) : std::wstring(L"unknown")) +
          L" notify=" + Count(options.files.notify));
} catch (...) {
  // Logging is best effort.
}

// Counts, HRESULTs and milliseconds only. A warning unless every file was checked at every size
// up to kLargestDocumentedSize and nothing failed.
void LogEnd(const RefreshCounts& c, HRESULT hr, ULONGLONG ms) noexcept try {
  // Every reason to skip but kNotPdf, which only files that are no PDFs have.
  std::wstring skipped;
  for (size_t i = 0; i < static_cast<size_t>(CandidateSkip::kLongPath); ++i) {
    if (i > 0) skipped += L',';
    skipped += Count(c.skipped[i]);
  }
  std::wstring dropped;
  for (const UINT size : c.sizesDropped) {
    if (!dropped.empty()) dropped += L',';
    dropped += Count(size);
  }
  if (dropped.empty()) dropped = L"none";
  const bool droppedDocumented =
      std::any_of(c.sizesDropped.begin(), c.sizesDropped.end(),
                  [](UINT size) { return size <= kLargestDocumentedSize; });
  const bool complete =
      hr == S_OK && c.failed == 0 && !c.capped && !c.stopped && !droppedDocumented;
  Log(complete ? LogLevel::kInfo : LogLevel::kWarning, kLogComponent,
      L"refresh end files=" + Count(c.files) + L" cached=" + Count(c.entriesCached) +
          L" other-size=" + Count(c.otherSize) + L" refreshed=" + Count(c.refreshed) +
          L" extracted=" + Count(c.extracted) + L" superseded=" + Count(c.superseded) +
          L" failed=" + Count(c.failed) + L" fail-hr=" + HResultText(c.firstFailure) + L" gone=" +
          Count(c.gone) + L" skipped=" + skipped + L" sources=" + Count(c.fromWindows) + L"," +
          Count(c.fromDesktops) + L"," + Count(c.fromIndex) + L"," + Count(c.fromWalk) +
          L" index=" + YesNo(c.indexUsed) + L" capped=" + YesNo(c.capped) + L" stopped=" +
          YesNo(c.stopped) + L" sizes-dropped=" + dropped + L" ms=" + Count(ms) + L" find-ms=" +
          Count(c.findMs) + L" probe-ms=" + Count(c.probeMs) + L" extract-ms=" +
          Count(c.extractMs) + L" notify-ms=" + Count(c.notifyMs));
} catch (...) {
  // Logging is best effort.
}

// One IThumbnailCache in this process's apartment, about one shell item at a time.
class RealThumbnailCacheOps final : public ThumbnailCacheOps {
 public:
  explicit RealThumbnailCacheOps(wil::com_ptr_nothrow<IThumbnailCache> cache) noexcept
      : cache_(std::move(cache)) {}

  HRESULT Select(const std::filesystem::path& file) noexcept override {
    item_.reset();
    // Not logged: a file may be gone, and failures say nothing new.
    return SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(item_.put()));
  }

  HRESULT Get(UINT size, DWORD flags, CacheAnswer* answer) noexcept override {
    *answer = {};
    // The handler must never run in this process, whatever the caller asks.
    if (flags & WTS_EXTRACTINPROC) return E_INVALIDARG;
    if (!item_) return E_UNEXPECTED;
    wil::com_ptr_nothrow<ISharedBitmap> bitmap;
    WTS_CACHEFLAGS cacheFlags = WTS_DEFAULT;
    WTS_THUMBNAILID id = {};
    // Not logged: a thumbnail that is not cached is an answer.
    const HRESULT hr = cache_->GetThumbnail(item_.get(), size, static_cast<WTS_FLAGS>(flags),
                                            bitmap.put(), &cacheFlags, &id);
    answer->cacheFlags = static_cast<DWORD>(cacheFlags);
    std::copy(std::begin(id.rgbKey), std::end(id.rgbKey), answer->id.begin());
    // Only its size is read; the bitmap goes at once.
    if (bitmap) {
      answer->bitmap = true;
      SIZE dims{};
      if (SUCCEEDED(bitmap->GetSize(&dims))) answer->dims = dims;
    }
    return hr;
  }

 private:
  wil::com_ptr_nothrow<IThumbnailCache> cache_;
  wil::com_ptr_nothrow<IShellItem> item_;
};

class RealShellNotifierImpl final : public ShellNotifier {
 public:
  void ItemUpdated(const std::filesystem::path& file) noexcept override {
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, file.c_str(), nullptr);
  }

  void FolderUpdated(const std::filesystem::path& folder) noexcept override {
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, folder.c_str(), nullptr);
  }

  void RefreshViews(std::span<const std::filesystem::path> folders) noexcept override try {
    if (folders.empty()) return;
    // The desktop shows the user's and the public desktop folder.
    bool desktop = false;
    for (const KNOWNFOLDERID* id : {&FOLDERID_Desktop, &FOLDERID_PublicDesktop}) {
      wil::unique_cotaskmem_string known;
      if (FAILED(SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &known))) continue;
      for (const std::filesystem::path& folder : folders) {
        if (SameImagePath(folder.native(), known.get())) desktop = true;
      }
    }
    // The action uses nothing of this frame: after a timeout it may still run on the watchdog's
    // thread.
    size_t found = 0;
    (void)ForEachViewShowing(
        folders, desktop, kViewRefreshTimeoutMs,
        [](IShellBrowser*, IShellView* view) { (void)view->Refresh(); }, &found);
  } catch (...) {
    // Telling File Explorer is best effort.
  }

  void AssociationsChanged() noexcept override { (void)NotifyAssocChanged(RegistryRoots{}); }
};

// What a walk has found so far.
struct WalkState {
  const WalkLimits* limits = nullptr;
  std::stop_token stop;
  std::vector<std::filesystem::path>* files = nullptr;
  RefreshCounts* counts = nullptr;
  std::vector<ULONGLONG> buffer;  // FILE_FULL_DIR_INFO holds 64-bit fields: aligned for them
  bool capped = false;
  bool stopped = false;
};

// GetDriveTypeW's answer for the volume |folder| is on; DRIVE_REMOTE for a UNC path.
UINT DriveTypeOf(const std::filesystem::path& folder) noexcept {
  if (StartsWithTwoSeparators(folder.native())) return DRIVE_REMOTE;
  wchar_t volume[MAX_PATH + 1] = {};
  if (!GetVolumePathNameW(folder.c_str(), volume, ARRAYSIZE(volume))) return DRIVE_UNKNOWN;
  return GetDriveTypeW(volume);
}

// Where Windows resolves the open |handle| to, on the volume's device
// (\Device\HarddiskVolume3\Users\...), without a separator at its end.
HRESULT FinalNtPath(HANDLE handle, std::wstring* path) noexcept try {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    RETURN_LAST_ERROR_IF(length == 0);
    if (length < buffer.size()) {
      buffer.resize(length);
      break;
    }
    // Too small: |length| is the size needed, terminating null included.
    RETURN_HR_IF(HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE), length > kMaxPathChars);
    buffer.resize(length);
  }
  while (!buffer.empty() && buffer.back() == L'\\') buffer.pop_back();
  *path = std::move(buffer);
  return S_OK;
}
CATCH_RETURN();

// A folder still to be listed: its path, how many levels below a folder given it is, and where it
// must really be (FinalNtPath: its parent's final path and the name the parent's listing showed;
// empty for a folder given, which is wherever its path leads).
struct PendingFolder {
  std::filesystem::path path;
  int depth = 0;
  std::wstring finalPath;
};

// Lists |folder|, opened as itself: its PDFs go to |state|, and its subfolders that may be walked
// to |subfolders| (none at the depth limit). A folder that cannot be opened, is a link, is not on
// this PC yet, or is no longer where its parent's listing put it (a folder on the way became a
// link since) gives nothing; a listing that fails half way keeps what it read.
void ListFolder(const PendingFolder& folder, UINT driveType, WalkState* state,
                std::vector<PendingFolder>* subfolders) {
  subfolders->clear();
  // A path that names a pipe gives its server an identification token at most.
  const wil::unique_hfile handle(
      CreateFileW(folder.path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT |
                      SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                  nullptr));
  if (!handle) return;
  FILE_ATTRIBUTE_TAG_INFO tag = {};
  if (!GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
      !(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (tag.FileAttributes & kNotHere) ||
      IsLink(tag.FileAttributes, tag.ReparseTag)) {
    return;
  }
  // FILE_FLAG_OPEN_REPARSE_POINT keeps only the last name from leading elsewhere.
  std::wstring finalPath;
  if (FAILED(FinalNtPath(handle.get(), &finalPath)) ||
      (!folder.finalPath.empty() && !IEquals(finalPath, folder.finalPath))) {
    return;
  }
  const bool enter = folder.depth < state->limits->maxDepth;
  FILE_INFO_BY_HANDLE_CLASS listing = FileFullDirectoryRestartInfo;
  for (;;) {
    if (state->stop.stop_requested()) {
      state->stopped = true;
      return;
    }
    // ERROR_NO_MORE_FILES at the end; any other error ends this folder's listing too.
    if (!GetFileInformationByHandleEx(
            handle.get(), listing, state->buffer.data(),
            static_cast<DWORD>(state->buffer.size() * sizeof(ULONGLONG)))) {
      return;
    }
    listing = FileFullDirectoryInfo;
    const BYTE* at = reinterpret_cast<const BYTE*>(state->buffer.data());
    for (;;) {
      const auto* entry = reinterpret_cast<const FILE_FULL_DIR_INFO*>(at);
      const std::wstring_view name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      const DWORD attributes = entry->FileAttributes;
      // For a reparse point, EaSize holds its tag.
      if (name != L"." && name != L".." && !IsLink(attributes, entry->EaSize)) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
          if (enter && !(attributes & kNotHere) && !IEquals(name, L"$Recycle.Bin")) {
            subfolders->push_back(PendingFolder{folder.path / name, folder.depth + 1,
                                                finalPath + L'\\' + std::wstring(name)});
          }
        } else if (HasPdfExtension(name)) {  // the long name: a listing has no 8.3 aliases
          std::filesystem::path file = folder.path / name;
          const CandidateSkip skip =
              ClassifyCandidate(file.native(), attributes,
                                static_cast<ULONGLONG>(entry->EndOfFile.QuadPart), driveType);
          if (skip != CandidateSkip::kNone) {
            ++state->counts->skipped[static_cast<size_t>(skip) - 1];
          } else if (state->files->size() >= state->limits->maxFiles) {
            state->capped = true;
            return;
          } else {
            state->files->push_back(std::move(file));
          }
        }
      }
      if (entry->NextEntryOffset == 0) break;
      at += entry->NextEntryOffset;
    }
  }
}

}  // namespace

bool AreSafeRefreshFlags(DWORD probe, DWORD extract, DWORD store) noexcept {
  constexpr DWORD kNever = WTS_EXTRACTINPROC | WTS_EXTRACTDONOTCACHE;
  const bool probeSafe =
      (probe & WTS_INCACHEONLY) != 0 && (probe & (WTS_FORCEEXTRACTION | WTS_EXTRACTINPROC)) == 0;
  const bool extractSafe =
      (extract & WTS_FORCEEXTRACTION) != 0 && (extract & (kNever | WTS_INCACHEONLY)) == 0;
  const bool storeSafe = (store & kNever) == 0;
  return probeSafe && extractSafe && storeSafe;
}

RefreshRefusal DecideRefreshRefusal(const RefreshConditions& c) noexcept {
  if (c.elevated) return RefreshRefusal::kElevated;
  if (!c.writesChecked || c.virtualized) return RefreshRefusal::kVirtualized;
  if (c.otherShellUser) return RefreshRefusal::kOtherShellUser;
  if (!c.classRegistered) return RefreshRefusal::kNotInstalled;
  if (!c.effectiveIsOurs) return RefreshRefusal::kNotOurHandler;
  if (c.iconsOnly || c.thumbnailsOffByPolicy) return RefreshRefusal::kThumbnailsOff;
  return RefreshRefusal::kNone;
}

HRESULT ReadRefreshConditions(const ProcessContext& context, const RegistryRoots& roots,
                              const AssociationQueries& queries,
                              RefreshConditions* conditions) noexcept try {
  *conditions = {};
  RefreshConditions& c = *conditions;
  c.elevated = context.elevated;
  c.writesChecked = context.probe == ContextProbe::kWrite;
  c.virtualized = RefusesWrites(context.virtualization);
  c.otherShellUser = context.differentFromShellUser;
  if (c.elevated || !c.writesChecked || c.virtualized || c.otherShellUser) return S_OK;

  std::optional<ClassReg> userClass;
  std::optional<ClassReg> machineClass;
  RETURN_IF_FAILED(ReadClass(roots, Scope::kUser, &userClass));
  RETURN_IF_FAILED(ReadClass(roots, Scope::kMachine, &machineClass));
  c.classRegistered = userClass.has_value() || machineClass.has_value();
  std::optional<std::wstring> effective;
  RETURN_IF_FAILED(QueryEffectiveHandler(queries, &effective));
  c.effectiveIsOurs = effective.has_value() && IsOurClsid(*effective);
  ExplorerSettings explorer;
  RETURN_IF_FAILED(ReadExplorerSettings(roots, &explorer));
  c.iconsOnly = explorer.iconsOnly;
  c.thumbnailsOffByPolicy =
      explorer.thumbnailsOffByUserPolicy || explorer.thumbnailsOffByMachinePolicy;
  return S_OK;
}
CATCH_RETURN();

const wchar_t* RefreshRefusalToken(RefreshRefusal refusal) noexcept {
  switch (refusal) {
    case RefreshRefusal::kElevated:
      return L"elevated";
    case RefreshRefusal::kVirtualized:
      return L"virtualized";
    case RefreshRefusal::kOtherShellUser:
      return L"other-shell-user";
    case RefreshRefusal::kNotInstalled:
      return L"not-installed";
    case RefreshRefusal::kNotOurHandler:
      return L"not-our-handler";
    case RefreshRefusal::kThumbnailsOff:
      return L"thumbnails-off";
    case RefreshRefusal::kNone:
      return L"none";
  }
  return L"none";
}

ExitCode RefreshRefusalExitCode(RefreshRefusal refusal) noexcept {
  switch (refusal) {
    case RefreshRefusal::kElevated:
    case RefreshRefusal::kVirtualized:
    case RefreshRefusal::kOtherShellUser:
      return ExitCode::kWrongContext;
    case RefreshRefusal::kNotInstalled:
      return ExitCode::kNotInstalled;
    case RefreshRefusal::kNotOurHandler:
      return ExitCode::kProblems;
    case RefreshRefusal::kThumbnailsOff:
    case RefreshRefusal::kNone:
      return ExitCode::kOk;
  }
  return ExitCode::kOk;
}

std::wstring RefreshRefusalText(RefreshRefusal refusal, const ProcessContext& context) {
  switch (refusal) {
    case RefreshRefusal::kElevated:
      return L"Cannot run here: running as administrator. The thumbnail cache is the signed-in "
             L"user's own, and this program does not make thumbnails as administrator; run "
             L"--refresh from a normal (not administrator) prompt.";
    case RefreshRefusal::kOtherShellUser:
      return L"Cannot run here: this program runs as " + context.userAccount +
             L", but File Explorer on this desktop belongs to another account, and the thumbnail "
             L"cache is each account's own. Run it as the signed-in user.";
    case RefreshRefusal::kNotInstalled:
      return std::wstring(kProductName) + L" is not installed here; there is nothing to refresh.";
    case RefreshRefusal::kNotOurHandler:
      return L"Windows uses another program's handler for .pdf thumbnails, or none, so a refresh "
             L"would not make them with " +
             std::wstring(kProductName) + L". Run --fix --take-back to use " +
             std::wstring(kProductName) + L" again.";
    case RefreshRefusal::kThumbnailsOff:
      return L"File Explorer shows icons only, so there are no thumbnails to refresh.";
    case RefreshRefusal::kVirtualized:
    case RefreshRefusal::kNone:
      // kVirtualized uses the caller's CannotRunHereMessage(context); kNone is no refusal.
      return {};
  }
  return {};
}

CandidateSkip ClassifyCandidate(std::wstring_view path, DWORD attributes, ULONGLONG size,
                                UINT driveType) noexcept {
  if (attributes & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) {
    return CandidateSkip::kPlaceholder;
  }
  if (attributes & FILE_ATTRIBUTE_OFFLINE) return CandidateSkip::kOffline;
  if (StartsWithTwoSeparators(path) || driveType == DRIVE_REMOTE ||
      driveType == DRIVE_NO_ROOT_DIR || driveType == DRIVE_UNKNOWN) {
    return CandidateSkip::kNetwork;
  }
  if (InRecycleBin(path)) return CandidateSkip::kRecycleBin;
  if (size > kMaxPdfBytes) return CandidateSkip::kTooLarge;
  if (path.size() >= MAX_PATH) return CandidateSkip::kLongPath;
  if (!HasPdfExtension(path)) return CandidateSkip::kNotPdf;
  return CandidateSkip::kNone;
}

std::vector<std::filesystem::path> GroupByFolder(std::vector<std::filesystem::path> files) {
  std::map<std::wstring, size_t, IgnoreCaseLess> groupOf;
  std::vector<std::vector<std::filesystem::path>> groups;
  for (std::filesystem::path& file : files) {
    const auto [at, added] = groupOf.try_emplace(FolderOf(file), groups.size());
    if (added) groups.emplace_back();
    groups[at->second].push_back(std::move(file));
  }
  std::vector<std::filesystem::path> grouped;
  grouped.reserve(files.size());
  for (std::vector<std::filesystem::path>& group : groups) {
    for (std::filesystem::path& file : group) grouped.push_back(std::move(file));
  }
  return grouped;
}

bool HasShapeOf(SIZE dims, SIZE stored) noexcept {
  const LONG edge = std::max(dims.cx, dims.cy);
  const LONG storedEdge = std::max(stored.cx, stored.cy);
  if (dims.cx <= 0 || dims.cy <= 0 || stored.cx <= 0 || stored.cy <= 0) return false;
  // |stored| scaled to |edge|, rounded to the nearest pixel.
  const auto scaled = [edge, storedEdge](LONG side) {
    return static_cast<LONG>((2LL * side * edge + storedEdge) / (2LL * storedEdge));
  };
  return std::abs(scaled(stored.cx) - dims.cx) <= 1 && std::abs(scaled(stored.cy) - dims.cy) <= 1;
}

UINT CacheSizeOf(UINT size) noexcept {
  for (const UINT own : kRefreshSizes) {
    if (own >= size) return own;
  }
  return size;
}

CacheHit ClassifyCacheAnswer(UINT size, HRESULT hr, const CacheAnswer& answer) noexcept {
  if (hr == E_INVALIDARG) return CacheHit::kRefused;
  if (hr != S_OK || !answer.bitmap || (answer.cacheFlags & WTS_CACHED) == 0) {
    return CacheHit::kNotCached;
  }
  const UINT own = CacheSizeOf(size);
  UINT below = 0;  // the size before |own|'s
  for (const UINT other : kRefreshSizes) {
    if (other < own) below = other;
  }
  const LONG longer = std::max(answer.dims.cx, answer.dims.cy);
  const UINT edge = longer > 0 ? static_cast<UINT>(longer) : 0;
  if (edge > own) return CacheHit::kLarger;
  if (edge <= below) return CacheHit::kSmaller;
  return CacheHit::kHeld;
}

HRESULT MakeThumbnailCacheOps(std::unique_ptr<ThumbnailCacheOps>* ops) noexcept try {
  ops->reset();
  wil::com_ptr_nothrow<IThumbnailCache> cache;
  RETURN_IF_FAILED(CoCreateInstance(__uuidof(LocalThumbnailCache), nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(cache.put())));
  *ops = std::make_unique<RealThumbnailCacheOps>(std::move(cache));
  return S_OK;
}
CATCH_RETURN();

ShellNotifier& RealShellNotifier() noexcept {
  static RealShellNotifierImpl notifier;
  return notifier;
}

HRESULT RefreshFiles(std::span<const std::filesystem::path> files,
                     const RefreshFilesOptions& options, ThumbnailCacheOps& cache,
                     ShellNotifier& notifier, const RefreshProgressFn& progress,
                     std::stop_token stop, RefreshCounts* counts) noexcept try {
  ResetRefreshing(counts);
  RETURN_HR_IF(E_INVALIDARG, options.sizes.empty() ||
                                 !AreSafeRefreshFlags(options.probeFlags, options.extractFlags,
                                                      options.storeFlags.value_or(0)));
  const auto now = [&options]() { return Now(options.clock); };
  const auto report = [&](RefreshPhase phase) noexcept {
    if (!progress) return;
    try {
      progress(
          RefreshProgress{phase, files.size(), counts->files, counts->refreshed, counts->failed});
    } catch (...) {
      // A progress report is not worth stopping for.
    }
  };
  const auto failedWith = [counts](HRESULT hr) noexcept {
    ++counts->failed;
    if (SUCCEEDED(counts->firstFailure)) counts->firstFailure = hr;
  };
  std::vector<bool> dropped(options.sizes.size(), false);
  size_t sizesLeft = options.sizes.size();
  counts->sizesDropped.reserve(options.sizes.size());
  const auto drop = [&](size_t index) {
    if (dropped[index]) return;
    dropped[index] = true;
    --sizesLeft;
    counts->sizesDropped.push_back(options.sizes[index]);
  };
  // The question at |size| (WTS_INCACHEONLY), timed; its HRESULT in *hr when given.
  const auto ask = [&](UINT size, CacheAnswer* answer, HRESULT* hr = nullptr) noexcept {
    const ULONGLONG probeStart = now();
    const HRESULT probe = cache.Get(size, options.probeFlags, answer);
    counts->probeMs += Elapsed(probeStart, now());
    if (hr) *hr = probe;
    return ClassifyCacheAnswer(size, probe, *answer);
  };
  std::vector<size_t> held;  // per file: the indices of the sizes held
  held.reserve(options.sizes.size());

  // The folders whose files had a thumbnail refreshed, for the views refreshed at the end.
  std::vector<std::filesystem::path> viewFolders;
  bool folderRefreshed = false;  // a file of the folder under way had a thumbnail refreshed
  const auto finishFolder = [&](const std::filesystem::path& file) {
    if (!folderRefreshed) return;
    folderRefreshed = false;
    std::filesystem::path folder = file.parent_path();
    if (options.notify & kNotifyFolders) notifier.FolderUpdated(folder);
    if (options.notify & kRefreshViews) viewFolders.push_back(std::move(folder));
  };

  report(RefreshPhase::kRefreshing);
  ULONGLONG lastReport = now();
  bool stopped = false;
  const std::filesystem::path* last = nullptr;  // the file checked last
  for (size_t i = 0; i < files.size(); ++i) {
    if (stop.stop_requested()) {
      stopped = true;
      break;
    }
    // Nothing is left to ask the cache about this file or the rest.
    if (sizesLeft == 0) break;
    const std::filesystem::path& file = files[i];
    last = &file;
    ++counts->files;
    size_t fileRefreshed = 0;
    const HRESULT selected = cache.Select(file);
    if (IsGone(selected)) {
      ++counts->gone;
    } else if (FAILED(selected)) {
      failedWith(selected);
    } else {
      bool answered = false;  // the cache answered a question about this file
      // Every size is asked about before anything is made: a store changes what the others
      // answer.
      held.clear();
      for (size_t s = 0; s < options.sizes.size(); ++s) {
        if (dropped[s]) continue;
        if (stop.stop_requested()) {
          stopped = true;
          break;
        }
        const UINT size = options.sizes[s];
        CacheAnswer answer;
        const CacheHit hit = ask(size, &answer);
        if (hit == CacheHit::kRefused) {
          drop(s);
          continue;
        }
        answered = true;
        // Another size's thumbnail: this size's own file holds none to make again.
        if (hit == CacheHit::kLarger || hit == CacheHit::kSmaller) {
          ++counts->otherSize;
          continue;
        }
        if (hit != CacheHit::kHeld) continue;
        ++counts->entriesCached;
        held.push_back(s);
      }
      // The largest first. A store at one size replaces the file's whole record in the cache with
      // that size and every smaller one (build 26100), so a smaller size made after a larger one
      // would take the larger one away. Once one size is stored, each smaller one is asked about
      // again: held with the stored bitmap's shape, it is that store's, at the new zoom, and is
      // not made again; held with another shape, the store left the old one there (a cache that
      // does not replace the record), and it is made again; not held any more, nothing old is
      // left at that size (superseded). These questions are asked also after a stop: they make
      // nothing.
      std::sort(held.begin(), held.end(),
                [&options](size_t a, size_t b) { return options.sizes[a] > options.sizes[b]; });
      bool storedLarger = false;  // a larger size of this file was made again and stored
      SIZE storedDims{};          // ... the bitmap that store answered with
      for (const size_t s : held) {
        const UINT size = options.sizes[s];
        CacheAnswer answer;
        if (storedLarger) {
          HRESULT probe = S_OK;
          const CacheHit hit = ask(size, &answer, &probe);
          if (hit == CacheHit::kHeld && HasShapeOf(answer.dims, storedDims)) {
            ++counts->refreshed;
            ++fileRefreshed;
            continue;
          }
          if (hit == CacheHit::kRefused) {
            drop(s);
            failedWith(probe);
            continue;
          }
          if (hit != CacheHit::kHeld) {
            ++counts->superseded;
            continue;
          }
        }
        if (stopped || stop.stop_requested()) {
          stopped = true;
          if (!storedLarger) break;
          continue;
        }
        const ULONGLONG extractStart = now();
        HRESULT made = cache.Get(size, options.extractFlags, &answer);
        if (SUCCEEDED(made) && options.storeFlags) {
          made = cache.Get(size, *options.storeFlags, &answer);
        }
        const ULONGLONG spent = Elapsed(extractStart, now());
        counts->extractMs += spent;
        RecordMs(options.extractionMs, spent);
        if (SUCCEEDED(made)) {
          ++counts->refreshed;
          ++counts->extracted;
          ++fileRefreshed;
          if (!storedLarger) {
            storedLarger = true;
            storedDims = answer.bitmap ? answer.dims : SIZE{};
          }
        } else {
          failedWith(made);
          if (made == E_INVALIDARG) drop(s);
        }
      }
      if (answered && held.empty() && !stopped) ++counts->notCached;
    }

    const ULONGLONG notifyStart = now();
    if (fileRefreshed > 0) {
      ++counts->filesRefreshed;
      folderRefreshed = true;
      if (options.notify & kNotifyItems) notifier.ItemUpdated(file);
    }
    if (!stopped && (i + 1 == files.size() || !IEquals(FolderOf(file), FolderOf(files[i + 1])))) {
      finishFolder(file);
    }
    counts->notifyMs += Elapsed(notifyStart, now());
    if (stopped) break;

    const ULONGLONG t = now();
    if (Elapsed(lastReport, t) >= kProgressEveryMs) {
      report(RefreshPhase::kRefreshing);
      lastReport = t;
    }
  }

  // Ended half way through a folder (stopped, or no size left): File Explorer still hears about
  // what changed.
  const ULONGLONG notifyStart = now();
  if (last) finishFolder(*last);
  counts->notifyMs += Elapsed(notifyStart, now());
  if (counts->refreshed > 0 && (options.notify & (kRefreshViews | kNotifyAssociations))) {
    report(RefreshPhase::kNotifying);
    const ULONGLONG endStart = now();
    if ((options.notify & kRefreshViews) && !viewFolders.empty()) {
      notifier.RefreshViews(viewFolders);
    }
    if (options.notify & kNotifyAssociations) notifier.AssociationsChanged();
    counts->notifyMs += Elapsed(endStart, now());
  }
  if (stopped) {
    counts->stopped = true;
    return kCancelled;
  }
  return sizesLeft == 0 ? kNoSizeLeft : S_OK;
}
CATCH_RETURN();

HRESULT FindPdfsInFolders(std::span<const std::filesystem::path> folders, const WalkLimits& limits,
                          std::stop_token stop, std::vector<std::filesystem::path>* files,
                          RefreshCounts* counts) noexcept try {
  files->clear();
  for (const std::filesystem::path& folder : folders) {
    RETURN_HR_IF(E_INVALIDARG, !folder.is_absolute());
  }
  WalkState state;
  state.limits = &limits;
  state.stop = stop;
  state.files = files;
  state.counts = counts;
  state.buffer.resize(kListingBytes / sizeof(ULONGLONG));
  counts->walkUsed = true;

  const ULONGLONG start = Now(limits.clock);
  // Each folder once, also where one given is inside another.
  std::set<std::wstring, IgnoreCaseLess> walked;
  std::vector<PendingFolder> subfolders;
  for (const std::filesystem::path& root : folders) {
    // Every folder below |root| is checked to really be below it (ListFolder), so its files are
    // on a drive of this type.
    const UINT driveType = DriveTypeOf(root);
    // Depth first, each folder's files before its subfolders, those in their listing's order.
    std::vector<PendingFolder> pending;
    pending.push_back(PendingFolder{root, 0, {}});
    while (!pending.empty() && !state.capped && !state.stopped) {
      if (stop.stop_requested()) {
        state.stopped = true;
        break;
      }
      if (limits.maxMs != 0 && Elapsed(start, Now(limits.clock)) > limits.maxMs) {
        state.capped = true;
        break;
      }
      PendingFolder folder = std::move(pending.back());
      pending.pop_back();
      if (!walked.insert(folder.path.native()).second) continue;
      ListFolder(folder, driveType, &state, &subfolders);
      for (auto it = subfolders.rbegin(); it != subfolders.rend(); ++it) {
        pending.push_back(std::move(*it));
      }
    }
    if (state.capped || state.stopped) break;
  }
  counts->fromWalk += files->size();
  if (state.capped) counts->capped = true;
  return state.stopped ? kCancelled : S_OK;
}
CATCH_RETURN();

namespace {

// How long the shell is given to list its folder windows.
constexpr DWORD kWindowListWaitMs = 5000;

// A path on a drive: "C:\..." (never a UNC, device or shell namespace name).
bool IsDrivePath(const std::filesystem::path& path) noexcept {
  const std::wstring& text = path.native();
  return text.size() >= 3 &&
         ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) &&
         text[1] == L':' && (text[2] == L'\\' || text[2] == L'/');
}

bool IsLocalDriveType(UINT driveType) noexcept {
  return driveType != DRIVE_REMOTE && driveType != DRIVE_NO_ROOT_DIR && driveType != DRIVE_UNKNOWN;
}

void AppendKnownFolder(REFKNOWNFOLDERID id, std::vector<std::filesystem::path>* folders) {
  wil::unique_cotaskmem_string path;
  if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path))) {
    folders->emplace_back(path.get());
  }
}

class RealRefreshSourcesImpl final : public RefreshSources {
 public:
  HRESULT OpenFolders(std::vector<std::filesystem::path>* folders) noexcept override try {
    folders->clear();
    std::vector<FolderWindow> windows;
    // No shell, or none that answers: no windows to look at.
    if (ListFolderWindows(kWindowListWaitMs, &windows) != S_OK) return S_OK;
    for (const FolderWindow& window : windows) {
      if (!window.parsingName.empty()) folders->emplace_back(window.parsingName);
    }
    return S_OK;
  }
  CATCH_RETURN();
  HRESULT Desktops(std::vector<std::filesystem::path>* folders) noexcept override try {
    folders->clear();
    AppendKnownFolder(FOLDERID_Desktop, folders);
    AppendKnownFolder(FOLDERID_PublicDesktop, folders);
    return S_OK;
  }
  CATCH_RETURN();
  HRESULT Indexed(size_t cap, std::stop_token stop, std::vector<std::filesystem::path>* files,
                  bool* available) noexcept override {
    return QueryIndexedPdfs(cap, nullptr, stop, files, available);
  }
  HRESULT WalkFolders(std::vector<std::filesystem::path>* folders) noexcept override try {
    folders->clear();
    AppendKnownFolder(FOLDERID_Desktop, folders);
    AppendKnownFolder(FOLDERID_Documents, folders);
    AppendKnownFolder(FOLDERID_Downloads, folders);
    return S_OK;
  }
  CATCH_RETURN();
};

}  // namespace

RefreshSources& RealRefreshSources() noexcept {
  static RealRefreshSourcesImpl sources;
  return sources;
}

HRESULT FindDefaultPdfs(RefreshSources& sources, const WalkLimits& limits, std::stop_token stop,
                        std::vector<std::filesystem::path>* files, RefreshCounts* counts) noexcept
    try {
  files->clear();
  if (stop.stop_requested()) return kCancelled;
  std::set<std::wstring, IgnoreCaseLess> seen;
  const auto add = [&](std::vector<std::filesystem::path>* found, size_t* from) {
    for (std::filesystem::path& file : *found) {
      if (files->size() >= limits.maxFiles) {
        counts->capped = true;
        return;
      }
      if (seen.insert(file.native()).second) {
        files->push_back(std::move(file));
        ++*from;
      }
    }
  };
  // The PDFs in |folders| on a local drive, within |walk|, go to |from| instead of fromWalk.
  const auto look = [&](const std::vector<std::filesystem::path>& folders, const WalkLimits& walk,
                        size_t* from) -> HRESULT {
    std::vector<std::filesystem::path> local;
    for (const std::filesystem::path& folder : folders) {
      if (IsDrivePath(folder) && IsLocalDriveType(DriveTypeOf(folder))) local.push_back(folder);
    }
    if (local.empty()) return S_OK;
    std::vector<std::filesystem::path> found;
    const size_t fromWalk = counts->fromWalk;
    const bool walkUsed = counts->walkUsed;
    const HRESULT hr = FindPdfsInFolders(local, walk, stop, &found, counts);
    counts->fromWalk = fromWalk;
    counts->walkUsed = walkUsed;
    add(&found, from);
    return hr;
  };

  WalkLimits flat = limits;
  flat.maxDepth = 0;
  std::vector<std::filesystem::path> folders;
  if (FAILED(sources.OpenFolders(&folders))) folders.clear();
  RETURN_IF_FAILED_EXPECTED(look(folders, flat, &counts->fromWindows));
  if (FAILED(sources.Desktops(&folders))) folders.clear();
  RETURN_IF_FAILED_EXPECTED(look(folders, flat, &counts->fromDesktops));

  std::vector<std::filesystem::path> indexed;
  bool available = false;
  const HRESULT queried = sources.Indexed(limits.maxFiles, stop, &indexed, &available);
  if (queried == kCancelled) return kCancelled;
  if (FAILED(queried)) available = false;
  counts->indexUsed = available;
  if (available) {
    if (indexed.size() >= limits.maxFiles) counts->capped = true;
    std::array<UINT, 26> driveTypes{};  // 0 is DRIVE_UNKNOWN: not asked yet, or unknown
    std::vector<std::filesystem::path> usable;
    usable.reserve(indexed.size());
    for (std::filesystem::path& file : indexed) {
      if (stop.stop_requested()) return kCancelled;
      UINT driveType = DRIVE_REMOTE;
      if (IsDrivePath(file)) {
        const size_t letter = static_cast<size_t>(towupper(file.native()[0]) - L'A');
        if (driveTypes[letter] == 0) {
          const wchar_t root[] = {file.native()[0], L':', L'\\', L'\0'};
          driveTypes[letter] = GetDriveTypeW(root);
        }
        driveType = driveTypes[letter];
      } else if (!StartsWithTwoSeparators(file.native())) {
        continue;  // no file-system path at all
      }
      // Nothing on another computer is touched.
      WIN32_FILE_ATTRIBUTE_DATA data = {};
      if (IsLocalDriveType(driveType)) {
        if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) continue;  // gone
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      }
      const ULONGLONG size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
      const CandidateSkip skip =
          ClassifyCandidate(file.native(), data.dwFileAttributes, size, driveType);
      if (skip != CandidateSkip::kNone) {
        ++counts->skipped[static_cast<size_t>(skip) - 1];
      } else {
        usable.push_back(std::move(file));
      }
    }
    add(&usable, &counts->fromIndex);
  } else if (files->size() < limits.maxFiles) {
    if (FAILED(sources.WalkFolders(&folders))) folders.clear();
    WalkLimits walk = limits;
    walk.maxFiles = std::min(kFallbackWalkFiles, limits.maxFiles - files->size());
    walk.maxMs = limits.maxMs == 0 ? kFallbackWalkMs : std::min(limits.maxMs, kFallbackWalkMs);
    counts->walkUsed = true;
    RETURN_IF_FAILED_EXPECTED(look(folders, walk, &counts->fromWalk));
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT RefreshPdfThumbnails(const PdfRefreshOptions& options, const RefreshProgressFn& progress,
                             std::stop_token stop, RefreshCounts* counts) noexcept {
  *counts = {};
  RETURN_HR_IF(E_INVALIDARG, !AreUsableOptions(options, true));
  const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(init)) {
    LogFailure(init);
    return init;
  }
  const auto uninitialize = wil::scope_exit([]() noexcept { CoUninitialize(); });
  // Released before COM is.
  std::unique_ptr<ThumbnailCacheOps> cache;
  if (const HRESULT hr = MakeThumbnailCacheOps(&cache); FAILED(hr)) {
    LogFailure(hr);
    return hr;
  }
  return RefreshPdfThumbnails(options, *cache, RealShellNotifier(), progress, stop, counts,
                              &RealRefreshSources());
}

HRESULT RefreshPdfThumbnails(const PdfRefreshOptions& options, ThumbnailCacheOps& cache,
                             ShellNotifier& notifier, const RefreshProgressFn& progress,
                             std::stop_token stop, RefreshCounts* counts,
                             RefreshSources* sources) noexcept try {
  *counts = {};
  RETURN_HR_IF(E_INVALIDARG, !AreUsableOptions(options, sources != nullptr));
  const auto now = [&options]() { return Now(options.files.clock); };
  const ULONGLONG start = now();
  LogStart(options);

  if (progress) {
    try {
      progress(RefreshProgress{RefreshPhase::kFinding});
    } catch (...) {
      // A progress report is not worth stopping for.
    }
  }
  std::vector<std::filesystem::path> files;
  HRESULT hr = options.folders.empty()
                   ? FindDefaultPdfs(*sources, options.limits, stop, &files, counts)
                   : FindPdfsInFolders(options.folders, options.limits, stop, &files, counts);
  if (hr == S_OK) files = GroupByFolder(std::move(files));
  counts->findMs = Elapsed(start, now());
  if (hr == S_OK) {
    hr = RefreshFiles(files, options.files, cache, notifier, progress, stop, counts);
  } else if (hr == kCancelled) {
    counts->stopped = true;
  }
  // The counts of a run that ended early still say what was done.
  if (FAILED(hr) && hr != kCancelled && hr != kNoSizeLeft) {
    LogFailure(hr);
    return hr;
  }
  LogEnd(*counts, hr, Elapsed(start, now()));
  return hr;
}
CATCH_RETURN();

HRESULT ValidateRefreshFolder(std::wstring_view text, std::filesystem::path* folder,
                              FolderProblem* problem) noexcept try {
  folder->clear();
  *problem = FolderProblem::kNone;
  const auto refuse = [problem](FolderProblem why, HRESULT hr) noexcept {
    *problem = why;
    return hr;
  };
  const HRESULT badPath = HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);
  const HRESULT network = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  // On another computer: a UNC path, or a drive letter that names a network drive.
  const auto onNetwork = [](const std::filesystem::path& path) {
    if (StartsWithTwoSeparators(path.native())) return true;
    const std::filesystem::path root = path.root_path();
    return !root.empty() && GetDriveTypeW(root.c_str()) == DRIVE_REMOTE;
  };

  switch (TestFilePathProblem(text)) {
    case TestFileProblem::kNone:
      break;
    case TestFileProblem::kDevicePath:
      return refuse(FolderProblem::kDevicePath, badPath);
    default:
      return refuse(FolderProblem::kBadName, E_INVALIDARG);
  }
  std::error_code error;
  const std::filesystem::path full = std::filesystem::absolute(std::filesystem::path(text), error);
  if (error) return refuse(FolderProblem::kBadName, HRESULT_FROM_WIN32(error.value()));
  // A reserved name ("NUL", "COM1") becomes a device path only now ("\\.\NUL").
  if (TestFilePathProblem(full.native()) != TestFileProblem::kNone) {
    return refuse(FolderProblem::kDevicePath, badPath);
  }
  // A path that names another computer is refused before it is opened; a link on the way that
  // leads to one is seen only once opened (below).
  if (onNetwork(full)) return refuse(FolderProblem::kNetwork, network);

  // Attributes only, sharing everything; links followed; folders open too (backup semantics). A
  // path that names a pipe gives its server an identification token at most.
  wil::unique_hfile handle(CreateFileW(
      full.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (!handle) {
    const DWORD code = GetLastError();
    const bool missing = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ||
                         code == ERROR_INVALID_NAME || code == ERROR_BAD_NETPATH ||
                         code == ERROR_CANT_RESOLVE_FILENAME || code == ERROR_INVALID_DRIVE;
    return refuse(missing ? FolderProblem::kNotFound : FolderProblem::kBadName,
                  HRESULT_FROM_WIN32(code ? code : ERROR_OPEN_FAILED));
  }
  if (GetFileType(handle.get()) != FILE_TYPE_DISK) {
    return refuse(FolderProblem::kNotAFolder, HRESULT_FROM_WIN32(ERROR_DIRECTORY));
  }
  FILE_BASIC_INFO basic = {};
  if (!GetFileInformationByHandleEx(handle.get(), FileBasicInfo, &basic, sizeof(basic))) {
    const DWORD code = GetLastError();
    return refuse(FolderProblem::kBadName, HRESULT_FROM_WIN32(code ? code : ERROR_READ_FAULT));
  }
  if (!(basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
    return refuse(FolderProblem::kNotAFolder, HRESULT_FROM_WIN32(ERROR_DIRECTORY));
  }
  handle.reset();

  std::filesystem::path resolved;
  if (const HRESULT hr = Canonicalize(full, &resolved); FAILED(hr)) {
    return refuse(FolderProblem::kBadName, hr);
  }
  // A link may lead to another computer.
  if (onNetwork(resolved)) return refuse(FolderProblem::kNetwork, network);
  *folder = std::move(resolved);
  return S_OK;
}
CATCH_RETURN();

std::wstring FolderProblemText(std::wstring_view text, FolderProblem problem) {
  const std::wstring folder = L"--folder " + std::wstring(text) + L": ";
  switch (problem) {
    case FolderProblem::kBadName:
    case FolderProblem::kDevicePath:
      return folder + L"not a usable path.";
    case FolderProblem::kNotFound:
      return folder + L"no such folder.";
    case FolderProblem::kNotAFolder:
      return folder + L"not a folder.";
    case FolderProblem::kNetwork:
      return folder + L"network folders are not refreshed.";
    case FolderProblem::kNone:
      return {};
  }
  return {};
}

std::vector<std::wstring> RefreshReport(HRESULT hr, const RefreshCounts& c, size_t maxFiles,
                                        bool refreshDefault) {
  std::vector<std::wstring> lines;
  // Every reason to skip a PDF file (kNotPdf is for files that are none).
  size_t skipped = 0;
  for (size_t i = 0; i < static_cast<size_t>(CandidateSkip::kLongPath); ++i) {
    skipped += c.skipped[i];
  }
  if (hr == kCancelled) {
    lines.push_back(L"Stopped: " + Counted(c.refreshed, L"thumbnail", L"thumbnails") +
                    L" refreshed; the rest keep the old zoom. Run --refresh --folder again to "
                    L"finish.");
  } else if (c.files == 0) {
    // Files that were all skipped have their own line.
    if (skipped == 0) lines.push_back(L"No PDF files were found; there is nothing to refresh.");
  } else if (hr == S_OK && c.refreshed == 0 && c.failed == 0 && !c.capped) {
    // Not when a limit left files unchecked: those may have thumbnails to refresh.
    lines.push_back(
        L"None of these PDF files has a thumbnail File Explorer made; there is nothing to "
        L"refresh.");
  } else {
    lines.push_back(L"Refreshed " + Counted(c.refreshed, L"PDF thumbnail", L"PDF thumbnails") +
                    L" in " + Counted(c.filesRefreshed, L"file", L"files") + L" (" +
                    Counted(c.files, L"file checked", L"files checked") + L").");
  }
  if (skipped > 0) {
    lines.push_back(Counted(skipped, L"file was", L"files were") +
                    L" skipped: online-only or offline files, network files, files in the "
                    L"recycle bin, files over 256 MB and files at paths too long for File "
                    L"Explorer keep their thumbnails.");
  }
  if (c.failed > 0) {
    lines.push_back(Counted(c.failed, L"thumbnail", L"thumbnails") + L" could not be made again (" +
                    HResultText(c.firstFailure) + L"); File Explorer keeps the old " +
                    (c.failed == 1 ? L"one." : L"ones."));
  }
  if (hr == kNoSizeLeft) {
    lines.push_back(
        L"The thumbnail cache refused every thumbnail size, so the PDF files after these were "
        L"not checked; they keep their thumbnails.");
  }
  // A walk that stopped at its limit, when every file it found was then checked (a stop and a
  // cache that refused every size have lines of their own).
  if (c.capped && hr == S_OK) {
    lines.push_back(L"Only the first " + Count(maxFiles) +
                    L" PDF files found were checked; run --refresh --folder on smaller folders "
                    L"for the rest.");
  }
  if (c.refreshed > 0 && !refreshDefault) {
    lines.push_back(
        L"Folder windows that are open now may show the old thumbnails until you open the folder "
        L"again.");
  }
  return lines;
}

ExitCode RefreshExitCode(HRESULT hr, const RefreshCounts& c) noexcept {
  if (hr == kCancelled) return ExitCode::kCancelled;
  if (hr == kNoSizeLeft) return ExitCode::kProblems;
  if (FAILED(hr)) return ExitCode::kFailed;
  if (c.failed > 0 || c.capped) return ExitCode::kProblems;
  return ExitCode::kOk;
}

std::wstring RefreshFailureText(HRESULT failure) {
  return L"Cannot refresh the PDF thumbnails: " + HResultText(failure) + L". Nothing changed.";
}

std::wstring RefreshProgressText(const RefreshProgress& p) {
  switch (p.phase) {
    case RefreshPhase::kFinding:
      return L"Looking for PDF files...";
    case RefreshPhase::kRefreshing:
      if (p.files == 0) return {};
      return L"Checked " + Count(p.done) + L" of " + Counted(p.files, L"PDF file", L"PDF files") +
             L"; " + Counted(p.refreshed, L"thumbnail", L"thumbnails") + L" refreshed so far...";
    case RefreshPhase::kNotifying:
      return L"Telling File Explorer...";
  }
  return {};
}

}  // namespace pdft
