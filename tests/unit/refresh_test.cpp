// The PDF thumbnail refresh's pure parts and its engine (refresh.h): which flags are safe, the
// refusal order, which files are skipped, grouping by folder, RefreshFiles against a fake
// thumbnail cache and a fake File Explorer on a fake clock, and what --refresh --folder says: its
// refusals, the folder problems, the report, the exit codes and the progress lines. Nothing here
// asks the real thumbnail cache or tells the real File Explorer anything.

#include <windows.h>

#include "refresh.h"

#include <gtest/gtest.h>
#include <thumbcache.h>

#include <algorithm>
#include <map>
#include <new>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "project_config.h"
#include "strings.h"

namespace pdft {
namespace {

using std::filesystem::path;

// ---- AreSafeRefreshFlags ----

TEST(RefreshFlags, TheProductFlagsAreSafe) {
  EXPECT_TRUE(AreSafeRefreshFlags(kRefreshProbeFlags, kRefreshExtractFlags, 0));
  // The experiment's variants.
  EXPECT_TRUE(AreSafeRefreshFlags(WTS_INCACHEONLY, WTS_FORCEEXTRACTION, 0));
  EXPECT_TRUE(AreSafeRefreshFlags(kRefreshProbeFlags, WTS_FORCEEXTRACTION | WTS_SLOWRECLAIM, 0));
  EXPECT_TRUE(
      AreSafeRefreshFlags(kRefreshProbeFlags, WTS_FORCEEXTRACTION | WTS_INSTANCESURROGATE, 0));
  EXPECT_TRUE(AreSafeRefreshFlags(kRefreshProbeFlags, kRefreshExtractFlags, WTS_REQUIRESURROGATE));
}

TEST(RefreshFlags, RefusesExtractingHereOrWithoutStoring) {
  const DWORD probe = kRefreshProbeFlags;
  const DWORD extract = kRefreshExtractFlags;
  EXPECT_FALSE(AreSafeRefreshFlags(probe, extract | WTS_EXTRACTINPROC, 0));
  EXPECT_FALSE(AreSafeRefreshFlags(probe, extract | WTS_EXTRACTDONOTCACHE, 0));
  EXPECT_FALSE(AreSafeRefreshFlags(probe, WTS_REQUIRESURROGATE, 0));  // no WTS_FORCEEXTRACTION
  EXPECT_FALSE(AreSafeRefreshFlags(probe, extract | WTS_INCACHEONLY, 0));
  EXPECT_FALSE(AreSafeRefreshFlags(probe, extract, WTS_EXTRACTINPROC));
  EXPECT_FALSE(AreSafeRefreshFlags(probe, extract, WTS_EXTRACTDONOTCACHE));
}

TEST(RefreshFlags, TheProbeMustNeverExtract) {
  const DWORD extract = kRefreshExtractFlags;
  EXPECT_FALSE(AreSafeRefreshFlags(WTS_IDEALCACHESIZEONLY, extract, 0));  // no WTS_INCACHEONLY
  EXPECT_FALSE(AreSafeRefreshFlags(0, extract, 0));
  EXPECT_FALSE(AreSafeRefreshFlags(WTS_INCACHEONLY | WTS_FORCEEXTRACTION, extract, 0));
  EXPECT_FALSE(AreSafeRefreshFlags(WTS_INCACHEONLY | WTS_EXTRACTINPROC, extract, 0));
}

// ---- DecideRefreshRefusal ----

RefreshConditions Clean() {
  RefreshConditions c;
  c.writesChecked = true;
  c.classRegistered = true;
  c.effectiveIsOurs = true;
  return c;
}

TEST(RefreshRefusal, OrderAndTruthTable) {
  EXPECT_EQ(DecideRefreshRefusal(Clean()), RefreshRefusal::kNone);
  // Nothing set up: refused, never run.
  EXPECT_EQ(DecideRefreshRefusal(RefreshConditions{}), RefreshRefusal::kVirtualized);

  RefreshConditions c = Clean();
  c.elevated = true;
  c.virtualized = true;
  c.otherShellUser = true;
  c.classRegistered = false;
  c.effectiveIsOurs = false;
  c.iconsOnly = true;
  EXPECT_EQ(DecideRefreshRefusal(c), RefreshRefusal::kElevated);
  c.elevated = false;
  EXPECT_EQ(DecideRefreshRefusal(c), RefreshRefusal::kVirtualized);
  c.virtualized = false;
  EXPECT_EQ(DecideRefreshRefusal(c), RefreshRefusal::kOtherShellUser);
  c.otherShellUser = false;
  EXPECT_EQ(DecideRefreshRefusal(c), RefreshRefusal::kNotInstalled);
  c.classRegistered = true;
  EXPECT_EQ(DecideRefreshRefusal(c), RefreshRefusal::kNotOurHandler);
  c.effectiveIsOurs = true;
  EXPECT_EQ(DecideRefreshRefusal(c), RefreshRefusal::kThumbnailsOff);
  c.iconsOnly = false;
  EXPECT_EQ(DecideRefreshRefusal(c), RefreshRefusal::kNone);

  RefreshConditions unchecked = Clean();
  unchecked.writesChecked = false;
  EXPECT_EQ(DecideRefreshRefusal(unchecked), RefreshRefusal::kVirtualized);
  RefreshConditions policy = Clean();
  policy.thumbnailsOffByPolicy = true;
  EXPECT_EQ(DecideRefreshRefusal(policy), RefreshRefusal::kThumbnailsOff);
}

// ---- ClassifyCandidate ----

CandidateSkip Local(std::wstring_view file, DWORD attributes = FILE_ATTRIBUTE_ARCHIVE,
                    ULONGLONG size = 1000) {
  return ClassifyCandidate(file, attributes, size, DRIVE_FIXED);
}

TEST(RefreshCandidate, ALocalPdfIsRefreshed) {
  EXPECT_EQ(Local(L"C:\\Data\\Documents\\x.pdf"), CandidateSkip::kNone);
  EXPECT_EQ(Local(L"C:\\Data\\Documents\\X.PDF"), CandidateSkip::kNone);
  EXPECT_EQ(Local(L"D:\\a.b.pdf", FILE_ATTRIBUTE_READONLY), CandidateSkip::kNone);
  EXPECT_EQ(ClassifyCandidate(L"E:\\x.pdf", FILE_ATTRIBUTE_NORMAL, 1, DRIVE_REMOVABLE),
            CandidateSkip::kNone);
}

TEST(RefreshCandidate, OnlineOnlyAndOfflineFiles) {
  EXPECT_EQ(Local(L"C:\\x.pdf", 0x400000), CandidateSkip::kPlaceholder);  // recall on data access
  EXPECT_EQ(Local(L"C:\\x.pdf", 0x40000), CandidateSkip::kPlaceholder);   // recall on open
  EXPECT_EQ(Local(L"C:\\x.pdf", 0x1000), CandidateSkip::kOffline);
  // The first reason wins.
  EXPECT_EQ(Local(L"C:\\x.pdf", 0x400000 | 0x1000), CandidateSkip::kPlaceholder);
}

TEST(RefreshCandidate, NetworkFiles) {
  EXPECT_EQ(Local(L"\\\\server\\share\\x.pdf"), CandidateSkip::kNetwork);
  EXPECT_EQ(Local(L"//server/share/x.pdf"), CandidateSkip::kNetwork);
  EXPECT_EQ(ClassifyCandidate(L"Z:\\x.pdf", 0, 1, DRIVE_REMOTE), CandidateSkip::kNetwork);
  EXPECT_EQ(ClassifyCandidate(L"Q:\\x.pdf", 0, 1, DRIVE_NO_ROOT_DIR), CandidateSkip::kNetwork);
  EXPECT_EQ(ClassifyCandidate(L"Q:\\x.pdf", 0, 1, DRIVE_UNKNOWN), CandidateSkip::kNetwork);
}

TEST(RefreshCandidate, TheRecycleBinInAnyCaseAndPosition) {
  EXPECT_EQ(Local(L"C:\\$Recycle.Bin\\S-1-5-21-1\\$R1.pdf"), CandidateSkip::kRecycleBin);
  EXPECT_EQ(Local(L"C:\\$RECYCLE.BIN\\x.pdf"), CandidateSkip::kRecycleBin);
  EXPECT_EQ(Local(L"D:\\a\\$recycle.bin\\b\\x.pdf"), CandidateSkip::kRecycleBin);
  EXPECT_EQ(Local(L"D:/a/$Recycle.Bin/x.pdf"), CandidateSkip::kRecycleBin);
  // Only a whole component.
  EXPECT_EQ(Local(L"C:\\my$Recycle.Bin\\x.pdf"), CandidateSkip::kNone);
  EXPECT_EQ(Local(L"C:\\$Recycle.Bins\\x.pdf"), CandidateSkip::kNone);
}

TEST(RefreshCandidate, TheHandlersSizeLimit) {
  EXPECT_EQ(Local(L"C:\\x.pdf", 0, 268435456), CandidateSkip::kNone);  // exactly 256 MiB
  EXPECT_EQ(Local(L"C:\\x.pdf", 0, 268435457), CandidateSkip::kTooLarge);
}

TEST(RefreshCandidate, PathsTooLongForFileExplorer) {
  // "C:\" + name + ".pdf"
  const std::wstring shortest = L"C:\\" + std::wstring(MAX_PATH - 1 - 3 - 4, L'a') + L".pdf";
  ASSERT_EQ(shortest.size(), static_cast<size_t>(MAX_PATH - 1));
  EXPECT_EQ(Local(shortest), CandidateSkip::kNone);
  const std::wstring longer = L"C:\\" + std::wstring(MAX_PATH - 3 - 4, L'a') + L".pdf";
  ASSERT_EQ(longer.size(), static_cast<size_t>(MAX_PATH));
  EXPECT_EQ(Local(longer), CandidateSkip::kLongPath);
}

TEST(RefreshCandidate, OnlyTheRealNameCounts) {
  EXPECT_EQ(Local(L"C:\\x.pdfx"), CandidateSkip::kNotPdf);
  EXPECT_EQ(Local(L"C:\\x.pdf.txt"), CandidateSkip::kNotPdf);
  EXPECT_EQ(Local(L"C:\\x.pdf:stream"), CandidateSkip::kNotPdf);
  EXPECT_EQ(Local(L"C:\\pdf"), CandidateSkip::kNotPdf);
  EXPECT_EQ(Local(L"C:\\a.pdf\\x"), CandidateSkip::kNotPdf);
}

// ---- GroupByFolder ----

TEST(RefreshGroup, FoldersInTheOrderTheyFirstAppear) {
  const std::vector<path> files = {L"C:\\b\\1.pdf",      L"C:\\a\\1.pdf", L"C:\\B\\2.pdf",
                                   L"C:\\b\\sub\\1.pdf", L"C:\\a\\2.pdf", L"C:/b/3.pdf"};
  const std::vector<path> grouped = GroupByFolder(files);
  const std::vector<path> expected = {L"C:\\b\\1.pdf", L"C:\\B\\2.pdf", L"C:/b/3.pdf",
                                      L"C:\\a\\1.pdf", L"C:\\a\\2.pdf", L"C:\\b\\sub\\1.pdf"};
  EXPECT_EQ(grouped, expected);
  EXPECT_TRUE(GroupByFolder({}).empty());
}

// ---- CacheSizeOf and ClassifyCacheAnswer ----

TEST(RefreshCacheAnswer, TheFileASizeLandsIn) {
  EXPECT_EQ(kRefreshProbeFlags, static_cast<DWORD>(WTS_INCACHEONLY));
  for (const UINT size : kRefreshSizes) EXPECT_EQ(CacheSizeOf(size), size);
  EXPECT_EQ(CacheSizeOf(1), 16u);
  EXPECT_EQ(CacheSizeOf(17), 32u);
  EXPECT_EQ(CacheSizeOf(100), 256u);
  EXPECT_EQ(CacheSizeOf(257), 768u);
  EXPECT_EQ(CacheSizeOf(1024), 1280u);  // as on build 26100
  EXPECT_EQ(CacheSizeOf(2561), 2561u);  // above the largest: itself
}

CacheAnswer Cached(LONG w, LONG h, DWORD flags = WTS_CACHED) {
  CacheAnswer answer;
  answer.bitmap = true;
  answer.cacheFlags = flags;
  answer.dims = {w, h};
  return answer;
}

TEST(RefreshCacheAnswer, TheLongerEdgeNamesTheFile) {
  struct Row {
    UINT size;
    LONG w, h;
    CacheHit hit;
  };
  const Row rows[] = {
      // 256's file holds edges above 96 up to 256.
      {256, 68, 96, CacheHit::kSmaller},
      {256, 69, 97, CacheHit::kHeld},
      {256, 181, 256, CacheHit::kHeld},
      {256, 256, 181, CacheHit::kHeld},  // landscape: the longer side
      {256, 182, 257, CacheHit::kLarger},
      // A size between two is its file's: 100 is 256's.
      {100, 68, 96, CacheHit::kSmaller},
      {100, 71, 100, CacheHit::kHeld},
      {100, 181, 256, CacheHit::kHeld},
      {100, 362, 512, CacheHit::kLarger},
      // The smallest file holds every edge up to 16.
      {16, 1, 1, CacheHit::kHeld},
      {16, 11, 16, CacheHit::kHeld},
      {16, 12, 17, CacheHit::kLarger},
      {16, 0, 0, CacheHit::kSmaller},  // a bitmap of no size is no file's
      // 1024 is 1280's: from 768 up.
      {1024, 543, 768, CacheHit::kSmaller},
      {1024, 544, 769, CacheHit::kHeld},
      {1024, 905, 1280, CacheHit::kHeld},
      {1024, 905, 1281, CacheHit::kLarger},
      {2560, 1810, 2560, CacheHit::kHeld},
      {2560, 1357, 1920, CacheHit::kSmaller},
      // Above the largest, the size itself is the file.
      {3000, 1810, 2560, CacheHit::kSmaller},
      {3000, 1811, 2561, CacheHit::kHeld},
      {3000, 2122, 3001, CacheHit::kLarger},
  };
  for (const Row& row : rows) {
    EXPECT_EQ(ClassifyCacheAnswer(row.size, S_OK, Cached(row.w, row.h)), row.hit)
        << row.size << ": " << row.w << "x" << row.h;
  }
  // Smaller than asked (WTS_LOWQUALITY), but in the size's own file: held.
  EXPECT_EQ(ClassifyCacheAnswer(256, S_OK, Cached(102, 144, WTS_CACHED | WTS_LOWQUALITY)),
            CacheHit::kHeld);
}

TEST(RefreshCacheAnswer, RefusalsAndAnswersNotFromTheCache) {
  EXPECT_EQ(ClassifyCacheAnswer(256, E_INVALIDARG, CacheAnswer{}), CacheHit::kRefused);
  EXPECT_EQ(ClassifyCacheAnswer(256, E_INVALIDARG, Cached(181, 256)), CacheHit::kRefused);
  EXPECT_EQ(ClassifyCacheAnswer(256, WTS_E_FAILEDEXTRACTION, CacheAnswer{}), CacheHit::kNotCached);
  EXPECT_EQ(ClassifyCacheAnswer(256, E_FAIL, Cached(181, 256)), CacheHit::kNotCached);
  EXPECT_EQ(ClassifyCacheAnswer(256, S_FALSE, Cached(181, 256)), CacheHit::kNotCached);
  // S_OK without a bitmap, and a bitmap without WTS_CACHED.
  CacheAnswer none;
  none.cacheFlags = WTS_CACHED;
  EXPECT_EQ(ClassifyCacheAnswer(256, S_OK, none), CacheHit::kNotCached);
  EXPECT_EQ(ClassifyCacheAnswer(256, S_OK, Cached(181, 256, 0)), CacheHit::kNotCached);
  EXPECT_EQ(ClassifyCacheAnswer(256, S_OK, Cached(181, 256, WTS_LOWQUALITY)), CacheHit::kNotCached);
}

// ---- RefreshFiles against a fake cache ----

// An A4 portrait page's bitmap at |edge| pixels, as the handler makes it at 100 %.
SIZE Portrait(UINT edge) {
  return {std::max<LONG>(1, static_cast<LONG>(edge * 595 / 842)), static_cast<LONG>(edge)};
}

// A thumbnail cache that holds what the test says, on a fake clock, and records every call. It
// keeps its entries as the real one keeps them in its files, one file per size of kRefreshSizes
// (CacheSizeOf): a WTS_INCACHEONLY question is answered from the size's own file first; else
// from the smallest larger file, as stored (a case the code must handle, which CI never showed:
// there a file stored at 256 answered 16 and 96 with bitmaps of those sizes, and 1024 is
// answered from 1280's file because that is its own); else from the largest smaller file,
// marked WTS_LOWQUALITY; else not at all. Bitmaps are A4 portrait pages at the size they were
// stored at. A successful extraction that stores replaces the file's record, as build 26100's
// cache does: afterwards it holds that size and every smaller size of kRefreshSizes (only that
// size without storeFillsSmaller), each of them made in this run (made). It may also drop whole
// files (dropsWhenStored), as the real cache drops a file's every size while others are stored.
class FakeCache final : public ThumbnailCacheOps {
 public:
  struct Call {
    std::wstring file;
    UINT size = 0;  // 0 for Select
    DWORD flags = 0;
    bool operator==(const Call&) const = default;
  };

