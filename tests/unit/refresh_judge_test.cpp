// The refresh experiment's verdict (refresh_judge.h): which zoom a bitmap's size shows, for A4
// portrait and landscape at every size the experiment asks for, exactly and (from 32 px on)
// within 1 px; the folder rules (a valid baseline, refreshed folders, control folders, entries
// created and IDs changed, each entry taken once); the rules for files the cache drops whole (a
// refresh's own extraction answers, the pages each folder must show, the control); the record
// files written and read back under %TEMP% (TempDir). Nothing here asks the thumbnail cache.

#include <windows.h>

#include "refresh_judge.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <vector>

#include "crop_math.h"
#include "temp_dir.h"

namespace pdft::test {
namespace {

// The sizes the experiment asks the cache for (the product's and 1024).
constexpr unsigned kSizes[] = {16, 32, 48, 96, 256, 768, 1024, 1280, 1920, 2560};

constexpr int kZoomPairs[][2] = {{100, 142}, {130, 142}};

struct Orientation {
  const wchar_t* key;
  double w, h;
};
constexpr Orientation kOrientations[] = {{L"P-product/portrait-001.pdf", 595, 842},
                                         {L"P-product/landscape-001.pdf", 842, 595}};

// ---- ClassifyCrop ----

TEST(RefreshJudgeCrop, TheOldAndTheNewZoomAtEverySize) {
  for (const Orientation& page : kOrientations) {
    for (const auto& zooms : kZoomPairs) {
      for (const unsigned size : kSizes) {
        SCOPED_TRACE(::testing::Message() << page.w << "x" << page.h << " pt at " << size << " px, "
                                          << zooms[0] << " -> " << zooms[1]);
        const CropGeometry old = ComputeCrop(page.w, page.h, size, zooms[0]);
        const CropGeometry now = ComputeCrop(page.w, page.h, size, zooms[1]);
        // Every size tells these zooms apart: the experiment depends on it.
        EXPECT_FALSE(old.bmpW == now.bmpW && old.bmpH == now.bmpH);
        EXPECT_EQ(ClassifyCrop(page.key, old.bmpW, old.bmpH, zooms[0], zooms[1]), Crop::kOld);
        EXPECT_EQ(ClassifyCrop(page.key, now.bmpW, now.bmpH, zooms[0], zooms[1]), Crop::kNew);
        // Neither, by far.
        EXPECT_EQ(ClassifyCrop(page.key, static_cast<int>(size / 2), static_cast<int>(size),
                               zooms[0], zooms[1]),
                  Crop::kOther);
      }
    }
  }
}

TEST(RefreshJudgeCrop, TheNumbersByHand) {
  const wchar_t* portrait = L"C-control/portrait-002.pdf";
  const wchar_t* landscape = L"C-control/landscape-002.pdf";
  EXPECT_EQ(ClassifyCrop(portrait, 11, 16, 100, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(portrait, 16, 16, 100, 142), Crop::kNew);
  EXPECT_EQ(ClassifyCrop(portrait, 181, 256, 100, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(portrait, 256, 256, 100, 142), Crop::kNew);
  EXPECT_EQ(ClassifyCrop(landscape, 256, 181, 100, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(landscape, 256, 256, 100, 142), Crop::kNew);
  EXPECT_EQ(ClassifyCrop(portrait, 15, 16, 130, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(portrait, 16, 16, 130, 142), Crop::kNew);
  EXPECT_EQ(ClassifyCrop(portrait, 235, 256, 130, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(landscape, 256, 235, 130, 142), Crop::kOld);
  // A landscape size for a portrait page is neither.
  EXPECT_EQ(ClassifyCrop(portrait, 256, 181, 100, 142), Crop::kOther);
}

TEST(RefreshJudgeCrop, WithinOnePixelOnlyFrom32) {
  const wchar_t* portrait = L"P-product/portrait-001.pdf";
  // 256: 181 x 256 before, 256 x 256 after.
  EXPECT_EQ(ClassifyCrop(portrait, 255, 256, 100, 142), Crop::kNew);
  EXPECT_EQ(ClassifyCrop(portrait, 256, 255, 100, 142), Crop::kNew);
  EXPECT_EQ(ClassifyCrop(portrait, 182, 256, 100, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(portrait, 180, 256, 100, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(portrait, 183, 256, 100, 142), Crop::kOther);
  EXPECT_EQ(ClassifyCrop(portrait, 200, 256, 100, 142), Crop::kOther);
  // 32: 23 x 32 before, 32 x 32 after.
  EXPECT_EQ(ClassifyCrop(portrait, 31, 32, 100, 142), Crop::kNew);
  EXPECT_EQ(ClassifyCrop(portrait, 24, 32, 100, 142), Crop::kOld);
  // Below 32 only exact sizes count: 16 is 11 x 16 before, 16 x 16 after.
  EXPECT_EQ(ClassifyCrop(portrait, 15, 16, 100, 142), Crop::kOther);
  EXPECT_EQ(ClassifyCrop(portrait, 12, 16, 100, 142), Crop::kOther);
  EXPECT_EQ(ClassifyCrop(portrait, 16, 15, 100, 142), Crop::kOther);
  // An exact size wins over one within 1 px of the other zoom: 141 gives 255 x 256 at 256.
  EXPECT_EQ(ClassifyCrop(portrait, 256, 256, 141, 142), Crop::kNew);
  EXPECT_EQ(ClassifyCrop(portrait, 255, 256, 141, 142), Crop::kOld);
}

TEST(RefreshJudgeCrop, ZoomsThatGiveTheSameSize) {
  const wchar_t* portrait = L"P-product/portrait-001.pdf";
  EXPECT_EQ(ClassifyCrop(portrait, 256, 256, 142, 150), Crop::kIndistinguishable);
  EXPECT_EQ(ClassifyCrop(portrait, 255, 256, 142, 150), Crop::kIndistinguishable);
  EXPECT_EQ(ClassifyCrop(portrait, 181, 256, 100, 100), Crop::kIndistinguishable);
  EXPECT_EQ(ClassifyCrop(portrait, 181, 256, 142, 150), Crop::kOther);
}

TEST(RefreshJudgeCrop, ThePageComesFromTheName) {
  EXPECT_EQ(ClassifyCrop(L"P/Portrait-7.PDF", 181, 256, 100, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(L"LANDSCAPE-7.pdf", 256, 181, 100, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(L"P/landscape-x/portrait-1.pdf", 181, 256, 100, 142), Crop::kOld);
  EXPECT_EQ(ClassifyCrop(L"portrait-x/x.pdf", 181, 256, 100, 142), Crop::kOther);
  EXPECT_EQ(ClassifyCrop(L"P/01-portrait.pdf", 181, 256, 100, 142), Crop::kOther);
  EXPECT_EQ(ClassifyCrop(L"", 181, 256, 100, 142), Crop::kOther);
  EXPECT_EQ(ClassifyCrop(L"P/portrait-1.pdf", 0, 0, 100, 142), Crop::kOther);
  EXPECT_EQ(ClassifyCrop(L"P/portrait-1.pdf", -181, 256, 100, 142), Crop::kOther);
}

// ---- Judge ----

// A record of |file| in |label| at |size|, cached at |zoom| (0: not cached).
CacheRecord Entry(const std::wstring& label, const std::wstring& file, UINT size, int zoom,
                  BYTE id = 1) {
  CacheRecord record;
  record.label = label;
  record.key = label + L"/" + file;
  record.size = size;
  if (zoom == 0) {
    record.hr = S_FALSE;
    return record;
  }
  const bool landscape = file.starts_with(L"landscape");
  const CropGeometry crop =
      landscape ? ComputeCrop(842, 595, size, zoom) : ComputeCrop(595, 842, size, zoom);
  record.cached = true;
  record.w = crop.bmpW;
  record.h = crop.bmpH;
  record.cacheFlags = 2;  // WTS_CACHED
  record.id[0] = id;
  return record;
}

// Two files at two sizes in |label|, cached at |zoom|.
std::vector<CacheRecord> Folder(const std::wstring& label, int zoom, BYTE id = 1) {
  std::vector<CacheRecord> records;
  for (const wchar_t* file : {L"portrait-001.pdf", L"landscape-001.pdf"}) {
    for (const UINT size : {16u, 256u}) records.push_back(Entry(label, file, size, zoom, id));
  }
  return records;
}

std::vector<CacheRecord> Join(std::vector<CacheRecord> a, const std::vector<CacheRecord>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

TEST(RefreshJudge, ARefreshedFolderPasses) {
  const std::vector<FolderVerdict> verdicts =
      Judge(Folder(L"P-product", 100), Folder(L"P-product", 142), 100, 142, {});
  ASSERT_EQ(verdicts.size(), 1u);
  const FolderVerdict& v = verdicts[0];
  EXPECT_EQ(v.label, L"P-product");
  EXPECT_FALSE(v.control);
  EXPECT_EQ(v.baseline, 4u);
  EXPECT_EQ(v.baselineNotOld, 0u);
  EXPECT_EQ(v.newAfter, 4u);
  EXPECT_EQ(v.oldAfter + v.missAfter + v.otherAfter + v.created + v.idChanged, 0u);
  EXPECT_TRUE(v.baselineValid);
  EXPECT_TRUE(v.passA);
}

TEST(RefreshJudge, ThumbnailsNotMadeAgainFail) {
  // Still cached and old: the forced extraction did not replace them.
  std::vector<FolderVerdict> verdicts =
      Judge(Folder(L"F-force", 100), Folder(L"F-force", 100), 100, 142, {});
  ASSERT_EQ(verdicts.size(), 1u);
  EXPECT_TRUE(verdicts[0].baselineValid);
  EXPECT_EQ(verdicts[0].oldAfter, 4u);
  EXPECT_FALSE(verdicts[0].passA);

  // One of them old.
  std::vector<CacheRecord> after = Folder(L"F-force", 142);
  after[1] = Entry(L"F-force", L"portrait-001.pdf", 256, 100);
  verdicts = Judge(Folder(L"F-force", 100), after, 100, 142, {});
  EXPECT_EQ(verdicts[0].newAfter, 3u);
  EXPECT_EQ(verdicts[0].oldAfter, 1u);
  EXPECT_FALSE(verdicts[0].passA);

  // A size that is neither zoom's.
  after = Folder(L"F-force", 142);
  after[1].w = 200;
  verdicts = Judge(Folder(L"F-force", 100), after, 100, 142, {});
  EXPECT_EQ(verdicts[0].otherAfter, 1u);
  EXPECT_FALSE(verdicts[0].passA);
}

TEST(RefreshJudge, ThumbnailsGoneFail) {
  // Not cached any more (removed without being stored again), or not asked about.
  std::vector<CacheRecord> after = Folder(L"R-slowreclaim", 142);
  after[0] = Entry(L"R-slowreclaim", L"portrait-001.pdf", 16, 0);
  after.pop_back();
  const std::vector<FolderVerdict> verdicts =
      Judge(Folder(L"R-slowreclaim", 100), after, 100, 142, {});
  ASSERT_EQ(verdicts.size(), 1u);
  EXPECT_EQ(verdicts[0].missAfter, 2u);
  EXPECT_EQ(verdicts[0].newAfter, 2u);
  EXPECT_FALSE(verdicts[0].passA);
}

TEST(RefreshJudge, AControlMustStayOld) {
  const std::vector<std::wstring> controls = {L"C-control"};
  std::vector<FolderVerdict> verdicts =
      Judge(Folder(L"C-control", 100), Folder(L"C-control", 100), 100, 142, controls);
  ASSERT_EQ(verdicts.size(), 1u);
  EXPECT_TRUE(verdicts[0].control);
  EXPECT_EQ(verdicts[0].oldAfter, 4u);
  EXPECT_TRUE(verdicts[0].passA);

  // File Explorer (or the refresh) made it again: the control did not hold.
  verdicts = Judge(Folder(L"C-control", 100), Folder(L"C-control", 142), 100, 142, controls);
  EXPECT_TRUE(verdicts[0].control);
  EXPECT_EQ(verdicts[0].newAfter, 4u);
  EXPECT_FALSE(verdicts[0].passA);

  // Gone is no control either.
  std::vector<CacheRecord> after = Folder(L"C-control", 100);
  after[2] = Entry(L"C-control", L"landscape-001.pdf", 16, 0);
  verdicts = Judge(Folder(L"C-control", 100), after, 100, 142, controls);
  EXPECT_EQ(verdicts[0].missAfter, 1u);
  EXPECT_FALSE(verdicts[0].passA);

  // The same label judged as a refreshed folder (after the product's own refresh).
  verdicts = Judge(Folder(L"C-control", 100), Folder(L"C-control", 142), 100, 142, {});
  EXPECT_FALSE(verdicts[0].control);
  EXPECT_TRUE(verdicts[0].passA);
}

TEST(RefreshJudge, TheBaselineMustBeAtTheOldZoom) {
  // One entry already new before the refresh: nothing can be told from this folder.
  std::vector<CacheRecord> before = Folder(L"S-instance", 100);
  before[3] = Entry(L"S-instance", L"landscape-001.pdf", 256, 142);
  std::vector<FolderVerdict> verdicts = Judge(before, Folder(L"S-instance", 142), 100, 142, {});
  ASSERT_EQ(verdicts.size(), 1u);
  EXPECT_EQ(verdicts[0].baseline, 4u);
  EXPECT_EQ(verdicts[0].baselineNotOld, 1u);
  EXPECT_FALSE(verdicts[0].baselineValid);
  EXPECT_FALSE(verdicts[0].passA);

  // A size that is neither zoom's.
  before = Folder(L"S-instance", 100);
  before[0].h = 13;
  verdicts = Judge(before, Folder(L"S-instance", 142), 100, 142, {});
  EXPECT_EQ(verdicts[0].baselineNotOld, 1u);
  EXPECT_FALSE(verdicts[0].passA);

  // Nothing cached before.
  verdicts = Judge(Folder(L"S-instance", 0), Folder(L"S-instance", 142), 100, 142, {});
  EXPECT_EQ(verdicts[0].baseline, 0u);
  EXPECT_FALSE(verdicts[0].baselineValid);
  EXPECT_FALSE(verdicts[0].passA);
  EXPECT_EQ(verdicts[0].created, 4u);

  // Only zooms that give the same sizes: nothing at the old zoom to tell apart.
  verdicts = Judge(Folder(L"S-instance", 142), Folder(L"S-instance", 150), 142, 150, {});
  EXPECT_EQ(verdicts[0].baselineNotOld, 0u);
  EXPECT_FALSE(verdicts[0].baselineValid);
  EXPECT_FALSE(verdicts[0].passA);
}

TEST(RefreshJudge, CreatedEntriesAndNewIdsAreReportedOnly) {
  std::vector<CacheRecord> before = Folder(L"E-force-then-store", 100);
  before.push_back(Entry(L"E-force-then-store", L"portrait-001.pdf", 48, 0));  // not cached
  std::vector<CacheRecord> after = Folder(L"E-force-then-store", 142, 7);      // new IDs
  after.push_back(Entry(L"E-force-then-store", L"portrait-001.pdf", 48, 142));
  after.push_back(Entry(L"E-force-then-store", L"landscape-001.pdf", 96, 142));  // never asked
  const std::vector<FolderVerdict> verdicts = Judge(before, after, 100, 142, {});
  ASSERT_EQ(verdicts.size(), 1u);
  EXPECT_EQ(verdicts[0].baseline, 4u);
  EXPECT_EQ(verdicts[0].newAfter, 4u);
  EXPECT_EQ(verdicts[0].created, 2u);
  EXPECT_EQ(verdicts[0].idChanged, 4u);
  EXPECT_TRUE(verdicts[0].passA);
}

TEST(RefreshJudge, FoldersInTheOrderTheyFirstAppear) {
  const std::vector<CacheRecord> before =
      Join(Folder(L"P-product", 100), Folder(L"C-control", 100));
  const std::vector<CacheRecord> after =
      Join(Join(Folder(L"X-extra", 142), Folder(L"C-control", 100)), Folder(L"P-product", 142));
  const std::vector<std::wstring> controls = {L"C-control"};
  const std::vector<FolderVerdict> verdicts = Judge(before, after, 100, 142, controls);
  ASSERT_EQ(verdicts.size(), 3u);
  EXPECT_EQ(verdicts[0].label, L"P-product");
  EXPECT_TRUE(verdicts[0].passA);
  EXPECT_EQ(verdicts[1].label, L"C-control");
  EXPECT_TRUE(verdicts[1].control);
  EXPECT_TRUE(verdicts[1].passA);
  // Only after: no baseline.
  EXPECT_EQ(verdicts[2].label, L"X-extra");
  EXPECT_EQ(verdicts[2].baseline, 0u);
  EXPECT_EQ(verdicts[2].created, 4u);
  EXPECT_FALSE(verdicts[2].passA);
}

TEST(RefreshJudge, EachRecordAfterGoesWithOneEntry) {
  // The same entry twice before, once after: the second has nothing after it.
  std::vector<CacheRecord> before = Folder(L"P-product", 100);
  before.push_back(before[0]);
  const std::vector<FolderVerdict> verdicts =
      Judge(before, Folder(L"P-product", 142), 100, 142, {});
  ASSERT_EQ(verdicts.size(), 1u);
  EXPECT_EQ(verdicts[0].baseline, 5u);
  EXPECT_EQ(verdicts[0].newAfter, 4u);
  EXPECT_EQ(verdicts[0].missAfter, 1u);
  EXPECT_EQ(verdicts[0].created, 0u);
  EXPECT_FALSE(verdicts[0].passA);
}

TEST(RefreshJudge, LabelsCompareExactly) {
  const std::vector<std::wstring> controls = {L"c-control"};
  const std::vector<FolderVerdict> verdicts =
      Judge(Folder(L"C-control", 100), Folder(L"C-control", 100), 100, 142, controls);
  ASSERT_EQ(verdicts.size(), 1u);
  EXPECT_FALSE(verdicts[0].control);
  EXPECT_FALSE(verdicts[0].passA);
}

TEST(RefreshJudge, TheOutcomeSaysWhatPassASays) {
  std::vector<FolderVerdict> verdicts =
      Judge(Folder(L"P-product", 100), Folder(L"P-product", 142), 100, 142, {});
  EXPECT_EQ(verdicts[0].outcome, FolderOutcome::kPass);
  EXPECT_FALSE(verdicts[0].ruled);
  verdicts = Judge(Folder(L"P-product", 100), Folder(L"P-product", 100), 100, 142, {});
  EXPECT_EQ(verdicts[0].outcome, FolderOutcome::kFail);
  verdicts = Judge(Folder(L"P-product", 0), Folder(L"P-product", 142), 100, 142, {});
  EXPECT_EQ(verdicts[0].outcome, FolderOutcome::kInvalid);
}

// ---- Judge with rules ----

constexpr UINT kRuleSizes[] = {16, 96, 256};
constexpr DWORD kCached = 2;  // WTS_CACHED
const std::vector<std::wstring> kFourFiles = {L"portrait-001.pdf", L"portrait-002.pdf",
                                              L"landscape-001.pdf", L"landscape-002.pdf"};

// The control's eight files, the pages in turn.
std::vector<std::wstring> EightFiles() {
  std::vector<std::wstring> files;
  for (const wchar_t* n : {L"001", L"002", L"003", L"004"}) {
    files.push_back(std::wstring(L"portrait-") + n + L".pdf");
    files.push_back(std::wstring(L"landscape-") + n + L".pdf");
  }
  return files;
}

// |files| of |label| at kRuleSizes, cached at |zoom| (0: not cached).
std::vector<CacheRecord> Records(const std::wstring& label, const std::vector<std::wstring>& files,
                                 int zoom) {
  std::vector<CacheRecord> records;
  for (const std::wstring& file : files) {
    for (const UINT size : kRuleSizes) records.push_back(Entry(label, file, size, zoom));
  }
  return records;
}

// An extraction's answer for |file| at |size|: S_OK with a bitmap at |zoom| and the out |flags|;
// a failure for zoom 0.
CacheRecord Made(const std::wstring& label, const std::wstring& file, UINT size, int zoom,
                 DWORD flags = kCached) {
  CacheRecord record = Entry(label, file, size, zoom == 0 ? 100 : zoom);
  record.cacheFlags = flags;
  record.cached = (flags & kCached) != 0;
  if (zoom == 0) {
    record.hr = static_cast<HRESULT>(0x8004B200);  // WTS_E_FAILEDEXTRACTION
    record.w = record.h = 0;
    record.cached = false;
  }
  return record;
}

// |file|'s records in |records| (at |sizes|; at every size when empty) no longer held: the cache
// dropped them.
void Drop(std::vector<CacheRecord>* records, const std::wstring& file,
          const std::vector<UINT>& sizes = {}) {
  for (CacheRecord& record : *records) {
    if (!record.key.ends_with(L"/" + file)) continue;
    if (!sizes.empty() && std::find(sizes.begin(), sizes.end(), record.size) == sizes.end()) {
      continue;
    }
    record = Entry(record.label, file, record.size, 0);
    record.hr = static_cast<HRESULT>(0x80030002);  // STG_E_FILENOTFOUND
  }
}

// The records of |records| other than |file|'s (at |size|; at every size for 0).
std::vector<CacheRecord> Without(std::vector<CacheRecord> records, const std::wstring& file,
                                 UINT size = 0) {
  std::erase_if(records, [&](const CacheRecord& record) {
    return record.key.ends_with(L"/" + file) && (size == 0 || record.size == size);
  });
  return records;
}

// A variant's folder refreshed as it should be: held at 100 % before, each entry made at 142 %
// and stored, and held at 142 % afterwards.
struct Refreshed {
  std::vector<CacheRecord> before, after, made;
  bool inProcess = true;  // |made| holds every extraction the refresh made
};

Refreshed Variant(const std::wstring& label = L"P-product") {
  Refreshed r{Records(label, kFourFiles, 100), Records(label, kFourFiles, 142), {}};
  for (const std::wstring& file : kFourFiles) {
    for (const UINT size : kRuleSizes) r.made.push_back(Made(label, file, size, 142));
  }
  return r;
}

FolderVerdict JudgeOne(const std::vector<CacheRecord>& before,
                       const std::vector<CacheRecord>& after, const JudgeRules& rules,
                       std::span<const std::wstring> controls = {}) {
  const std::vector<FolderVerdict> verdicts = Judge(before, after, 100, 142, controls, rules);
  EXPECT_EQ(verdicts.size(), 1u);
  return verdicts.empty() ? FolderVerdict{} : verdicts[0];
}

FolderVerdict JudgeRefreshed(const Refreshed& r, bool clean = true) {
  JudgeRules rules;
  rules.sizes = kRuleSizes;
  rules.refreshClean = clean;
  rules.made = r.made;
  rules.inProcess = r.inProcess;
  return JudgeOne(r.before, r.after, rules);
}

// A control's verdict with |filesPerPage|.
FolderVerdict JudgeControl(const std::vector<CacheRecord>& before,
                           const std::vector<CacheRecord>& after, size_t filesPerPage = 1) {
  static const std::wstring controls[] = {L"C-control"};
  JudgeRules rules;
  rules.sizes = kRuleSizes;
  rules.filesPerPage = filesPerPage;
  return JudgeOne(before, after, rules, controls);
}

TEST(RefreshJudgeRules, AFolderMadeAgainPasses) {
  const FolderVerdict v = JudgeRefreshed(Variant());
  EXPECT_TRUE(v.ruled);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_TRUE(v.passA);
  EXPECT_TRUE(v.baselineValid);
  EXPECT_EQ(v.baseline, 12u);
  EXPECT_EQ(v.newAfter, 12u);
  EXPECT_EQ(v.heldAfter, 12u);
  EXPECT_EQ(v.filesHeldAfter, 4u);
  EXPECT_EQ(v.heldNewCached, 12u);
  EXPECT_EQ(v.oldAfter + v.missAfter + v.otherAfter + v.madeWrong + v.droppedAfter, 0u);
  EXPECT_EQ(v.portraitBefore, 2u);
  EXPECT_EQ(v.landscapeBefore, 2u);
  EXPECT_EQ(v.portraitAfter, 2u);
  EXPECT_EQ(v.landscapeAfter, 2u);
  EXPECT_FALSE(v.refreshFailed);
  EXPECT_FALSE(v.nothingLeft);
}

TEST(RefreshJudgeRules, AFileDroppedWholeCountsNeitherWay) {
  Refreshed r = Variant();
  Drop(&r.after, L"portrait-001.pdf");
  const FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.droppedAfter, 3u);
  EXPECT_EQ(v.filesDropped, std::vector<std::wstring>{L"P-product/portrait-001.pdf"});
  EXPECT_EQ(v.filesGoneBefore, 0u);
  EXPECT_EQ(v.missAfter, 0u);
  EXPECT_EQ(v.newAfter, 9u);
  EXPECT_EQ(v.portraitAfter, 1u);
  EXPECT_EQ(v.landscapeAfter, 2u);
}

TEST(RefreshJudgeRules, AFilePartlyGoneFails) {
  Refreshed r = Variant();
  Drop(&r.after, L"portrait-001.pdf", {96, 256});
  const FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.missAfter, 2u);
  EXPECT_EQ(v.droppedAfter, 0u);
  EXPECT_TRUE(v.filesDropped.empty());
}

TEST(RefreshJudgeRules, ADroppedFileNeedsItsOwnExtractionMadeAndStored) {
  struct Row {
    const char* what;
    int zoom;
    DWORD flags;
    size_t madeWrong, missAfter;
  };
  const Row rows[] = {
      {"its extraction failed", 0, 0, 3, 0},
      {"made at the old zoom", 100, kCached, 3, 0},
      {"made without WTS_CACHED", 142, 0, 0, 3},
  };
  for (const Row& row : rows) {
    SCOPED_TRACE(row.what);
    Refreshed r = Variant();
    for (CacheRecord& made : r.made) {
      if (made.key == L"P-product/portrait-001.pdf") {
        made = Made(L"P-product", L"portrait-001.pdf", made.size, row.zoom, row.flags);
      }
    }
    Drop(&r.after, L"portrait-001.pdf");
    const FolderVerdict v = JudgeRefreshed(r);
    EXPECT_EQ(v.outcome, FolderOutcome::kFail);
    EXPECT_EQ(v.madeWrong, row.madeWrong);
    EXPECT_EQ(v.madeWrongGone, row.madeWrong);
    EXPECT_EQ(v.madeWrongOld, 0u);
    EXPECT_EQ(v.missAfter, row.missAfter);
    EXPECT_EQ(v.droppedAfter, 0u);
  }

  // Not reached at all: the refresh found it gone already.
  Refreshed r = Variant();
  r.made = Without(r.made, L"portrait-001.pdf");
  Drop(&r.after, L"portrait-001.pdf");
  const FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.droppedAfter, 3u);
  EXPECT_EQ(v.filesDropped.size(), 1u);
  EXPECT_EQ(v.filesGoneBefore, 1u);
  EXPECT_EQ(v.madeWrong, 0u);
}

TEST(RefreshJudgeRules, AnEntryHeldNeedsItsOwnExtractionAtTheNewZoom) {
  struct Row {
    const char* what;
    int zoom;  // -1: no record at that size
  };
  for (const Row& row : {Row{"its extraction failed", 0}, Row{"made at the old zoom", 100},
                         Row{"never made, in a file made at smaller sizes only", -1}}) {
    SCOPED_TRACE(row.what);
    // The largest size, which no larger size's store makes, when it is never made.
    const UINT size = row.zoom >= 0 ? 96 : 256;
    Refreshed r = Variant();
    r.made = Without(r.made, L"portrait-001.pdf", size);
    if (row.zoom >= 0) r.made.push_back(Made(L"P-product", L"portrait-001.pdf", size, row.zoom));
    const FolderVerdict v = JudgeRefreshed(r);
    EXPECT_EQ(v.outcome, FolderOutcome::kFail);
    EXPECT_EQ(v.madeWrong, 1u);
    EXPECT_EQ(v.newAfter, 11u);
  }

  // Held and new right after: WTS_CACHED on its extraction is recorded, not needed.
  Refreshed r = Variant();
  for (CacheRecord& made : r.made) {
    if (made.key == L"P-product/landscape-001.pdf") made.cacheFlags = 0;
  }
  const FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.newAfter, 12u);
  EXPECT_EQ(v.heldNewCached, 9u);
}

TEST(RefreshJudgeRules, AStoreAtALargerSizeMakesTheSmallerOnes) {
  // Each file made again at its largest size only: the cache holds the smaller sizes from that
  // store, at the new zoom.
  Refreshed r = Variant();
  r.made.clear();
  for (const std::wstring& file : kFourFiles) r.made.push_back(Made(L"P-product", file, 256, 142));
  FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.newAfter, 12u);
  EXPECT_EQ(v.heldNewCached, 12u);
  EXPECT_EQ(v.madeWrong, 0u);

  // A file whose largest size failed and whose next was made: that one's store makes 16.
  r.made = Without(r.made, L"portrait-001.pdf");
  r.made.push_back(Made(L"P-product", L"portrait-001.pdf", 256, 0));
  r.made.push_back(Made(L"P-product", L"portrait-001.pdf", 96, 142));
  v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.madeWrong, 1u);  // 256 only
  EXPECT_EQ(v.newAfter, 11u);

  // Its largest size failed and nothing else was made: no size of it was made again.
  r.made = Without(r.made, L"portrait-001.pdf", 96);
  v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.madeWrong, 3u);
  EXPECT_EQ(v.newAfter, 9u);
}

TEST(RefreshJudgeRules, AnEntryHeldInAFileTheRefreshDidNotReachFails) {
  // Something else made it, or the refresh never got there and it stayed: either way not the
  // refresh's doing.
  Refreshed r = Variant();
  r.made = Without(r.made, L"landscape-002.pdf");
  FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.madeWrong, 3u);
  EXPECT_EQ(v.madeWrongOld + v.madeWrongGone, 0u);
  EXPECT_EQ(v.newAfter, 9u);

  // It stayed old: counted as made wrong, and as old by what the cache holds.
  r.after = Without(r.after, L"landscape-002.pdf");
  for (const UINT size : kRuleSizes)
    r.after.push_back(Entry(L"P-product", L"landscape-002.pdf", size, 100));
  v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.madeWrong, 3u);
  EXPECT_EQ(v.madeWrongOld, 3u);
  EXPECT_EQ(v.oldAfter, 0u);
}

TEST(RefreshJudgeRules, ARefreshHereThatReachedNoFileIsNotDecided) {
  // The cache dropped every file before the refresh in this process asked about them: it made
  // nothing, and nothing of it is a failure.
  Refreshed r = Variant();
  r.made.clear();
  for (const std::wstring& file : kFourFiles) Drop(&r.after, file);
  FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kUndecided);
  EXPECT_FALSE(v.nothingLeft);
  EXPECT_EQ(v.droppedAfter, 12u);
  EXPECT_EQ(v.filesDropped.size(), 4u);
  EXPECT_EQ(v.filesGoneBefore, 4u);
  EXPECT_EQ(v.madeWrong + v.missAfter, 0u);

  // A file held afterwards although the refresh reached none: not its doing.
  r = Variant();
  r.made.clear();
  for (const std::wstring& file :
       {L"portrait-001.pdf", L"landscape-001.pdf", L"landscape-002.pdf"}) {
    Drop(&r.after, file);
  }
  v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.madeWrong, 3u);
  EXPECT_EQ(v.filesGoneBefore, 3u);
}

TEST(RefreshJudgeRules, TheLastExtractionCounts) {
  // An extraction, then a call that stores it: each entry by its second answer.
  Refreshed r = Variant();
  std::vector<CacheRecord> twice;
  for (const CacheRecord& made : r.made) {
    twice.push_back(made);
    twice.back().cacheFlags = 0;
    twice.push_back(made);
  }
  r.made = twice;
  FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.heldNewCached, 12u);

