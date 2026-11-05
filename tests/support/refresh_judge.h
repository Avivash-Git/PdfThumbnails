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
  bool cached = false;        // held: from the cache, the size's own file (ClassifyCacheAnswer)
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

// How a folder came out.
enum class FolderOutcome {
  kInvalid,    // the baseline cannot show it
  kFail,       // a control: it changed
  kPass,       // a control: it held
  kUndecided,  // nothing failed, but the cache dropped too many files to show it (rules only)
};

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
  // least one at the old zoom. With rules: enough files judged, and none new or neither.
  bool baselineValid = false;
  // The folder did what it should, on a valid baseline: every baseline entry is cached after and
  // new (at least one); for a control, cached after and still old (at least one). With rules:
  // outcome is kPass.
  bool passA = false;
  FolderOutcome outcome = FolderOutcome::kInvalid;

  // Judged with JudgeRules; these stay 0 (and empty) otherwise. Only the entries held before at
  // the old zoom are judged, and with rules the counts above are of them.
  bool ruled = false;
  size_t notHeldBefore = 0;  // entries at a judged size not held before: not judged
  size_t madeWrong = 0;     // judged, and not made again at the new zoom by the refresh (a failure)
  size_t madeWrongOld = 0;  // ... of them, held afterwards at the old zoom
  size_t madeWrongGone = 0;  // ... of them, not held afterwards
  size_t droppedAfter = 0;   // not held any more, with every other one of their file (no failure)
  std::vector<std::wstring> filesDropped;  // ... their files' keys, in the order of |before|
  size_t filesGoneBefore = 0;              // ... of them, files the refresh found gone already
  size_t heldAfter = 0;       // judged entries held afterwards, whatever they show (not made wrong)
  size_t filesHeldAfter = 0;  // ... their files
  size_t heldNewCached = 0;   // entries held and new whose own extraction answered WTS_CACHED
  bool refreshFailed = false;  // a refreshed folder whose refresh reported a failure
  bool nothingLeft = false;    // a refreshed folder: no file the refresh reached holds anything
  // At the judged size with the fewest: the files of each page with a judged entry, and with an
  // entry held afterwards that shows the crop the folder should (new; old for a control).
  size_t portraitBefore = 0;
  size_t landscapeBefore = 0;
  size_t portraitAfter = 0;
  size_t landscapeAfter = 0;
};

// One verdict per label, in the order the labels first appear in |before|, then in |after|
// (pure). An entry is a file at a size: each record of |before| goes with the first record of
// |after| for the same key and size not taken yet. |controls| lists the control folders' labels.
// Labels and keys compare exactly. The outcome is kInvalid, kPass or kFail, as baselineValid and
// passA say. May throw std::bad_alloc.
std::vector<FolderVerdict> Judge(std::span<const CacheRecord> before,
                                 std::span<const CacheRecord> after, int zoomBefore, int zoomAfter,
                                 std::span<const std::wstring> controls);

// How lost entries count. The cache keeps one record per file and drops a whole file (every size
// at once) while others are stored: a file with no entry left is not a refresh's doing when the
// refresh's own extraction made it at the new zoom and the cache said it stored it.
struct JudgeRules {
  std::span<const UINT> sizes;  // the sizes judged
  // At every size of |sizes|: at least this many files of each page (portrait, landscape) held at
  // the old zoom before, and as many afterwards with the crop the folder should show.
  size_t filesPerPage = 1;
  // For a folder refreshed between |before| and |after| (not a control): whether that refresh
  // reported no failure (no file or extraction failed; for the command-line executable, exit
  // code 0).
  bool refreshClean = true;
  // That refresh's extraction answers (every call without WTS_INCACHEONLY), in order, when it ran
  // in this process; empty for a control, or for a refresh in another process.
  std::span<const CacheRecord> made;
  // That refresh ran in this process, so |made| holds every extraction it made: a file with none
  // was not reached, also when |made| is empty (the refresh found every file gone already).
  // Records of the folder in |made| say so too.
  bool inProcess = false;
};

// The verdicts as above, by |rules| (pure). A judged entry is a file at a size of |rules|.sizes
// held before with the old crop; one not held before is counted in notHeldBefore. In a folder
// refreshed in this process, the refresh reached a file with at least one |made| record; each
// judged entry of such a file must have its own last record (the last call counts) S_OK with a
// bitmap of the new crop, and an entry held afterwards in a file it did not reach is made wrong
// too. Afterwards each judged entry is made wrong; held (new, old or neither); dropped: not held,
// its file holds none of its judged entries any more, and, for a refreshed folder, its own
// extraction answered WTS_CACHED or its file was not reached (in this process), or the refresh
// was clean (in another); or gone. The outcome: kInvalid without enough files judged before, or
// with an entry new or neither before; kFail for an entry gone, neither, made wrong, or old (new,
// for a control), and for a refreshed folder whose refresh was not clean or whose files reached
// (every judged file, in another process; at least one) hold nothing afterwards; kPass with
// enough files shown afterwards; kUndecided otherwise. May throw std::bad_alloc.
std::vector<FolderVerdict> Judge(std::span<const CacheRecord> before,
                                 std::span<const CacheRecord> after, int zoomBefore, int zoomAfter,
                                 std::span<const std::wstring> controls, const JudgeRules& rules);

// Writes |records| to |file| as tab-separated UTF-8 text with a header line, replacing it. False
// when it cannot be written, or a label or a key holds a tab, a line break or a lone surrogate.
bool WriteRecords(const std::filesystem::path& file, std::span<const CacheRecord> records);

// Reads a file WriteRecords wrote. False (and *records empty) when it cannot be read or is not
// exactly that format.
bool ReadRecords(const std::filesystem::path& file, std::vector<CacheRecord>* records);

}  // namespace pdft::test