  HRESULT Select(const path& file) noexcept override {
    now += selectMs;
    selected = file.native();
    calls.push_back({selected, 0, 0});
    if (gone.count(selected)) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    if (const auto failure = selectFails.find(selected); failure != selectFails.end()) {
      return failure->second;
    }
    return S_OK;
  }

  HRESULT Get(UINT size, DWORD flags, CacheAnswer* answer) noexcept override {
    *answer = {};
    now += getMs;
    const Call call{selected, size, flags};
    calls.push_back(call);
    if (onGet) onGet(call);
    if (refusedSizes.count(size) || refusedFlags.count(flags)) return E_INVALIDARG;
    if (flags & WTS_INCACHEONLY) {
      const auto file = cached.find(selected);
      if (file == cached.end()) return WTS_E_FAILEDEXTRACTION;
      const std::set<UINT>& held = file->second;  // smallest first
      const UINT own = CacheSizeOf(size);
      UINT stored = 0;
      DWORD quality = 0;
      for (const UINT entry : held) {
        if (CacheSizeOf(entry) == own) stored = entry;
      }
      if (stored == 0) {
        const auto larger = std::find_if(held.begin(), held.end(),
                                         [own](UINT entry) { return CacheSizeOf(entry) > own; });
        if (larger != held.end()) stored = *larger;
      }
      if (stored == 0) {
        for (const UINT entry : held) {
          if (CacheSizeOf(entry) < own) stored = entry;
        }
        quality = WTS_LOWQUALITY;
      }
      if (stored == 0) return WTS_E_FAILEDEXTRACTION;
      answer->bitmap = true;
      answer->cacheFlags = WTS_CACHED | quality;
      answer->dims = Portrait(stored);
      return S_OK;
    }
    if (const auto failure = getFails.find({selected, size}); failure != getFails.end()) {
      return failure->second;
    }
    answer->bitmap = true;
    answer->dims = Portrait(size);
    if (!(flags & WTS_EXTRACTDONOTCACHE)) {
      if (const auto drops = dropsWhenStored.find(selected); drops != dropsWhenStored.end()) {
        for (const std::wstring& file : drops->second) cached.erase(file);
      }
      std::set<UINT> record{size};
      if (storeFillsSmaller) {
        for (const UINT smaller : kRefreshSizes) {
          if (smaller < size) record.insert(smaller);
        }
      }
      cached[selected] = record;
      made[selected] = record;
    }
    return S_OK;
  }