  // The second failed.
  r.made.push_back(Made(L"P-product", L"portrait-002.pdf", 16, 0));
  v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.madeWrong, 1u);
}

TEST(RefreshJudgeRules, ARefreshThatReportedAFailureFails) {
  const FolderVerdict v = JudgeRefreshed(Variant(), false);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_TRUE(v.refreshFailed);
  EXPECT_EQ(v.newAfter, 12u);
}

TEST(RefreshJudgeRules, NothingLeftAfterTheRefreshFails) {
  // Each extraction said it stored its thumbnail, and none is held: they were not stored.
  Refreshed r = Variant();
  for (const std::wstring& file : kFourFiles) Drop(&r.after, file);
  FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_TRUE(v.nothingLeft);
  EXPECT_EQ(v.droppedAfter, 12u);
  EXPECT_EQ(v.filesDropped.size(), 4u);
  EXPECT_EQ(v.missAfter, 0u);

  // The same in another process (no extraction answers), with no failure reported.
  r.made.clear();
  r.inProcess = false;
  v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_TRUE(v.nothingLeft);
}

TEST(RefreshJudgeRules, WithoutExtractionAnswersADropCountsOnlyAfterACleanRefresh) {
  Refreshed r = Variant();
  r.made.clear();
  r.inProcess = false;
  Drop(&r.after, L"portrait-001.pdf");
  FolderVerdict v = JudgeRefreshed(r, true);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.droppedAfter, 3u);
  EXPECT_EQ(v.missAfter, 0u);
  EXPECT_EQ(v.filesGoneBefore, 0u);

  v = JudgeRefreshed(r, false);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.missAfter, 3u);
  EXPECT_EQ(v.droppedAfter, 0u);
  EXPECT_TRUE(v.refreshFailed);
}

