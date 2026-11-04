// The refresh experiment's verdict (refresh_judge.h): which zoom a bitmap's size shows, for A4
// portrait and landscape at every size the experiment asks for, exactly and (from 32 px on)
// within 1 px; the folder rules (a valid baseline, refreshed folders, control folders, entries
// created and IDs changed, each entry taken once); the record files written and read back under
// %TEMP% (TempDir). Nothing here asks the thumbnail cache.

#include <windows.h>

#include "refresh_judge.h"

#include <gtest/gtest.h>

#include <array>
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