  // The calls other than Select with |flags|.
  std::vector<std::pair<std::wstring, UINT>> GetsWith(DWORD flags) const {
    std::vector<std::pair<std::wstring, UINT>> found;
    for (const Call& call : calls) {
      if (call.size != 0 && call.flags == flags) found.emplace_back(call.file, call.size);
    }
    return found;
  }

  std::map<std::wstring, std::set<UINT>> cached;  // file -> the sizes it was stored at
  // file -> the sizes its last store in this run left it with (the new zoom's)
  std::map<std::wstring, std::set<UINT>> made;
  bool storeFillsSmaller = true;
  std::set<std::wstring> gone;
  std::map<std::wstring, HRESULT> selectFails;
  std::set<UINT> refusedSizes;   // E_INVALIDARG at these sizes, whatever the flags
  std::set<DWORD> refusedFlags;  // E_INVALIDARG for these flags, as build 26100 answers 0x8001
  std::map<std::pair<std::wstring, UINT>, HRESULT> getFails;  // extractions that fail
  // A successful extraction of the key without WTS_EXTRACTDONOTCACHE drops every entry of these
  // files.
  std::map<std::wstring, std::set<std::wstring>> dropsWhenStored;
  std::function<void(const Call&)> onGet;
  ULONGLONG now = 0;
  ULONGLONG selectMs = 0;
  ULONGLONG getMs = 0;
  std::wstring selected;
  std::vector<Call> calls;
};

// Records what File Explorer is told, in order.
class FakeNotifier final : public ShellNotifier {
 public:
  void ItemUpdated(const path& file) noexcept override {
    events.push_back(L"item " + file.native());
  }
  void FolderUpdated(const path& folder) noexcept override {
    events.push_back(L"folder " + folder.native());
  }
  void RefreshViews(std::span<const path> folders) noexcept override {
    std::wstring event = L"views";
    for (const path& folder : folders) event += L" " + folder.native();
    events.push_back(event);
  }
  void AssociationsChanged() noexcept override { events.push_back(L"assoc"); }

  std::vector<std::wstring> events;
};

RefreshFilesOptions Options(FakeCache* cache, std::vector<UINT> sizes = {16, 96, 256}) {
  RefreshFilesOptions options;
  options.sizes = std::move(sizes);
  options.clock = [cache]() { return cache->now; };
  return options;
}

HRESULT RunRefresh(std::span<const path> files, const RefreshFilesOptions& options,
                   FakeCache* cache, FakeNotifier* notifier, RefreshCounts* counts,
                   std::stop_token stop = std::stop_token(),
                   const RefreshProgressFn& progress = {}) {
  return RefreshFiles(files, options, *cache, *notifier, progress, stop, counts);
}

using Entries = std::vector<std::pair<std::wstring, UINT>>;

TEST(RefreshFiles, OnlyCachedSizesAreMadeAgain) {
  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = {96, 256};
  cache.cached[L"C:\\a\\3.pdf"] = {16};
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf", L"C:\\a\\3.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, &counts), S_OK);

  // Every size is asked about first; then each file's largest cached size is made again with the
  // product's flags, and a smaller cached one asked about again: 1.pdf's store at 256 made 96.
  EXPECT_EQ(cache.GetsWith(kRefreshProbeFlags).size(), 10u);
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags),
            (Entries{{L"C:\\a\\1.pdf", 256}, {L"C:\\a\\3.pdf", 16}}));
  EXPECT_EQ(cache.calls[1], (FakeCache::Call{L"C:\\a\\1.pdf", 16, kRefreshProbeFlags}));
  EXPECT_EQ(cache.calls[3], (FakeCache::Call{L"C:\\a\\1.pdf", 256, kRefreshProbeFlags}));
  EXPECT_EQ(cache.calls[4], (FakeCache::Call{L"C:\\a\\1.pdf", 256, kRefreshExtractFlags}));
  EXPECT_EQ(cache.calls[5], (FakeCache::Call{L"C:\\a\\1.pdf", 96, kRefreshProbeFlags}));
  EXPECT_EQ(counts.files, 3u);
  EXPECT_EQ(counts.notCached, 1u);
  EXPECT_EQ(counts.entriesCached, 3u);
  // 1.pdf at 16 (from 96's file), 3.pdf at 96 and 256 (from 16's): left alone.
  EXPECT_EQ(counts.otherSize, 3u);
  EXPECT_EQ(counts.refreshed, 3u);
  EXPECT_EQ(counts.filesRefreshed, 2u);
  EXPECT_EQ(counts.failed, 0u);
  EXPECT_EQ(counts.gone, 0u);
  EXPECT_FALSE(counts.stopped);
  EXPECT_TRUE(counts.sizesDropped.empty());
}

TEST(RefreshFiles, AFileHeldAtOneSizeIsMadeAgainAtThatSizeOnly) {
  // The other sizes' questions are answered from 256's file: by a larger size's thumbnail below
  // 256, by a smaller size's above it.
  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = {256};
  const std::vector<path> files = {L"C:\\a\\1.pdf"};
  RefreshFilesOptions options;  // every size of kRefreshSizes
  options.clock = [&cache]() { return cache.now; };
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, options, &cache, &notifier, &counts), S_OK);
  EXPECT_EQ(cache.GetsWith(kRefreshProbeFlags).size(), std::size(kRefreshSizes));
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags), (Entries{{L"C:\\a\\1.pdf", 256}}));
  EXPECT_EQ(counts.entriesCached, 1u);
  EXPECT_EQ(counts.otherSize, 8u);
  EXPECT_EQ(counts.refreshed, 1u);
  EXPECT_EQ(counts.notCached, 0u);
  EXPECT_TRUE(counts.sizesDropped.empty());
}