TEST(RefreshJudgeRules, TooManyFilesDroppedIsNotDecided) {
  Refreshed r = Variant();
  Drop(&r.after, L"portrait-001.pdf");
  Drop(&r.after, L"portrait-002.pdf");
  FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kUndecided);
  EXPECT_FALSE(v.passA);
  EXPECT_FALSE(v.nothingLeft);
  EXPECT_EQ(v.portraitAfter, 0u);
  EXPECT_EQ(v.landscapeAfter, 2u);

  // A control that lost every file changed nothing, and shows nothing.
  const std::vector<CacheRecord> before = Records(L"C-control", kFourFiles, 100);
  std::vector<CacheRecord> after = before;
  for (const std::wstring& file : kFourFiles) Drop(&after, file);
  v = JudgeControl(before, after);
  EXPECT_TRUE(v.control);
  EXPECT_EQ(v.outcome, FolderOutcome::kUndecided);
  EXPECT_EQ(v.droppedAfter, 12u);
  EXPECT_FALSE(v.nothingLeft);
}

TEST(RefreshJudgeRules, EnoughPagesOfEachKindAtEverySize) {
  const std::vector<std::wstring> files = EightFiles();
  for (const size_t need : {1u, 2u}) {
    for (size_t portraits = 0; portraits <= 4; ++portraits) {
      for (size_t landscapes = 0; landscapes <= 4; ++landscapes) {
        SCOPED_TRACE(::testing::Message() << need << " needed, " << portraits << " portrait and "
                                          << landscapes << " landscape files held");
        std::vector<CacheRecord> before = Records(L"C-control", files, 100);
        for (size_t n = portraits; n < 4; ++n) Drop(&before, files[2 * n]);
        for (size_t n = landscapes; n < 4; ++n) Drop(&before, files[2 * n + 1]);
        const FolderVerdict v = JudgeControl(before, before, need);
        EXPECT_EQ(v.portraitBefore, portraits);
        EXPECT_EQ(v.landscapeBefore, landscapes);
        const bool enough = portraits >= need && landscapes >= need;
        EXPECT_EQ(v.baselineValid, enough);
        EXPECT_EQ(v.outcome, enough ? FolderOutcome::kPass : FolderOutcome::kInvalid);
      }
    }
  }

  // One size short: the only portrait held is missing at 96.
  std::vector<CacheRecord> before = Records(L"C-control", kFourFiles, 100);
  Drop(&before, L"portrait-001.pdf");
  Drop(&before, L"portrait-002.pdf", {96});
  FolderVerdict v = JudgeControl(before, before);
  EXPECT_EQ(v.portraitBefore, 0u);
  EXPECT_EQ(v.notHeldBefore, 4u);
  EXPECT_EQ(v.outcome, FolderOutcome::kInvalid);

  // A page the judge does not know by its name counts for neither (and shows neither zoom).
  before = Records(L"C-control", {L"square-001.pdf", L"landscape-001.pdf"}, 100);
  v = JudgeControl(before, before);
  EXPECT_EQ(v.portraitBefore, 0u);
  EXPECT_EQ(v.landscapeBefore, 1u);
  EXPECT_EQ(v.outcome, FolderOutcome::kInvalid);
}

