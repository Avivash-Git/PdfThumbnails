// Refreshing PDF thumbnails in place: for each PDF and each thumbnail size, the cache is asked
// whether it holds that thumbnail; only the ones it holds are extracted again, out of process
// in the thumbnail process, with the zoom saved now, and replace the old ones. Then File Explorer
// is told. Never extracts in this process; never extracts without storing the result.
//
// The PDFs come from folders and their subfolders, each folder opened as itself and listed
// through its handle, and only while it is where its parent's listing put it: never into links
// or junctions (also where a folder on the way became one since), folders not on this PC yet, or
// the recycle bin. Files that are online-only or offline, on the network, too large for the
// handler, or at paths too long for File Explorer are skipped by what the listing says, before
// the shell sees them.
//
// What is logged: counts, HRESULTs and milliseconds. Never a path, a file or folder name.

#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "context.h"
#include "exit_code.h"
#include "registration.h"
#include "registry_roots.h"

namespace pdft {

// The thumbnail sizes asked about, in pixels: the cache's buckets.
inline constexpr UINT kRefreshSizes[] = {16, 32, 48, 96, 256, 768, 1280, 1920, 2560};

// The flags (WTS_FLAGS) of the question "is this thumbnail cached?": WTS_INCACHEONLY (never
// extract) | WTS_IDEALCACHESIZEONLY (only the bucket of the size asked for, so a small size is
// never answered from a larger one).
inline constexpr DWORD kRefreshProbeFlags = 0x1 | 0x8000;
// The flags of the extraction that replaces a cached thumbnail: WTS_FORCEEXTRACTION (never answer
// from the cache) | WTS_REQUIRESURROGATE (only in the thumbnail process). Never WTS_EXTRACTINPROC,
// never WTS_EXTRACTDONOTCACHE (checked when compiled and when run).
inline constexpr DWORD kRefreshExtractFlags = 0x4 | 0x800;

// How File Explorer is told (bits).
enum RefreshNotify : unsigned {
  kNotifyItems = 1,         // SHCNE_UPDATEITEM for each file, once its thumbnails are done
  kNotifyFolders = 2,       // SHCNE_UPDATEDIR for each folder, once its last file is done
  kRefreshViews = 4,        // IShellView::Refresh of the open views of those folders, at the end
  kNotifyAssociations = 8,  // SHCNE_ASSOCCHANGED, at the end
};
inline constexpr unsigned kRefreshNotify = kNotifyItems;

// Whether the flags are safe (pure): the probe has WTS_INCACHEONLY and neither
// WTS_FORCEEXTRACTION nor WTS_EXTRACTINPROC; the extraction has WTS_FORCEEXTRACTION and none of
// WTS_INCACHEONLY, WTS_EXTRACTINPROC, WTS_EXTRACTDONOTCACHE; |store| (the second call's flags,
// 0 when there is none) has neither WTS_EXTRACTINPROC nor WTS_EXTRACTDONOTCACHE.
bool AreSafeRefreshFlags(DWORD probe, DWORD extract, DWORD store) noexcept;

// Why the refresh does not start.
enum class RefreshRefusal {
  kNone,
  kElevated,        // 8
  kVirtualized,     // 8: or the registry check did not run
  kOtherShellUser,  // 8: File Explorer on this desktop is another account's
  kNotInstalled,    // 6: no class of ours is registered
  kNotOurHandler,   // 1: Windows uses another handler for .pdf thumbnails, or none
  kThumbnailsOff,   // 0: File Explorer shows icons only: nothing to refresh
};

struct RefreshConditions {
  bool elevated = false;
  bool writesChecked = false;          // the registry canary ran (ContextProbe::kWrite)
  bool virtualized = false;            // RefusesWrites(context.virtualization)
  bool otherShellUser = false;         // context.differentFromShellUser
  bool classRegistered = false;        // a class of ours, per user or for all users
  bool effectiveIsOurs = false;        // QueryEffectiveHandler named our class
  bool iconsOnly = false;              // ExplorerSettings::iconsOnly
  bool thumbnailsOffByPolicy = false;  // DisableThumbnails, the user's or the machine's
};

// The first refusal in the order of RefreshRefusal (pure).
RefreshRefusal DecideRefreshRefusal(const RefreshConditions& c) noexcept;

// The conditions of a refresh started in |context|: the context's own first; past a refusal they
// decide nothing more is read. Then whether a class of ours is registered (per user or for all
// users) and File Explorer's settings, through |roots|, and the handler Windows resolves for .pdf
// (|queries|). Read-only.
[[nodiscard]] HRESULT ReadRefreshConditions(const ProcessContext& context,
                                            const RegistryRoots& roots,
                                            const AssociationQueries& queries,
                                            RefreshConditions* conditions) noexcept;

// The log's word for |refusal| (pure): "elevated", "not-installed" and so on; "none" for kNone.
const wchar_t* RefreshRefusalToken(RefreshRefusal refusal) noexcept;

// The exit code of |refusal| (pure), as RefreshRefusal lists them; kOk for kNone.
ExitCode RefreshRefusalExitCode(RefreshRefusal refusal) noexcept;

// Why the refresh does not start (pure), for standard error, except kThumbnailsOff, which fails
// nothing (standard output). |context| names the account. Empty for kNone and kVirtualized (the
// caller's CannotRunHereMessage). May throw std::bad_alloc.
std::wstring RefreshRefusalText(RefreshRefusal refusal, const ProcessContext& context);

// Why a file is not refreshed.
enum class CandidateSkip {
  kNone,
  kPlaceholder,  // online-only: not on this PC (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS or _ON_OPEN)
  kOffline,      // FILE_ATTRIBUTE_OFFLINE
  kNetwork,      // a path that starts with two separators, or a drive that is not local
  kRecycleBin,   // in the recycle bin
  kTooLarge,     // larger than the handler reads (256 MiB): it keeps the plain icon anyway
  kLongPath,     // MAX_PATH characters or more
  kNotPdf,       // the real name does not end in .pdf
};

// Why |path| is skipped, from what a listing says about it (pure): |attributes| and |size| as
// listed, |driveType| GetDriveTypeW's answer for its volume (DRIVE_REMOTE, DRIVE_NO_ROOT_DIR and
// DRIVE_UNKNOWN count as the network). The first reason in the order of CandidateSkip; a
// "$Recycle.Bin" component counts in any case and position.
CandidateSkip ClassifyCandidate(std::wstring_view path, DWORD attributes, ULONGLONG size,
                                UINT driveType) noexcept;

// |files| grouped by folder (ignoring case), the folders in the order they first appear and the
// files of each in their order (pure). May throw std::bad_alloc.
std::vector<std::filesystem::path> GroupByFolder(std::vector<std::filesystem::path> files);

// What the cache answered.
struct CacheAnswer {
  bool bitmap = false;        // it returned a bitmap
  DWORD cacheFlags = 0;       // WTS_CACHEFLAGS: WTS_CACHED (2) when it came from the cache
  SIZE dims{};                // the bitmap's size
  std::array<BYTE, 16> id{};  // the thumbnail's ID in the cache (WTS_THUMBNAILID)
};

// The thumbnail cache as the refresh uses it: the real one (MakeThumbnailCacheOps) and the tests'
// fake.
class ThumbnailCacheOps {
 public:
  virtual ~ThumbnailCacheOps() = default;
  // The file to ask about next (SHCreateItemFromParsingName); not-found errors mean it is gone.
  [[nodiscard]] virtual HRESULT Select(const std::filesystem::path& file) noexcept = 0;
  // IThumbnailCache::GetThumbnail of the selected file at |size| pixels with |flags| (WTS_FLAGS).
  [[nodiscard]] virtual HRESULT Get(UINT size, DWORD flags, CacheAnswer* answer) noexcept = 0;
};

// The real one: one IThumbnailCache (CLSID_LocalThumbnailCache) in this process's apartment (the
// caller initialised COM; its thread must not change apartments while it is in use). Its Get
// refuses WTS_EXTRACTINPROC (E_INVALIDARG).
[[nodiscard]] HRESULT MakeThumbnailCacheOps(std::unique_ptr<ThumbnailCacheOps>* ops) noexcept;

// How File Explorer is told: the real one (RealShellNotifier) and the tests' fake.
class ShellNotifier {
 public:
  virtual ~ShellNotifier() = default;
  virtual void ItemUpdated(const std::filesystem::path& file) noexcept = 0;      // SHCNE_UPDATEITEM
  virtual void FolderUpdated(const std::filesystem::path& folder) noexcept = 0;  // SHCNE_UPDATEDIR
  // IShellView::Refresh of every open view of one of |folders| (ForEachViewShowing), and of the
  // desktop's view when one of them is the user's or the public desktop.
  virtual void RefreshViews(std::span<const std::filesystem::path> folders) noexcept = 0;
  virtual void AssociationsChanged() noexcept = 0;  // SHCNE_ASSOCCHANGED
};

// The real one: SHChangeNotify with SHCNF_PATHW | SHCNF_FLUSHNOWAIT for items and folders.
ShellNotifier& RealShellNotifier() noexcept;

struct RefreshFilesOptions {
  std::vector<UINT> sizes{std::begin(kRefreshSizes), std::end(kRefreshSizes)};
  DWORD probeFlags = kRefreshProbeFlags;
  DWORD extractFlags = kRefreshExtractFlags;
  // A second call after each extraction that succeeded, with these flags (WTS_EXTRACT, 0, is a
  // plain one); none when empty. The entry counts as refreshed only when both succeed.
  std::optional<DWORD> storeFlags;
  unsigned notify = kRefreshNotify;            // RefreshNotify bits
  std::function<ULONGLONG()> clock;            // milliseconds; empty: GetTickCount64
  std::vector<DWORD>* extractionMs = nullptr;  // (optional) how long each extraction took
};

enum class RefreshPhase { kFinding, kRefreshing, kNotifying };

struct RefreshProgress {
  RefreshPhase phase = RefreshPhase::kFinding;
  size_t files = 0;  // to refresh (0 while finding them)
  size_t done = 0;   // of them, checked so far
  size_t refreshed = 0;
  size_t failed = 0;
};

// Called when a phase starts and in between at most every 250 ms; what it throws is ignored.
using RefreshProgressFn = std::function<void(const RefreshProgress&)>;

struct RefreshCounts {
  // Refreshing (RefreshFiles).
  size_t files = 0;          // checked
  size_t gone = 0;           // no longer there
  size_t notCached = 0;      // the cache answered and held no thumbnail of them
  size_t entriesCached = 0;  // thumbnails the cache held (each file and size once)
  size_t refreshed = 0;      // ... made again
  // ... that could not be made again; also each file the shell could not open (other than gone)
  size_t failed = 0;
  HRESULT firstFailure = S_OK;     // the first of those failures
  size_t filesRefreshed = 0;       // files with a thumbnail made again
  std::vector<UINT> sizesDropped;  // sizes the cache refused (E_INVALIDARG): not asked again
  bool stopped = false;