TEST(RefreshFiles, AFileHeldAtEverySizeEndsWithEverySizeMadeAgain) {
  const std::set<UINT> every(std::begin(kRefreshSizes), std::end(kRefreshSizes));
  // The fake is the cache of build 26100: a store at the smallest size leaves the file with that
  // size only, so a refresh that made the sizes smallest first would end with 16 alone.
  FakeCache smallest;
  smallest.cached[L"C:\\a\\1.pdf"] = every;
  CacheAnswer answer;
  ASSERT_EQ(smallest.Select(L"C:\\a\\1.pdf"), S_OK);
  ASSERT_EQ(smallest.Get(16, kRefreshExtractFlags, &answer), S_OK);
  EXPECT_EQ(smallest.cached[L"C:\\a\\1.pdf"], std::set<UINT>{16});

  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = every;
  const std::vector<path> files = {L"C:\\a\\1.pdf"};
  RefreshFilesOptions options;  // every size of kRefreshSizes
  options.clock = [&cache]() { return cache.now; };
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, options, &cache, &notifier, &counts), S_OK);
  // Made again once, at the largest size; every size is held afterwards, each from that store.
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags), (Entries{{L"C:\\a\\1.pdf", 2560}}));
  EXPECT_EQ(cache.cached[L"C:\\a\\1.pdf"], every);
  EXPECT_EQ(cache.made[L"C:\\a\\1.pdf"], every);
  // Asked at every size, then at each smaller one again.
  EXPECT_EQ(cache.GetsWith(kRefreshProbeFlags).size(), 2 * std::size(kRefreshSizes) - 1);
  EXPECT_EQ(counts.entriesCached, std::size(kRefreshSizes));
  EXPECT_EQ(counts.refreshed, std::size(kRefreshSizes));
  EXPECT_EQ(counts.otherSize, 0u);
  EXPECT_EQ(counts.failed, 0u);
  EXPECT_EQ(counts.filesRefreshed, 1u);
  EXPECT_EQ(notifier.events, (std::vector<std::wstring>{L"item C:\\a\\1.pdf"}));
}

TEST(RefreshFiles, ASmallerSizeTheLargerStoreDidNotKeepCountsAsFailed) {
  // A cache whose store kept only the size stored: 96 is not held any more, and is not made
  // again, which would take 256's new thumbnail away.
  FakeCache cache;
  cache.storeFillsSmaller = false;
  cache.cached[L"C:\\a\\1.pdf"] = {96, 256};
  const std::vector<path> files = {L"C:\\a\\1.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  const HRESULT hr = RunRefresh(files, Options(&cache), &cache, &notifier, &counts);
  ASSERT_EQ(hr, S_OK);
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags), (Entries{{L"C:\\a\\1.pdf", 256}}));
  EXPECT_EQ(cache.calls.back(), (FakeCache::Call{L"C:\\a\\1.pdf", 96, kRefreshProbeFlags}));
  EXPECT_EQ(cache.cached[L"C:\\a\\1.pdf"], std::set<UINT>{256});
  EXPECT_EQ(counts.entriesCached, 2u);
  EXPECT_EQ(counts.refreshed, 1u);
  EXPECT_EQ(counts.failed, 1u);
  EXPECT_EQ(counts.firstFailure, E_UNEXPECTED);
  EXPECT_EQ(RefreshExitCode(hr, counts), ExitCode::kProblems);
}

TEST(RefreshFiles, AFileTheCacheDropsIsNotMadeAgainAndFailsNothing) {
  // Storing the first file's thumbnails drops the third's: the third is asked about, misses, and
  // is left for File Explorer to make when it shows it.
  FakeCache cache;
  for (const wchar_t* file : {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf", L"C:\\a\\3.pdf"}) {
    cache.cached[file] = {16, 96, 256};
  }
  cache.dropsWhenStored[L"C:\\a\\1.pdf"] = {L"C:\\a\\3.pdf"};
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf", L"C:\\a\\3.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  const HRESULT hr = RunRefresh(files, Options(&cache), &cache, &notifier, &counts);
  ASSERT_EQ(hr, S_OK);
  // Five questions for each of the first two (three, then 96 and 16 again), three for the third.
  EXPECT_EQ(cache.GetsWith(kRefreshProbeFlags).size(), 13u);
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags),
            (Entries{{L"C:\\a\\1.pdf", 256}, {L"C:\\a\\2.pdf", 256}}));
  EXPECT_EQ(counts.notCached, 1u);
  EXPECT_EQ(counts.refreshed, 6u);
  EXPECT_EQ(counts.failed, 0u);
  EXPECT_EQ(RefreshExitCode(hr, counts), ExitCode::kOk);
  EXPECT_EQ(notifier.events,
            (std::vector<std::wstring>{L"item C:\\a\\1.pdf", L"item C:\\a\\2.pdf"}));
}

TEST(RefreshFiles, AFileDroppedAfterItsRefreshChangesNothing) {
  // Storing the second file's thumbnails drops the first's, made again just before: the refresh
  // reports what it made, as without the drop.
  const auto run = [](bool drop, RefreshCounts* counts, std::vector<std::wstring>* events) {
    FakeCache cache;
    cache.cached[L"C:\\a\\1.pdf"] = {16, 96, 256};
    cache.cached[L"C:\\a\\2.pdf"] = {16, 96, 256};
    if (drop) cache.dropsWhenStored[L"C:\\a\\2.pdf"] = {L"C:\\a\\1.pdf"};
    const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
    FakeNotifier notifier;
    EXPECT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, counts), S_OK);
    EXPECT_EQ(cache.cached.count(L"C:\\a\\1.pdf"), drop ? 0u : 1u);
    *events = notifier.events;
  };
  RefreshCounts dropped, kept;
  std::vector<std::wstring> droppedEvents, keptEvents;
  run(true, &dropped, &droppedEvents);
  run(false, &kept, &keptEvents);
  EXPECT_EQ(dropped.files, kept.files);
  EXPECT_EQ(dropped.notCached, kept.notCached);
  EXPECT_EQ(dropped.entriesCached, kept.entriesCached);
  EXPECT_EQ(dropped.refreshed, kept.refreshed);
  EXPECT_EQ(dropped.refreshed, 6u);
  EXPECT_EQ(dropped.filesRefreshed, kept.filesRefreshed);
  EXPECT_EQ(dropped.failed, 0u);
  EXPECT_EQ(dropped.otherSize, kept.otherSize);
  EXPECT_EQ(droppedEvents, keptEvents);
}