TEST(RefreshJudgeRules, TheControl) {
  const std::vector<CacheRecord> before = Records(L"C-control", kFourFiles, 100);

  // One file dropped, the rest old: it held.
  std::vector<CacheRecord> after = before;
  Drop(&after, L"landscape-001.pdf");
  FolderVerdict v = JudgeControl(before, after);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.droppedAfter, 3u);
  EXPECT_EQ(v.oldAfter, 9u);

  // One entry new: something else made it again.
  after = before;
  after[4] = Entry(L"C-control", L"portrait-002.pdf", 96, 142);
  v = JudgeControl(before, after);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.newAfter, 1u);

  // One file partly gone.
  after = before;
  Drop(&after, L"portrait-002.pdf", {256});
  v = JudgeControl(before, after);
  EXPECT_EQ(v.outcome, FolderOutcome::kFail);
  EXPECT_EQ(v.missAfter, 1u);

  // Eight files, two of each page needed: four dropped, two of each left.
  const std::vector<std::wstring> files = EightFiles();
  const std::vector<CacheRecord> eight = Records(L"C-control", files, 100);
  after = eight;
  for (size_t i = 4; i < 8; ++i) Drop(&after, files[i]);
  v = JudgeControl(eight, after, 2);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.portraitAfter, 2u);
  EXPECT_EQ(v.landscapeAfter, 2u);
  EXPECT_EQ(v.filesDropped.size(), 4u);

  // One portrait and three landscapes left: not decided afterwards, and no baseline before.
  after = eight;
  for (const size_t i : {2u, 4u, 6u, 7u}) Drop(&after, files[i]);
  v = JudgeControl(eight, after, 2);
  EXPECT_EQ(v.outcome, FolderOutcome::kUndecided);
  EXPECT_EQ(v.portraitAfter, 1u);
  EXPECT_EQ(v.landscapeAfter, 3u);
  v = JudgeControl(after, after, 2);
  EXPECT_EQ(v.outcome, FolderOutcome::kInvalid);
}