  // Finding.
  std::array<size_t, 7> skipped{};  // per CandidateSkip, kPlaceholder first
  size_t fromWindows = 0;           // from open folder windows
  size_t fromDesktops = 0;          // from the desktops
  size_t fromIndex = 0;             // from Windows Search
  size_t fromWalk = 0;              // from a walk of folders
  bool indexUsed = false;
  bool walkUsed = false;
  bool capped = false;  // more files were there than a limit let through

  ULONGLONG findMs = 0;
  ULONGLONG probeMs = 0;
  ULONGLONG extractMs = 0;
  ULONGLONG notifyMs = 0;
};

// Refreshes the cached thumbnails of |files|, one request at a time (see the top of this file),
// and tells File Explorer as |options|.notify says. |files| should come grouped by folder
// (GroupByFolder), so each folder is told once. The stop is honoured between files and between
// sizes; a request already running finishes first (the handler's own limits bound it). A size the
// cache refuses with E_INVALIDARG is not asked for again. Fills the refreshing fields of *counts
// (from |files| to |stopped|) and the probe, extract and notify times, and leaves the others as
// they are. S_OK when every file was checked; HRESULT_FROM_WIN32(ERROR_CANCELLED) when stopped;
// HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) when the cache refused every size, so the files after
// that were not asked about (what was refreshed until then is still told); E_INVALIDARG for no
// sizes or flags AreSafeRefreshFlags refuses, before any call.
[[nodiscard]] HRESULT RefreshFiles(std::span<const std::filesystem::path> files,
                                   const RefreshFilesOptions& options, ThumbnailCacheOps& cache,
                                   ShellNotifier& notifier, const RefreshProgressFn& progress,
                                   std::stop_token stop, RefreshCounts* counts) noexcept;

struct WalkLimits {
  size_t maxFiles = 20000;           // at most this many PDFs
  DWORD maxMs = 0;                   // 0: no time limit
  int maxDepth = 32;                 // subfolder levels below a folder given (0: none)
  std::function<ULONGLONG()> clock;  // milliseconds; empty: GetTickCount64
};

// The PDFs in |folders| (absolute) and their subfolders, each once, a folder's files before its
// subfolders: each folder opened as itself and listed through its handle (the long names only),
// a subfolder only while its final path is its parent's and the name listed there; never into
// links, junctions, folders not on this PC yet (a folder given included) or $Recycle.Bin; a
// folder that cannot be opened is left out. Files ClassifyCandidate skips are counted in
// counts->skipped, the files found added to counts->fromWalk; counts->walkUsed is set, and
// counts->capped when a limit stopped the walk early. Folder names stay in memory. S_OK;
// HRESULT_FROM_WIN32(ERROR_CANCELLED) when stopped (the files found so far); E_INVALIDARG for a
// relative folder.
[[nodiscard]] HRESULT FindPdfsInFolders(std::span<const std::filesystem::path> folders,
                                        const WalkLimits& limits, std::stop_token stop,
                                        std::vector<std::filesystem::path>* files,
                                        RefreshCounts* counts) noexcept;

struct PdfRefreshOptions {
  // The folders to refresh, with their subfolders (ValidateRefreshFolder's form); at least one.
  std::vector<std::filesystem::path> folders;
  RefreshFilesOptions files;
  WalkLimits limits;
};

// Refreshes the PDF thumbnails in |options|.folders: FindPdfsInFolders, GroupByFolder, then
// RefreshFiles with the real thumbnail cache and RealShellNotifier. Runs on the calling thread,
// in a single-threaded apartment (it initialises COM itself and undoes that), and the caller
// holds the operation lock. S_OK with *counts; HRESULT_FROM_WIN32(ERROR_CANCELLED) when stopped
// and HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) when the cache refused every size (RefreshFiles;
// the counts so far); otherwise a failure only when it could not start (E_INVALIDARG: no folder,
// no size, or flags AreSafeRefreshFlags refuses). Logs its start and end (component "refresh";
// counts only).
[[nodiscard]] HRESULT RefreshPdfThumbnails(const PdfRefreshOptions& options,
                                           const RefreshProgressFn& progress, std::stop_token stop,
                                           RefreshCounts* counts) noexcept;

// The same with |cache| and |notifier| (tests: fakes); COM is the caller's.
[[nodiscard]] HRESULT RefreshPdfThumbnails(const PdfRefreshOptions& options,
                                           ThumbnailCacheOps& cache, ShellNotifier& notifier,
                                           const RefreshProgressFn& progress, std::stop_token stop,
                                           RefreshCounts* counts) noexcept;

// What is wrong with a --folder.
enum class FolderProblem {
  kNone,
  kBadName,     // not a usable path (empty, an embedded null, a name Windows rejects, unreadable)
  kDevicePath,  // \\.\..., \\?\..., \??\...: a device or the raw object namespace
  kNotFound,    // nothing there
  kNotAFolder,  // a file, a pipe, a device
  kNetwork,     // on another computer (a UNC path or a network drive)
};

// Checks the folder a user named: the text (TestFilePathProblem's rules), the path made absolute,
// a network path refused by its text before it is opened, then opened for attributes only,
// following links (a folder behind a junction is still the user's; a pipe's server gets an
// identification token at most), a folder on disk, and its final path (Canonicalize), which must
// be local too. S_OK with *problem kNone and *folder set; otherwise a failure HRESULT and
// *problem says what is wrong.
[[nodiscard]] HRESULT ValidateRefreshFolder(std::wstring_view text, std::filesystem::path* folder,
                                            FolderProblem* problem) noexcept;

// What is wrong with --folder |text| (pure), for standard error: |text| is the user's own, for the
// console only. Empty for kNone. May throw std::bad_alloc.
std::wstring FolderProblemText(std::wstring_view text, FolderProblem problem);

// What a refresh that ran says, line by line (pure), from RefreshPdfThumbnails' answer |hr| (S_OK,
// stopped, or every size refused; a failure to start has RefreshFailureText) and |counts|: what
// was refreshed, or that there was nothing to; the files skipped; the thumbnails that could not be
// made again; a cache that refused every size; a walk that stopped at |maxFiles|. Unless
// |refreshDefault| (kPdfRefreshDefault: the refresh experiment showed that open folder windows
// show the new thumbnails), it adds that they may not. May throw std::bad_alloc.
std::vector<std::wstring> RefreshReport(HRESULT hr, const RefreshCounts& counts, size_t maxFiles,
                                        bool refreshDefault);

// The exit code of a refresh that ran (pure): 3 when stopped; 1 when the cache refused every
// size, a thumbnail could not be made again or a limit stopped the walk; 5 for a failure to
// start; 0 otherwise, nothing to refresh included.
ExitCode RefreshExitCode(HRESULT hr, const RefreshCounts& counts) noexcept;

// Why the refresh could not start (pure). May throw std::bad_alloc.
std::wstring RefreshFailureText(HRESULT failure);

// The progress line for |progress| (pure); empty where there is nothing to say. May throw
// std::bad_alloc.
std::wstring RefreshProgressText(const RefreshProgress& progress);

}  // namespace pdft