TEST(RefreshFiles, TheQuestionWithIdealCacheSizeOnlyRefusedEverywhereRefreshesNothing) {
  // Build 26100 refuses WTS_INCACHEONLY | WTS_IDEALCACHESIZEONLY at every size, for thumbnails
  // it holds: a refresh that asked so would drop every size after the first file.
  constexpr DWORD kIdeal = static_cast<DWORD>(WTS_INCACHEONLY | WTS_IDEALCACHESIZEONLY);
  FakeCache cache;
  cache.refusedFlags = {kIdeal};
  cache.cached[L"C:\\a\\1.pdf"] = {16, 256};
  cache.cached[L"C:\\a\\2.pdf"] = {256};
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  RefreshFilesOptions options;
  options.probeFlags = kIdeal;
  options.clock = [&cache]() { return cache.now; };
  FakeNotifier notifier;
  RefreshCounts counts;
  EXPECT_EQ(RunRefresh(files, options, &cache, &notifier, &counts),
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
  EXPECT_EQ(cache.GetsWith(kIdeal).size(), std::size(kRefreshSizes));  // the first file's only
  EXPECT_EQ(counts.sizesDropped,
            std::vector<UINT>(std::begin(kRefreshSizes), std::end(kRefreshSizes)));
  EXPECT_EQ(counts.files, 1u);
  EXPECT_EQ(counts.refreshed, 0u);
  EXPECT_EQ(counts.entriesCached, 0u);
  EXPECT_TRUE(notifier.events.empty());
  // The product's question finds both.
  FakeCache product;
  product.refusedFlags = {kIdeal};
  product.cached = cache.cached;
  options.probeFlags = kRefreshProbeFlags;
  options.clock = [&product]() { return product.now; };
  ASSERT_EQ(RunRefresh(files, options, &product, &notifier, &counts), S_OK);
  EXPECT_EQ(counts.refreshed, 3u);
  EXPECT_TRUE(counts.sizesDropped.empty());
}

TEST(RefreshFiles, AnAnswerNotFromTheCacheIsNotCached) {
  // A bitmap without WTS_CACHED, or S_FALSE, is not a cached thumbnail.
  class Odd final : public ThumbnailCacheOps {
   public:
    HRESULT Select(const path&) noexcept override { return S_OK; }
    HRESULT Get(UINT size, DWORD flags, CacheAnswer* answer) noexcept override {
      *answer = {};
      if (!(flags & WTS_INCACHEONLY)) {
        ++extracted;
        return S_OK;
      }
      answer->bitmap = true;
      if (size == 16) return S_OK;  // no WTS_CACHED
      answer->cacheFlags = WTS_CACHED;
      return S_FALSE;
    }
    int extracted = 0;
  } cache;
  FakeNotifier notifier;
  RefreshCounts counts;
  RefreshFilesOptions options;
  options.sizes = {16, 32};
  const std::vector<path> files = {L"C:\\a\\1.pdf"};
  ASSERT_EQ(RefreshFiles(files, options, cache, notifier, {}, {}, &counts), S_OK);
  EXPECT_EQ(cache.extracted, 0);
  EXPECT_EQ(counts.notCached, 1u);
  EXPECT_TRUE(notifier.events.empty());
}

TEST(RefreshFiles, ARefusedSizeIsDroppedForTheRestOfTheRun) {
  FakeCache cache;
  cache.refusedSizes = {1280};
  cache.cached[L"C:\\a\\1.pdf"] = {256};
  cache.cached[L"C:\\a\\2.pdf"] = {256};
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, Options(&cache, {256, 1280}), &cache, &notifier, &counts), S_OK);
  // 1280 asked once, for the first file only.
  EXPECT_EQ(cache.GetsWith(kRefreshProbeFlags),
            (Entries{{L"C:\\a\\1.pdf", 256}, {L"C:\\a\\1.pdf", 1280}, {L"C:\\a\\2.pdf", 256}}));
  EXPECT_EQ(counts.sizesDropped, std::vector<UINT>{1280});
  EXPECT_EQ(counts.refreshed, 2u);
  EXPECT_EQ(counts.failed, 0u);
}

TEST(RefreshFiles, EverySizeRefusedStopsAsking) {
  FakeCache cache;
  cache.refusedSizes = {16, 96, 256};
  cache.cached[L"C:\\a\\1.pdf"] = {256};
  cache.cached[L"C:\\a\\2.pdf"] = {256};
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  EXPECT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, &counts),
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
  // The first file's questions only: nothing is left to ask about the second.
  const std::vector<FakeCache::Call> expected = {
      {L"C:\\a\\1.pdf", 0, 0},
      {L"C:\\a\\1.pdf", 16, kRefreshProbeFlags},
      {L"C:\\a\\1.pdf", 96, kRefreshProbeFlags},
      {L"C:\\a\\1.pdf", 256, kRefreshProbeFlags},
  };
  EXPECT_EQ(cache.calls, expected);
  EXPECT_EQ(counts.files, 1u);
  EXPECT_EQ(counts.notCached, 0u);  // never answered
  EXPECT_EQ(counts.refreshed, 0u);
  EXPECT_EQ(counts.sizesDropped, (std::vector<UINT>{16, 96, 256}));
  EXPECT_TRUE(notifier.events.empty());
}

TEST(RefreshFiles, TheLastSizeRefusedStillTellsWhatWasRefreshed) {
  FakeCache cache;
  cache.refusedSizes = {96};
  cache.cached[L"C:\\a\\1.pdf"] = {256};
  cache.cached[L"C:\\a\\2.pdf"] = {256};
  cache.cached[L"C:\\a\\3.pdf"] = {256};
  cache.getFails[{L"C:\\a\\2.pdf", 256}] = E_INVALIDARG;
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf", L"C:\\a\\3.pdf"};
  RefreshFilesOptions options = Options(&cache, {96, 256});
  options.notify = kNotifyItems | kNotifyFolders;
  FakeNotifier notifier;
  RefreshCounts counts;
  EXPECT_EQ(RunRefresh(files, options, &cache, &notifier, &counts),
            HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED));
  EXPECT_EQ(cache.calls.back(), (FakeCache::Call{L"C:\\a\\2.pdf", 256, kRefreshExtractFlags}));
  EXPECT_EQ(counts.files, 2u);
  EXPECT_EQ(counts.refreshed, 1u);
  EXPECT_EQ(counts.failed, 1u);
  EXPECT_EQ(counts.sizesDropped, (std::vector<UINT>{96, 256}));
  EXPECT_EQ(notifier.events, (std::vector<std::wstring>{L"item C:\\a\\1.pdf", L"folder C:\\a"}));
}

TEST(RefreshFiles, AnExtractionRefusedWithInvalidArgDropsItsSize) {
  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = {96, 256};
  cache.cached[L"C:\\a\\2.pdf"] = {96, 256};
  cache.getFails[{L"C:\\a\\1.pdf", 256}] = E_INVALIDARG;
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, Options(&cache, {96, 256}), &cache, &notifier, &counts), S_OK);
  // 1.pdf's largest size refused: its next one is made again instead.
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags),
            (Entries{{L"C:\\a\\1.pdf", 256}, {L"C:\\a\\1.pdf", 96}, {L"C:\\a\\2.pdf", 96}}));
  EXPECT_EQ(counts.sizesDropped, std::vector<UINT>{256});
  EXPECT_EQ(counts.failed, 1u);
  EXPECT_EQ(counts.firstFailure, E_INVALIDARG);
  EXPECT_EQ(counts.refreshed, 2u);
}

TEST(RefreshFiles, AFileGoneIsNotAFailure) {
  FakeCache cache;
  cache.gone = {L"C:\\a\\1.pdf"};
  cache.cached[L"C:\\a\\2.pdf"] = {256};
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, &counts), S_OK);
  EXPECT_EQ(counts.gone, 1u);
  EXPECT_EQ(counts.failed, 0u);
  EXPECT_EQ(counts.notCached, 0u);
  EXPECT_EQ(counts.refreshed, 1u);
  EXPECT_EQ(counts.firstFailure, S_OK);
  EXPECT_EQ(cache.GetsWith(kRefreshProbeFlags).size(), 3u);  // the second file's only
}

TEST(RefreshFiles, FailuresAreCountedWithTheFirstHresult) {
  FakeCache cache;
  cache.selectFails[L"C:\\a\\1.pdf"] = E_ACCESSDENIED;
  cache.cached[L"C:\\a\\2.pdf"] = {16, 96};
  // Its largest size fails, so 16 is made again by an extraction of its own.
  cache.getFails[{L"C:\\a\\2.pdf", 96}] = WTS_E_EXTRACTIONTIMEDOUT;
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, &counts), S_OK);
  EXPECT_EQ(counts.failed, 2u);
  EXPECT_EQ(counts.firstFailure, E_ACCESSDENIED);
  EXPECT_EQ(counts.refreshed, 1u);
  EXPECT_EQ(counts.filesRefreshed, 1u);
  EXPECT_EQ(counts.entriesCached, 2u);
  EXPECT_TRUE(counts.sizesDropped.empty());  // only E_INVALIDARG drops a size
}

TEST(RefreshFiles, StoreFlagsMakeASecondCall) {
  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = {256};
  cache.cached[L"C:\\a\\2.pdf"] = {256};
  cache.getFails[{L"C:\\a\\2.pdf", 256}] = E_FAIL;  // the extraction fails: no store
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  RefreshFilesOptions options = Options(&cache, {256});
  options.extractFlags = WTS_FORCEEXTRACTION;
  options.storeFlags = WTS_EXTRACT;  // a plain extraction: 0
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, options, &cache, &notifier, &counts), S_OK);
  const std::vector<FakeCache::Call> expected = {
      {L"C:\\a\\1.pdf", 0, 0},
      {L"C:\\a\\1.pdf", 256, kRefreshProbeFlags},
      {L"C:\\a\\1.pdf", 256, WTS_FORCEEXTRACTION},
      {L"C:\\a\\1.pdf", 256, WTS_EXTRACT},
      {L"C:\\a\\2.pdf", 0, 0},
      {L"C:\\a\\2.pdf", 256, kRefreshProbeFlags},
      {L"C:\\a\\2.pdf", 256, WTS_FORCEEXTRACTION},
  };
  EXPECT_EQ(cache.calls, expected);
  EXPECT_EQ(counts.refreshed, 1u);
  EXPECT_EQ(counts.failed, 1u);

  // Without store flags, one call per cached size.
  FakeCache single;
  single.cached[L"C:\\a\\1.pdf"] = {256};
  options = Options(&single, {256});
  ASSERT_EQ(RunRefresh(std::span(files).first(1), options, &single, &notifier, &counts), S_OK);
  EXPECT_EQ(single.calls.size(), 3u);
  EXPECT_EQ(counts.refreshed, 1u);
}