TEST(RefreshJudgeRules, EntriesNotHeldBeforeAreNotJudged) {
  Refreshed r = Variant();
  Drop(&r.before, L"landscape-002.pdf");
  // A size the rules do not judge is not counted at all.
  r.before.push_back(Entry(L"P-product", L"portrait-001.pdf", 32, 0));
  const FolderVerdict v = JudgeRefreshed(r);
  EXPECT_EQ(v.outcome, FolderOutcome::kPass);
  EXPECT_EQ(v.notHeldBefore, 3u);
  EXPECT_EQ(v.baseline, 9u);
  EXPECT_EQ(v.newAfter, 9u);
  EXPECT_EQ(v.created, 3u);
  EXPECT_EQ(v.landscapeBefore, 1u);
}

// ---- WriteRecords / ReadRecords ----

bool WriteBytes(const std::wstring& path, const std::string& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok =
      WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  CloseHandle(file);
  return ok && written == bytes.size();
}

void ExpectSame(const CacheRecord& got, const CacheRecord& want) {
  EXPECT_EQ(got.label, want.label);
  EXPECT_EQ(got.key, want.key);
  EXPECT_EQ(got.size, want.size);
  EXPECT_EQ(got.hr, want.hr);
  EXPECT_EQ(got.cached, want.cached);
  EXPECT_EQ(got.w, want.w);
  EXPECT_EQ(got.h, want.h);
  EXPECT_EQ(got.cacheFlags, want.cacheFlags);
  EXPECT_EQ(got.id, want.id);
}

