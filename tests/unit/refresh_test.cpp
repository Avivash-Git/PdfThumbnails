// The PDF thumbnail refresh's pure parts and its engine (refresh.h): which flags are safe, the
// refusal order, which files are skipped, grouping by folder, and RefreshFiles against a fake
// thumbnail cache and a fake File Explorer on a fake clock. Nothing here asks the real thumbnail
// cache or tells the real File Explorer anything.

#include <windows.h>

#include "refresh.h"

#include <gtest/gtest.h>
#include <thumbcache.h>

#include <map>
#include <new>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

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

// ---- RefreshFiles against a fake cache ----

// A thumbnail cache that holds what the test says, on a fake clock, and records every call.
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
    if (refusedSizes.count(size)) return E_INVALIDARG;
    const bool held = cached.count(selected) && cached.at(selected).count(size);
    if (flags & WTS_INCACHEONLY) {
      if (!held) return WTS_E_FAILEDEXTRACTION;
      answer->bitmap = true;
      answer->cacheFlags = WTS_CACHED;
      answer->dims = {static_cast<LONG>(size), static_cast<LONG>(size)};
      return S_OK;
    }
    if (const auto failure = getFails.find({selected, size}); failure != getFails.end()) {
      return failure->second;
    }
    answer->bitmap = true;
    answer->dims = {static_cast<LONG>(size), static_cast<LONG>(size)};
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

  std::map<std::wstring, std::set<UINT>> cached;  // file -> the sizes the cache holds
  std::set<std::wstring> gone;
  std::map<std::wstring, HRESULT> selectFails;
  std::set<UINT> refusedSizes;  // E_INVALIDARG, as the cache answers sizes above 1024
  std::map<std::pair<std::wstring, UINT>, HRESULT> getFails;  // extractions that fail
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

  // Every size is asked about, the cached ones made again with the product's flags, in order.
  EXPECT_EQ(cache.GetsWith(kRefreshProbeFlags).size(), 9u);
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags),
            (Entries{{L"C:\\a\\1.pdf", 96}, {L"C:\\a\\1.pdf", 256}, {L"C:\\a\\3.pdf", 16}}));
  EXPECT_EQ(cache.calls[1], (FakeCache::Call{L"C:\\a\\1.pdf", 16, kRefreshProbeFlags}));
  EXPECT_EQ(cache.calls[3], (FakeCache::Call{L"C:\\a\\1.pdf", 96, kRefreshExtractFlags}));
  EXPECT_EQ(counts.files, 3u);
  EXPECT_EQ(counts.notCached, 1u);
  EXPECT_EQ(counts.entriesCached, 3u);
  EXPECT_EQ(counts.refreshed, 3u);
  EXPECT_EQ(counts.failed, 0u);
  EXPECT_EQ(counts.gone, 0u);
  EXPECT_FALSE(counts.stopped);
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
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags),
            (Entries{{L"C:\\a\\1.pdf", 96}, {L"C:\\a\\1.pdf", 256}, {L"C:\\a\\2.pdf", 96}}));
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
  cache.getFails[{L"C:\\a\\2.pdf", 16}] = WTS_E_EXTRACTIONTIMEDOUT;
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  ASSERT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, &counts), S_OK);
  EXPECT_EQ(counts.failed, 2u);
  EXPECT_EQ(counts.firstFailure, E_ACCESSDENIED);
  EXPECT_EQ(counts.refreshed, 1u);
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
  // Stop while the second size's extraction is under way: it finishes, nothing follows.
  cache.onGet = [&source](const FakeCache::Call& call) {
    if (call.size == 96 && call.flags == kRefreshExtractFlags) source.request_stop();
  };
  const std::vector<path> files = {L"C:\\a\\1.pdf", L"C:\\a\\2.pdf"};
  FakeNotifier notifier;
  RefreshCounts counts;
  EXPECT_EQ(RunRefresh(files, Options(&cache), &cache, &notifier, &counts, source.get_token()),
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_EQ(cache.GetsWith(kRefreshExtractFlags),
            (Entries{{L"C:\\a\\1.pdf", 16}, {L"C:\\a\\1.pdf", 96}}));
  EXPECT_EQ(cache.calls.back(), (FakeCache::Call{L"C:\\a\\1.pdf", 96, kRefreshExtractFlags}));
  EXPECT_TRUE(counts.stopped);
  EXPECT_EQ(counts.files, 1u);
  EXPECT_EQ(counts.refreshed, 2u);
  EXPECT_EQ(counts.notCached, 0u);
  // What was refreshed is still told.
  EXPECT_EQ(notifier.events, (std::vector<std::wstring>{L"item C:\\a\\1.pdf"}));
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
  ASSERT_EQ(RunRefresh(files, options, &cache, &notifier, &counts), S_OK);
  EXPECT_EQ(extractions, (std::vector<DWORD>{7, 7}));
  EXPECT_EQ(counts.extractMs, 14u);
  EXPECT_EQ(counts.probeMs, 21u);  // three sizes asked about
  EXPECT_EQ(counts.refreshed, 2u);
  EXPECT_EQ(counts.fromWalk, 5u);
  EXPECT_TRUE(counts.capped);
  EXPECT_EQ(counts.skipped[0], 2u);
  EXPECT_EQ(counts.findMs, 40u);
}

}  // namespace
}  // namespace pdft