TEST(RefreshFiles, UnsafeFlagsMakeNoCall) {
  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = {256};
  const std::vector<path> files = {L"C:\\a\\1.pdf"};
  FakeNotifier notifier;
  const DWORD unsafe[] = {kRefreshExtractFlags | WTS_EXTRACTINPROC,
                          kRefreshExtractFlags | WTS_EXTRACTDONOTCACHE,
                          static_cast<DWORD>(WTS_REQUIRESURROGATE)};
  for (const DWORD extract : unsafe) {
    RefreshFilesOptions options = Options(&cache);
    options.extractFlags = extract;
    RefreshCounts counts;
    EXPECT_EQ(RunRefresh(files, options, &cache, &notifier, &counts), E_INVALIDARG);
  }
  RefreshFilesOptions probe = Options(&cache);
  probe.probeFlags = WTS_IDEALCACHESIZEONLY;
  RefreshCounts counts;
  EXPECT_EQ(RunRefresh(files, probe, &cache, &notifier, &counts), E_INVALIDARG);
  RefreshFilesOptions store = Options(&cache);
  store.storeFlags = WTS_EXTRACTINPROC;
  EXPECT_EQ(RunRefresh(files, store, &cache, &notifier, &counts), E_INVALIDARG);
  EXPECT_EQ(RunRefresh(files, Options(&cache, {}), &cache, &notifier, &counts), E_INVALIDARG);
  EXPECT_TRUE(cache.calls.empty());
  EXPECT_TRUE(notifier.events.empty());
}

TEST(RefreshFiles, StopsBetweenSizes) {
  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = {16, 96, 256};
  cache.cached[L"C:\\a\\2.pdf"] = {16, 96, 256};
  std::stop_source source;
  // Stop while the largest size's extraction is under way: it finishes, nothing follows.
  cache.onGet = [&source](const FakeCache::Call& call) {
    if (call.size == 256 && call.flags == kRefreshExtractFlags) source.request_stop();
  };
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  EXPECT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, &counts, source.get_token()),
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags), (Entries{{L"C:\\a\\1.pdf", 256}}));
  EXPECT_EQ(cache.calls.back(), (FakeCache::Call{L"C:\\a\\1.pdf", 256, kRefreshExtractFlags}));
  EXPECT_TRUE(counts.stopped);
  EXPECT_EQ(counts.files, 1u);
  EXPECT_EQ(counts.refreshed, 1u);
  EXPECT_EQ(counts.notCached, 0u);
  // What was refreshed is still told.
  EXPECT_EQ(notifier.events, (std::vector<std::wstring>{L"item C:\\a\\1.pdf"}));

  // Stopped while the sizes are asked about: nothing is made.
  FakeCache asked;
  asked.cached[L"C:\\a\\1.pdf"] = {16, 96, 256};
  std::stop_source early;
  asked.onGet = [&early](const FakeCache::Call& call) {
    if (call.size == 96) early.request_stop();
  };
  FakeNotifier quiet;
  RefreshCounts none;
  EXPECT_EQ(RunRefresh(files, Options(&asked), &asked, &quiet, &none, early.get_token()),
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_TRUE(asked.GetsWith(kRefreshExtractFlags).empty());
  EXPECT_EQ(asked.calls.back(), (FakeCache::Call{L"C:\\a\\1.pdf", 96, kRefreshProbeFlags}));
  EXPECT_EQ(none.refreshed, 0u);
  EXPECT_EQ(none.notCached, 0u);
  EXPECT_TRUE(quiet.events.empty());
}

TEST(RefreshFiles, StopsBetweenFiles) {
  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = {256};
  cache.cached[L"C:\\a\\2.pdf"] = {256};
  std::stop_source source;
  cache.onGet = [&source](const FakeCache::Call& call) {
    if (call.flags == kRefreshExtractFlags) source.request_stop();
  };
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  RefreshFilesOptions options = Options(&cache, {256});
  options.notify = kNotifyItems | kNotifyFolders | kRefreshViews;
  FakeNotifier notifier;
  RefreshCounts counts;
  EXPECT_EQ(RunRefresh(files, options, &cache, &notifier, &counts, source.get_token()),
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_EQ(cache.calls.size(), 3u);  // the first file only
  EXPECT_EQ(counts.files, 1u);
  EXPECT_EQ(counts.refreshed, 1u);
  // The folder under way and the views are told about what changed.
  EXPECT_EQ(notifier.events,
            (std::vector<std::wstring>{L"item C:\\a\\1.pdf", L"folder C:\\a", L"views C:\\a"}));

  // Stopped before it began: nothing is asked.
  FakeCache untouched;
  RefreshCounts none;
  std::stop_source already;
  already.request_stop();
  EXPECT_EQ(
      RunRefresh(files, Options(&untouched), &untouched, &notifier, &none, already.get_token()),
      HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_TRUE(untouched.calls.empty());
  EXPECT_TRUE(none.stopped);
}

TEST(RefreshFiles, NotificationsFollowTheMask) {
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf", L"C:\\a\\3.pdf",
                                   L"C:\\b\\1.pdf", L"C:\\c\\1.pdf"};
  const auto run = [&files](unsigned notify) {
    FakeCache cache;
    cache.cached[L"C:\\a\\1.pdf"] = {256};
    cache.cached[L"C:\\a\\3.pdf"] = {256};
    cache.cached[L"C:\\c\\1.pdf"] = {256};  // C:\b has nothing to refresh
    RefreshFilesOptions options = Options(&cache, {256});
    options.notify = notify;
    FakeNotifier notifier;
    RefreshCounts counts;
    EXPECT_EQ(RunRefresh(files, options, &cache, &notifier, &counts), S_OK);
    return notifier.events;
  };
  using Events = std::vector<std::wstring>;
  EXPECT_EQ(run(kNotifyItems),
            (Events{L"item C:\\a\\1.pdf", L"item C:\\a\\3.pdf", L"item C:\\c\\1.pdf"}));
  EXPECT_EQ(run(kNotifyFolders), (Events{L"folder C:\\a", L"folder C:\\c"}));
  EXPECT_EQ(run(kRefreshViews), (Events{L"views C:\\a C:\\c"}));
  EXPECT_EQ(run(kNotifyAssociations), (Events{L"assoc"}));
  EXPECT_EQ(run(0), Events{});
  EXPECT_EQ(run(kNotifyItems | kNotifyFolders | kRefreshViews | kNotifyAssociations),
            (Events{L"item C:\\a\\1.pdf", L"item C:\\a\\3.pdf", L"folder C:\\a",
                    L"item C:\\c\\1.pdf", L"folder C:\\c", L"views C:\\a C:\\c", L"assoc"}));
  EXPECT_EQ(kRefreshNotify, static_cast<unsigned>(kNotifyItems));
}

TEST(RefreshFiles, NothingRefreshedTellsNothing) {
  FakeCache cache;
  const std::vector<path> files = {L"C:\\a\\1.pdf"};
  RefreshFilesOptions options = Options(&cache);
  options.notify = kNotifyItems | kNotifyFolders | kRefreshViews | kNotifyAssociations;
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, options, &cache, &notifier, &counts), S_OK);
  EXPECT_TRUE(notifier.events.empty());
  EXPECT_EQ(counts.notCached, 1u);
}