TEST(RefreshJudgeRecords, WrittenAndReadBack) {
  TempDir dir(L"judge");
  ASSERT_TRUE(dir.ok());
  std::vector<CacheRecord> records = Folder(L"P-product", 100);
  CacheRecord miss = Entry(L"T-\u00e9t\u00e9 \u6f22", L"portrait-002.pdf", 2560, 0);
  miss.hr = static_cast<HRESULT>(0x8004B200);  // WTS_E_FAILEDEXTRACTION
  records.push_back(miss);
  CacheRecord full = Entry(L"P-product", L"landscape-002.pdf", 1920, 142);
  full.cacheFlags = 0xFFFFFFFF;
  for (size_t i = 0; i < full.id.size(); ++i) full.id[i] = static_cast<BYTE>(0xF0 + i);
  records.push_back(full);

  const std::wstring file = dir.File(L"records.tsv");
  ASSERT_TRUE(WriteRecords(file, records));
  std::vector<CacheRecord> read;
  ASSERT_TRUE(ReadRecords(file, &read));
  ASSERT_EQ(read.size(), records.size());
  for (size_t i = 0; i < records.size(); ++i) {
    SCOPED_TRACE(i);
    ExpectSame(read[i], records[i]);
  }

  // None at all.
  ASSERT_TRUE(WriteRecords(file, {}));
  read = records;
  ASSERT_TRUE(ReadRecords(file, &read));
  EXPECT_TRUE(read.empty());
}

