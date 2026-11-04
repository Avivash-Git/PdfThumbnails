// The automatic verdict of the thumbnail refresh experiment: whether the thumbnails the cache held
// before a refresh are still there afterwards and now show the new zoom's crop, judged from the
// bitmap sizes the cache reports (no pixels). Pure, apart from reading and writing the records'
// files; shared by thumbtest, the unit tests and the system tests.
//
// The experiment's PDFs are synthetic and named for their page: portrait-<n>.pdf holds one A4
// portrait page (595 x 842 pt), landscape-<n>.pdf one A4 landscape page (842 x 595 pt). A record
// names its file by the key "<label>/<file name>", the label standing for the folder (a variant
// of the experiment): names only, never a path.

#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pdft::test {

// What the cache answered for one file at one size.
struct CacheRecord {
  std::wstring label;         // the folder's label ("P-product")
  std::wstring key;           // "<label>/<file name>"
  UINT size = 0;              // the size asked for, in pixels
  HRESULT hr = S_OK;          // GetThumbnail's answer (or why the file could not be asked about)
  bool cached = false;        // S_OK with a bitmap that came from the cache (WTS_CACHED)
  int w = 0, h = 0;           // the bitmap's size; 0 x 0 without one
  DWORD cacheFlags = 0;       // WTS_CACHEFLAGS
  std::array<BYTE, 16> id{};  // the thumbnail's ID in the cache (WTS_THUMBNAILID)
};

// Which zoom a bitmap's size shows.
enum class Crop {
  kNew,                // the zoom after the refresh
  kOld,                // the zoom before it
  kIndistinguishable,  // both zooms give this size
  kOther,              // neither, or the page is not known from the file's name
};

// The crop of a |w| x |h| bitmap of the file |key| names (pure). The bitmap expected at a zoom is
// ComputeCrop(page, E, zoom) for the edge E = max(w, h) it came back with, so it does not matter
// which of the cache's sizes answered. Exact sizes first; when neither zoom's size matches
// exactly and E >= 32, a size within 1 px of one (a bitmap scaled from another size).
Crop ClassifyCrop(std::wstring_view key, int w, int h, int zoomBefore, int zoomAfter) noexcept;

// The verdict on one folder (one label).
struct FolderVerdict {
  std::wstring label;
  bool control = false;       // a control folder: it is not refreshed and must stay old
  size_t baseline = 0;        // entries cached before
  size_t baselineNotOld = 0;  // ... of them, new or neither (Crop) before: the baseline is wrong
  // The baseline entries afterwards, by their crop then (indistinguishable ones in none).
  size_t newAfter = 0;
  size_t oldAfter = 0;
  size_t missAfter = 0;  // not cached any more (or not asked about)
  size_t otherAfter = 0;
  size_t created = 0;    // cached after, but not before (reported, no failure)
  size_t idChanged = 0;  // cached both times under another thumbnail ID (reported, no failure)
  // There was a baseline, every baseline entry was at the old zoom (or indistinguishable), and at
  // least one at the old zoom.
  bool baselineValid = false;
  // The folder did what it should, on a valid baseline: every baseline entry is cached after and
  // new (at least one); for a control, cached after and still old (at least one).
  bool passA = false;
};

// One verdict per label, in the order the labels first appear in |before|, then in |after|
// (pure). An entry is a file at a size: each record of |before| goes with the first record of
// |after| for the same key and size not taken yet. |controls| lists the control folders' labels.
// Labels and keys compare exactly. May throw std::bad_alloc.
std::vector<FolderVerdict> Judge(std::span<const CacheRecord> before,
                                 std::span<const CacheRecord> after, int zoomBefore, int zoomAfter,
                                 std::span<const std::wstring> controls);

// Writes |records| to |file| as tab-separated UTF-8 text with a header line, replacing it. False
// when it cannot be written, or a label or a key holds a tab, a line break or a lone surrogate.
bool WriteRecords(const std::filesystem::path& file, std::span<const CacheRecord> records);

// Reads a file WriteRecords wrote. False (and *records empty) when it cannot be read or is not
// exactly that format.
bool ReadRecords(const std::filesystem::path& file, std::vector<CacheRecord>* records);

}  // namespace pdft::test