TEST(RefreshFiles, ProgressAtMostEvery250Ms) {
  FakeCache cache;
  cache.selectMs = 100;  // each file takes 100 ms on the fake clock
  std::vector<path> files;
  for (int i = 0; i < 20; ++i) {
    files.push_back(L"C:\\a\\" + std::to_wstring(i) + L".pdf");
    cache.cached[files.back().native()] = {256};
  }
  RefreshFilesOptions options = Options(&cache, {256});
  options.notify = kNotifyItems | kNotifyAssociations;
  struct Seen {
    RefreshProgress progress;
    ULONGLONG at = 0;
  };
  std::vector<Seen> seen;
  const RefreshProgressFn progress = [&](const RefreshProgress& p) {
    seen.push_back({p, cache.now});
  };
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, options, &cache, &notifier, &counts, {}, progress), S_OK);

  ASSERT_GE(seen.size(), 3u);
  EXPECT_EQ(seen.front().progress.phase, RefreshPhase::kRefreshing);
  EXPECT_EQ(seen.front().progress.files, 20u);
  EXPECT_EQ(seen.front().progress.done, 0u);
  EXPECT_EQ(seen.back().progress.phase, RefreshPhase::kNotifying);
  EXPECT_EQ(seen.back().progress.done, 20u);
  EXPECT_EQ(seen.back().progress.refreshed, 20u);
  size_t between = 0;
  for (size_t i = 1; i + 1 < seen.size(); ++i) {
    EXPECT_EQ(seen[i].progress.phase, RefreshPhase::kRefreshing);
    EXPECT_GE(seen[i].at - seen[i - 1].at, 250u) << i;
    EXPECT_GE(seen[i].progress.done, seen[i - 1].progress.done);
    ++between;
  }
  // 2 s of work: some reports in between, never one per file.
  EXPECT_GE(between, 4u);
  EXPECT_LE(between, 8u);
}

TEST(RefreshFiles, AThrowingProgressCallbackDoesNotStopIt) {
  FakeCache cache;
  cache.cached[L"C:\\a\\1.pdf"] = {256};
  const std::vector<path> files = {L"C:\\a\\1.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  const RefreshProgressFn progress = [](const RefreshProgress&) { throw std::bad_alloc(); };
  EXPECT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, &counts, {}, progress), S_OK);
  EXPECT_EQ(counts.refreshed, 1u);
}

TEST(RefreshFiles, TimesAndTheFindingCounts) {
  FakeCache cache;
  cache.getMs = 7;
  cache.cached[L"C:\\a\\1.pdf"] = {16, 256};
  const std::vector<path> files = {L"C:\\a\\1.pdf"};
  std::vector<DWORD> extractions;
  RefreshFilesOptions options = Options(&cache);
  options.extractionMs = &extractions;
  FakeNotifier notifier;
  RefreshCounts counts;
  counts.fromWalk = 5;
  counts.capped = true;
  counts.skipped[0] = 2;
  counts.findMs = 40;
  counts.refreshed = 99;  // RefreshFiles starts its own counts afresh
  counts.filesRefreshed = 99;
  ASSERT_EQ(RunRefresh(files, options, &cache, &notifier, &counts), S_OK);
  // One extraction (256's), whose store made 16 too.
  EXPECT_EQ(extractions, (std::vector<DWORD>{7}));
  EXPECT_EQ(counts.extractMs, 7u);
  EXPECT_EQ(counts.probeMs, 28u);  // three sizes asked about, and 16 again
  EXPECT_EQ(counts.refreshed, 2u);
  EXPECT_EQ(counts.filesRefreshed, 1u);
  EXPECT_EQ(counts.fromWalk, 5u);
  EXPECT_TRUE(counts.capped);
  EXPECT_EQ(counts.skipped[0], 2u);
  EXPECT_EQ(counts.findMs, 40u);
}

// ---- What --refresh --folder says ----

using Lines = std::vector<std::wstring>;

constexpr wchar_t kOpenWindowsCaveat[] =
    L"Folder windows that are open now may show the old thumbnails until you open the folder "
    L"again.";

TEST(RefreshFolderTexts, RefusalExitCodesAndLogWords) {
  EXPECT_EQ(RefreshRefusalExitCode(RefreshRefusal::kNone), ExitCode::kOk);
  EXPECT_EQ(RefreshRefusalExitCode(RefreshRefusal::kElevated), ExitCode::kWrongContext);
  EXPECT_EQ(RefreshRefusalExitCode(RefreshRefusal::kVirtualized), ExitCode::kWrongContext);
  EXPECT_EQ(RefreshRefusalExitCode(RefreshRefusal::kOtherShellUser), ExitCode::kWrongContext);
  EXPECT_EQ(RefreshRefusalExitCode(RefreshRefusal::kNotInstalled), ExitCode::kNotInstalled);
  EXPECT_EQ(RefreshRefusalExitCode(RefreshRefusal::kNotOurHandler), ExitCode::kProblems);
  // Icons only: there is nothing to refresh, which fails nothing.
  EXPECT_EQ(RefreshRefusalExitCode(RefreshRefusal::kThumbnailsOff), ExitCode::kOk);

  std::set<std::wstring> words;
  for (const RefreshRefusal refusal :
       {RefreshRefusal::kNone, RefreshRefusal::kElevated, RefreshRefusal::kVirtualized,
        RefreshRefusal::kOtherShellUser, RefreshRefusal::kNotInstalled,
        RefreshRefusal::kNotOurHandler, RefreshRefusal::kThumbnailsOff}) {
    words.insert(RefreshRefusalToken(refusal));
  }
  EXPECT_EQ(words.size(), 7u);
  EXPECT_STREQ(RefreshRefusalToken(RefreshRefusal::kNone), L"none");
  EXPECT_STREQ(RefreshRefusalToken(RefreshRefusal::kNotInstalled), L"not-installed");
  EXPECT_STREQ(RefreshRefusalToken(RefreshRefusal::kThumbnailsOff), L"thumbnails-off");
}

TEST(RefreshFolderTexts, Refusals) {
  ProcessContext context;
  context.userAccount = L"PC\\tester";
  EXPECT_EQ(RefreshRefusalText(RefreshRefusal::kElevated, context),
            L"Cannot run here: running as administrator. The thumbnail cache is the signed-in "
            L"user's own, and this program does not make thumbnails as administrator; run "
            L"--refresh from a normal (not administrator) prompt.");
  const std::wstring other = RefreshRefusalText(RefreshRefusal::kOtherShellUser, context);
  EXPECT_EQ(other.rfind(L"Cannot run here: this program runs as PC\\tester, but File Explorer", 0),
            0u)
      << WideToUtf8Lossy(other);
  EXPECT_EQ(RefreshRefusalText(RefreshRefusal::kNotInstalled, context),
            std::wstring(kProductName) + L" is not installed here; there is nothing to refresh.");
  // Another program's handler, or none at all (empty .pdf slots).
  EXPECT_EQ(RefreshRefusalText(RefreshRefusal::kNotOurHandler, context),
            L"Windows uses another program's handler for .pdf thumbnails, or none, so a refresh "
            L"would not make them with " +
                std::wstring(kProductName) + L". Run --fix --take-back to use " +
                std::wstring(kProductName) + L" again.");
  EXPECT_EQ(RefreshRefusalText(RefreshRefusal::kThumbnailsOff, context),
            L"File Explorer shows icons only, so there are no thumbnails to refresh.");
  EXPECT_TRUE(RefreshRefusalText(RefreshRefusal::kVirtualized, context).empty());
  EXPECT_TRUE(RefreshRefusalText(RefreshRefusal::kNone, context).empty());
}

// The folder is named as the user typed it.
TEST(RefreshFolderTexts, FolderProblems) {
  EXPECT_EQ(FolderProblemText(L"C:\\My PDFs", FolderProblem::kNotFound),
            L"--folder C:\\My PDFs: no such folder.");
  EXPECT_EQ(FolderProblemText(L"a|b", FolderProblem::kBadName),
            L"--folder a|b: not a usable path.");
  EXPECT_EQ(FolderProblemText(L"\\\\.\\C:", FolderProblem::kDevicePath),
            L"--folder \\\\.\\C:: not a usable path.");
  EXPECT_EQ(FolderProblemText(L"x.pdf", FolderProblem::kNotAFolder),
            L"--folder x.pdf: not a folder.");
  EXPECT_EQ(FolderProblemText(L"\\\\server\\share", FolderProblem::kNetwork),
            L"--folder \\\\server\\share: network folders are not refreshed.");
  EXPECT_TRUE(FolderProblemText(L"C:\\", FolderProblem::kNone).empty());
}

TEST(RefreshFolderTexts, WhatWasRefreshed) {
  RefreshCounts c;
  c.files = 12;
  c.notCached = 9;
  c.entriesCached = 5;
  c.refreshed = 5;
  c.filesRefreshed = 3;
  EXPECT_EQ(RefreshReport(S_OK, c, 20000, true),
            Lines{L"Refreshed 5 PDF thumbnails in 3 files (12 files checked)."});
  // Until the refresh experiment shows that open folder windows follow, the report says that
  // they may not.
  EXPECT_EQ(
      RefreshReport(S_OK, c, 20000, false),
      (Lines{L"Refreshed 5 PDF thumbnails in 3 files (12 files checked).", kOpenWindowsCaveat}));
  EXPECT_EQ(RefreshExitCode(S_OK, c), ExitCode::kOk);

  RefreshCounts one;
  one.files = 1;
  one.entriesCached = 1;
  one.refreshed = 1;
  one.filesRefreshed = 1;
  EXPECT_EQ(RefreshReport(S_OK, one, 20000, true),
            Lines{L"Refreshed 1 PDF thumbnail in 1 file (1 file checked)."});
}