TEST(RefreshJudgeRecords, NamesThatWouldBreakTheFormatAreRefused) {
  TempDir dir(L"judge");
  ASSERT_TRUE(dir.ok());
  // Each in the key and in the label alone (the other field clean).
  const std::wstring loneSurrogate(1, static_cast<wchar_t>(0xD800));  // not UTF-16
  for (const std::wstring& bad : {std::wstring(L"P\tproduct"), std::wstring(L"P\nproduct"),
                                  std::wstring(L"P\rproduct"), loneSurrogate}) {
    std::vector<CacheRecord> records = Folder(L"P-product", 100);
    records[0].key = bad;
    EXPECT_FALSE(WriteRecords(dir.File(L"key.tsv"), records));
    records = Folder(L"P-product", 100);
    records[0].label = bad;
    EXPECT_FALSE(WriteRecords(dir.File(L"label.tsv"), records));
  }
  // The same records with clean names are written.
  EXPECT_TRUE(WriteRecords(dir.File(L"clean.tsv"), Folder(L"P-product", 100)));
}

TEST(RefreshJudgeRecords, AnythingElseIsNotRead) {
  TempDir dir(L"judge");
  ASSERT_TRUE(dir.ok());
  const std::string header = "label\tkey\tsize\thr\tcached\tw\th\tflags\tid\n";
  const std::string id = "00112233445566778899aabbccddeeff";
  const std::string good = "P\tP/portrait-1.pdf\t256\t0x00000000\t1\t181\t256\t0x2\t" + id;
  const std::wstring file = dir.File(L"records.tsv");

  ASSERT_TRUE(WriteBytes(file, header + good + "\r\n"));
  std::vector<CacheRecord> read;
  ASSERT_TRUE(ReadRecords(file, &read));
  ASSERT_EQ(read.size(), 1u);
  EXPECT_EQ(read[0].id[15], 0xFF);

  const std::string bad[] = {
      "",                                                                   // no header
      "label\tkey\n",                                                       // another header
      header + good,                                                        // no line break
      header + good + "\t\n",                                               // a field too many
      header + "P\tP/portrait-1.pdf\t256\t0x00000000\t1\t181\t256\t0x2\n",  // one too few
      header + "P\tP/portrait-1.pdf\t-1\t0x00000000\t1\t181\t256\t0x2\t" + id + "\n",
      header + "P\tP/portrait-1.pdf\t256\t0x0000000\t1\t181\t256\t0x2\t" + id + "\n",
      header + "P\tP/portrait-1.pdf\t256\t00000000\t1\t181\t256\t0x2\t" + id + "\n",
      header + "P\tP/portrait-1.pdf\t256\t0x00000000\t2\t181\t256\t0x2\t" + id + "\n",
      header + "P\tP/portrait-1.pdf\t256\t0x00000000\t1\t+181\t256\t0x2\t" + id + "\n",
      header + "P\tP/portrait-1.pdf\t256\t0x00000000\t1\t181\t256\t0x\t" + id + "\n",
      header + "P\tP/portrait-1.pdf\t256\t0x00000000\t1\t181\t256\t0x2\t" + id + "0\n",
      header + "P\tP/portrait-1.pdf\t256\t0x00000000\t1\t181\t256\t0x2\t0x" + id.substr(2) + "\n",
      header + "P\t\xff\t256\t0x00000000\t1\t181\t256\t0x2\t" + id + "\n",  // not UTF-8
  };
  for (const std::string& text : bad) {
    SCOPED_TRACE(text);
    ASSERT_TRUE(WriteBytes(file, text));
    read.assign(1, CacheRecord{});
    EXPECT_FALSE(ReadRecords(file, &read));
    EXPECT_TRUE(read.empty());
  }
  read.assign(1, CacheRecord{});
  EXPECT_FALSE(ReadRecords(dir.File(L"missing.tsv"), &read));
  EXPECT_TRUE(read.empty());
}

}  // namespace
}  // namespace pdft::test