TEST(RefreshFolderTexts, NothingToRefresh) {
  RefreshCounts none;
  EXPECT_EQ(RefreshReport(S_OK, none, 20000, false),
            Lines{L"No PDF files were found; there is nothing to refresh."});
  EXPECT_EQ(RefreshExitCode(S_OK, none), ExitCode::kOk);

  RefreshCounts uncached;
  uncached.files = 4;
  uncached.notCached = 3;
  uncached.gone = 1;
  EXPECT_EQ(RefreshReport(S_OK, uncached, 20000, false),
            Lines{L"None of these PDF files has a thumbnail File Explorer made; there is nothing "
                  L"to refresh."});
  EXPECT_EQ(RefreshExitCode(S_OK, uncached), ExitCode::kOk);

  // Every PDF file skipped: that line says why. Files that are no PDFs do not count.
  RefreshCounts skipped;
  skipped.skipped[static_cast<size_t>(CandidateSkip::kPlaceholder) - 1] = 2;
  skipped.skipped[static_cast<size_t>(CandidateSkip::kLongPath) - 1] = 1;
  skipped.skipped[static_cast<size_t>(CandidateSkip::kNotPdf) - 1] = 9;
  EXPECT_EQ(RefreshReport(S_OK, skipped, 20000, false),
            Lines{L"3 files were skipped: online-only or offline files, network files, files in "
                  L"the recycle bin, files over 256 MB and files at paths too long for File "
                  L"Explorer keep their thumbnails."});
  EXPECT_EQ(RefreshExitCode(S_OK, skipped), ExitCode::kOk);
}

TEST(RefreshFolderTexts, WhatWasNotDone) {
  RefreshCounts c;
  c.files = 3;
  c.entriesCached = 3;
  c.refreshed = 2;
  c.filesRefreshed = 2;
  c.failed = 1;
  c.firstFailure = WTS_E_EXTRACTIONTIMEDOUT;
  c.skipped[static_cast<size_t>(CandidateSkip::kTooLarge) - 1] = 1;
  c.capped = true;
  EXPECT_EQ(
      RefreshReport(S_OK, c, 20000, false),
      (Lines{L"Refreshed 2 PDF thumbnails in 2 files (3 files checked).",
             L"1 file was skipped: online-only or offline files, network files, files in "
             L"the recycle bin, files over 256 MB and files at paths too long for File "
             L"Explorer keep their thumbnails.",
             L"1 thumbnail could not be made again (" + HResultText(WTS_E_EXTRACTIONTIMEDOUT) +
                 L"); File Explorer keeps the old one.",
             L"Only the first 20000 PDF files found were checked; run --refresh --folder on "
             L"smaller folders for the rest.",
             kOpenWindowsCaveat}));
  EXPECT_EQ(RefreshExitCode(S_OK, c), ExitCode::kProblems);

  // Nothing could be made again: no word about open windows.
  RefreshCounts failed;
  failed.files = 2;
  failed.entriesCached = 2;
  failed.failed = 2;
  failed.firstFailure = E_ACCESSDENIED;
  EXPECT_EQ(RefreshReport(S_OK, failed, 20000, false),
            (Lines{L"Refreshed 0 PDF thumbnails in 0 files (2 files checked).",
                   L"2 thumbnails could not be made again (0x80070005); File Explorer keeps the "
                   L"old ones."}));
  EXPECT_EQ(RefreshExitCode(S_OK, failed), ExitCode::kProblems);

  // None of the files checked had a thumbnail, but the files past the limit may have some.
  RefreshCounts capped;
  capped.files = 5;
  capped.notCached = 5;
  capped.capped = true;
  EXPECT_EQ(RefreshExitCode(S_OK, capped), ExitCode::kProblems);
  EXPECT_EQ(RefreshReport(S_OK, capped, 5, true),
            (Lines{L"Refreshed 0 PDF thumbnails in 0 files (5 files checked).",
                   L"Only the first 5 PDF files found were checked; run --refresh --folder on "
                   L"smaller folders for the rest."}));
}

TEST(RefreshFolderTexts, StoppedRefusedAndFailed) {
  const HRESULT stopped = HRESULT_FROM_WIN32(ERROR_CANCELLED);
  RefreshCounts c;
  c.files = 4;
  c.entriesCached = 3;
  c.refreshed = 3;
  c.filesRefreshed = 2;
  c.stopped = true;
  EXPECT_EQ(RefreshReport(stopped, c, 20000, false),
            (Lines{L"Stopped: 3 thumbnails refreshed; the rest keep the old zoom. Run --refresh "
                   L"--folder again to finish.",
                   kOpenWindowsCaveat}));
  EXPECT_EQ(RefreshExitCode(stopped, c), ExitCode::kCancelled);
  // Stopped while finding the files.
  RefreshCounts finding;
  finding.stopped = true;
  EXPECT_EQ(RefreshReport(stopped, finding, 20000, false),
            Lines{L"Stopped: 0 thumbnails refreshed; the rest keep the old zoom. Run --refresh "
                  L"--folder again to finish."});
  EXPECT_EQ(RefreshExitCode(stopped, finding), ExitCode::kCancelled);
  // Stopped before the files a capped walk found were all checked: no word about the limit.
  RefreshCounts stoppedCapped = c;
  stoppedCapped.capped = true;
  EXPECT_EQ(RefreshReport(stopped, stoppedCapped, 20000, false),
            (Lines{L"Stopped: 3 thumbnails refreshed; the rest keep the old zoom. Run --refresh "
                   L"--folder again to finish.",
                   kOpenWindowsCaveat}));
  EXPECT_EQ(RefreshExitCode(stopped, stoppedCapped), ExitCode::kCancelled);

  // The cache refused every size; a limit the walk reached adds nothing to that.
  const HRESULT noSize = HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
  RefreshCounts refused;
  refused.files = 1;
  refused.sizesDropped = {16, 256};
  const Lines refusedLines{L"Refreshed 0 PDF thumbnails in 0 files (1 file checked).",
                           L"The thumbnail cache refused every thumbnail size, so the PDF files "
                           L"after these were not checked; they keep their thumbnails."};
  EXPECT_EQ(RefreshReport(noSize, refused, 20000, false), refusedLines);
  EXPECT_EQ(RefreshExitCode(noSize, refused), ExitCode::kProblems);
  refused.capped = true;
  EXPECT_EQ(RefreshReport(noSize, refused, 20000, false), refusedLines);
  EXPECT_EQ(RefreshExitCode(noSize, refused), ExitCode::kProblems);

  // It could not start.
  EXPECT_EQ(RefreshExitCode(E_OUTOFMEMORY, RefreshCounts{}), ExitCode::kFailed);
  EXPECT_EQ(RefreshExitCode(E_INVALIDARG, RefreshCounts{}), ExitCode::kFailed);
  EXPECT_EQ(RefreshFailureText(E_ACCESSDENIED),
            L"Cannot refresh the PDF thumbnails: 0x80070005. Nothing changed.");
}

TEST(RefreshFolderTexts, ProgressLines) {
  EXPECT_EQ(RefreshProgressText(RefreshProgress{RefreshPhase::kFinding}),
            L"Looking for PDF files...");
  EXPECT_EQ(RefreshProgressText(RefreshProgress{RefreshPhase::kRefreshing, 400, 120, 37, 1}),
            L"Checked 120 of 400 PDF files; 37 thumbnails refreshed so far...");
  EXPECT_EQ(RefreshProgressText(RefreshProgress{RefreshPhase::kRefreshing, 1, 0, 1, 0}),
            L"Checked 0 of 1 PDF file; 1 thumbnail refreshed so far...");
  // No files to refresh: nothing to say.
  EXPECT_TRUE(RefreshProgressText(RefreshProgress{RefreshPhase::kRefreshing}).empty());
  EXPECT_EQ(RefreshProgressText(RefreshProgress{RefreshPhase::kNotifying, 4, 4, 4, 0}),
            L"Telling File Explorer...");
}

}  // namespace
}  // namespace pdft
