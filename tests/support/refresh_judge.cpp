#include <windows.h>

#include "refresh_judge.h"

#include <thumbcache.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "crop_math.h"

namespace pdft::test {
namespace {

// Within 1 px only from this edge on: a bitmap scaled from another size may be 1 px off, but
// below it 1 px is all that tells the zooms apart (at 16, A4 portrait is 15 x 16 at 130 % and
// 16 x 16 at 142 %).
constexpr unsigned kScaledMinEdge = 32;

// The records' file: this header, then one line per record, every line ending with a line break.
constexpr std::string_view kHeader = "label\tkey\tsize\thr\tcached\tw\th\tflags\tid";
constexpr size_t kFields = 9;

// Larger files are refused: the experiment's largest record is a few MB.
constexpr ULONGLONG kMaxRecordBytes = 64ull << 20;

// The file's page, in points (see the top of refresh_judge.h).
struct Page {
  double w = 0;
  double h = 0;
};

bool StartsWithIgnoringCase(std::wstring_view text, std::wstring_view prefix) noexcept {
  return text.size() >= prefix.size() &&
         CompareStringOrdinal(text.data(), static_cast<int>(prefix.size()), prefix.data(),
                              static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

bool PageOf(std::wstring_view key, Page* page) noexcept {
  const size_t slash = key.find_last_of(L"/\\");
  const std::wstring_view name = slash == std::wstring_view::npos ? key : key.substr(slash + 1);
  if (StartsWithIgnoringCase(name, L"portrait-")) {
    *page = {595, 842};
    return true;
  }
  if (StartsWithIgnoringCase(name, L"landscape-")) {
    *page = {842, 595};
    return true;
  }
  return false;
}

bool Within(int w, int h, const CropGeometry& crop, int pixels) noexcept {
  return std::abs(w - crop.bmpW) <= pixels && std::abs(h - crop.bmpH) <= pixels;
}

bool HasBreak(const std::wstring& text) noexcept {
  return text.find_first_of(L"\t\r\n") != std::wstring::npos;
}

bool ToUtf8(const std::wstring& text, std::string* out) {
  out->clear();
  if (text.empty()) return true;
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (size <= 0) return false;
  out->resize(static_cast<size_t>(size));
  return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), out->data(), size, nullptr,
                             nullptr) == size;
}

bool FromUtf8(std::string_view text, std::wstring* out) {
  out->clear();
  if (text.empty()) return true;
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  if (size <= 0) return false;
  out->resize(static_cast<size_t>(size));
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), out->data(), size) == size;
}

// The whole of |text| as an unsigned number in |base| (no sign, no prefix, no spaces).
template <typename T>
bool ParseWhole(std::string_view text, int base, T* value) noexcept {
  if (text.empty()) return false;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), *value, base);
  return error == std::errc() && end == text.data() + text.size();
}

// "0x" and 1 to |digits| hex digits; exactly |digits| when |exact|.
bool ParseHex(std::string_view text, size_t digits, bool exact, unsigned long* value) noexcept {
  if (text.size() < 3 || text.substr(0, 2) != "0x") return false;
  text.remove_prefix(2);
  if (text.size() > digits || (exact && text.size() != digits)) return false;
  for (char c : text) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
  }
  return ParseWhole(text, 16, value);
}

bool ParseRecord(std::string_view line, CacheRecord* record) {
  std::string_view fields[kFields];
  size_t count = 0;
  size_t start = 0;
  while (true) {
    const size_t tab = line.find('\t', start);
    if (count == kFields) return false;
    fields[count++] = line.substr(start, tab == std::string_view::npos ? tab : tab - start);
    if (tab == std::string_view::npos) break;
    start = tab + 1;
  }
  if (count != kFields) return false;
  unsigned long hr = 0, flags = 0;
  unsigned size = 0, w = 0, h = 0;
  if (!FromUtf8(fields[0], &record->label) || !FromUtf8(fields[1], &record->key) ||
      !ParseWhole(fields[2], 10, &size) || !ParseHex(fields[3], 8, true, &hr) ||
      (fields[4] != "0" && fields[4] != "1") || !ParseWhole(fields[5], 10, &w) ||
      !ParseWhole(fields[6], 10, &h) || !ParseHex(fields[7], 8, false, &flags) ||
      fields[8].size() != 2 * record->id.size() || w > static_cast<unsigned>(INT_MAX) ||
      h > static_cast<unsigned>(INT_MAX)) {
    return false;
  }
  for (size_t i = 0; i < record->id.size(); ++i) {
    const std::string_view pair = fields[8].substr(2 * i, 2);
    unsigned byte = 0;
    if (!std::isxdigit(static_cast<unsigned char>(pair[0])) ||
        !std::isxdigit(static_cast<unsigned char>(pair[1])) || !ParseWhole(pair, 16, &byte)) {
      return false;
    }
    record->id[i] = static_cast<BYTE>(byte);
  }
  record->size = size;
  record->hr = static_cast<HRESULT>(hr);
  record->cached = fields[4] == "1";
  record->w = static_cast<int>(w);
  record->h = static_cast<int>(h);
  record->cacheFlags = flags;
  return true;
}

bool WriteWholeFile(const std::filesystem::path& file, const std::string& text) {
  if (text.size() > MAXDWORD) return false;
  HANDLE handle =
      CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok =
      WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
      written == text.size();
  return CloseHandle(handle) && ok;
}

bool ReadWholeFile(const std::filesystem::path& file, std::string* text) {
  HANDLE handle =
      CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  bool ok = GetFileSizeEx(handle, &size) && size.QuadPart >= 0 &&
            static_cast<ULONGLONG>(size.QuadPart) <= kMaxRecordBytes;
  if (ok) {
    text->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = text->empty() ||
         (ReadFile(handle, text->data(), static_cast<DWORD>(text->size()), &read, nullptr) &&
          read == text->size());
  }
  CloseHandle(handle);
  return ok;
}

}  // namespace

Crop ClassifyCrop(std::wstring_view key, int w, int h, int zoomBefore, int zoomAfter) noexcept {
  Page page;
  if (w <= 0 || h <= 0 || !PageOf(key, &page)) return Crop::kOther;
  const unsigned edge = static_cast<unsigned>(std::max(w, h));
  const CropGeometry before = ComputeCrop(page.w, page.h, edge, zoomBefore);
  const CropGeometry after = ComputeCrop(page.w, page.h, edge, zoomAfter);
  for (const int pixels : {0, 1}) {
    if (pixels > 0 && edge < kScaledMinEdge) break;
    const bool old = Within(w, h, before, pixels);
    const bool now = Within(w, h, after, pixels);
    if (old && now) return Crop::kIndistinguishable;
    if (now) return Crop::kNew;
    if (old) return Crop::kOld;
  }
  return Crop::kOther;
}

std::vector<FolderVerdict> Judge(std::span<const CacheRecord> before,
                                 std::span<const CacheRecord> after, int zoomBefore, int zoomAfter,
                                 std::span<const std::wstring> controls) {
  std::vector<FolderVerdict> verdicts;
  std::vector<size_t> oldBefore;  // per verdict: baseline entries at the old zoom
  std::map<std::wstring, size_t> byLabel;
  const auto verdictOf = [&](const std::wstring& label) {
    const auto [at, added] = byLabel.try_emplace(label, verdicts.size());
    if (added) {
      FolderVerdict& verdict = verdicts.emplace_back();
      verdict.label = label;
      verdict.control = std::find(controls.begin(), controls.end(), label) != controls.end();
      oldBefore.push_back(0);
    }
    return at->second;
  };

  // The records of |after| per entry, in their order; each goes with one record of |before|.
  std::map<std::pair<std::wstring, UINT>, std::vector<size_t>> afterOf;
  for (size_t i = 0; i < after.size(); ++i) afterOf[{after[i].key, after[i].size}].push_back(i);
  std::map<std::pair<std::wstring, UINT>, size_t> takenOf;
  std::vector<bool> inBaseline(after.size(), false);  // goes with an entry cached before

  for (const CacheRecord& old : before) {
    const size_t index = verdictOf(old.label);
    FolderVerdict& verdict = verdicts[index];
    const CacheRecord* now = nullptr;
    const std::pair<std::wstring, UINT> entry{old.key, old.size};
    if (const auto found = afterOf.find(entry); found != afterOf.end()) {
      size_t& taken = takenOf[entry];
      if (taken < found->second.size()) {
        const size_t at = found->second[taken++];
        now = &after[at];
        inBaseline[at] = old.cached;
      }
    }
    if (!old.cached) continue;
    ++verdict.baseline;
    const Crop was = ClassifyCrop(old.key, old.w, old.h, zoomBefore, zoomAfter);
    if (was == Crop::kOld) {
      ++oldBefore[index];
    } else if (was != Crop::kIndistinguishable) {
      ++verdict.baselineNotOld;
    }
    if (!now || !now->cached) {
      ++verdict.missAfter;
      continue;
    }
    if (now->id != old.id) ++verdict.idChanged;
    switch (ClassifyCrop(now->key, now->w, now->h, zoomBefore, zoomAfter)) {
      case Crop::kNew:
        ++verdict.newAfter;
        break;
      case Crop::kOld:
        ++verdict.oldAfter;
        break;
      case Crop::kOther:
        ++verdict.otherAfter;
        break;
      case Crop::kIndistinguishable:
        break;
    }
  }
  for (size_t i = 0; i < after.size(); ++i) {
    const size_t index = verdictOf(after[i].label);
    if (after[i].cached && !inBaseline[i]) ++verdicts[index].created;
  }

  for (size_t i = 0; i < verdicts.size(); ++i) {
    FolderVerdict& v = verdicts[i];
    v.baselineValid = v.baseline > 0 && v.baselineNotOld == 0 && oldBefore[i] > 0;
    const size_t expected = v.control ? v.oldAfter : v.newAfter;
    const size_t wrong = v.missAfter + v.otherAfter + (v.control ? v.newAfter : v.oldAfter);
    v.passA = v.baselineValid && expected > 0 && wrong == 0;
    v.outcome = !v.baselineValid ? FolderOutcome::kInvalid
                : v.passA        ? FolderOutcome::kPass
                                 : FolderOutcome::kFail;
  }
  return verdicts;
}

std::vector<FolderVerdict> Judge(std::span<const CacheRecord> before,
                                 std::span<const CacheRecord> after, int zoomBefore, int zoomAfter,
                                 std::span<const std::wstring> controls, const JudgeRules& rules) {
  std::vector<FolderVerdict> verdicts;
  std::map<std::wstring, size_t> byLabel;
  const auto verdictOf = [&](const std::wstring& label) {
    const auto [at, added] = byLabel.try_emplace(label, verdicts.size());
    if (added) {
      FolderVerdict& verdict = verdicts.emplace_back();
      verdict.label = label;
      verdict.control = std::find(controls.begin(), controls.end(), label) != controls.end();
      verdict.ruled = true;
    }
    return at->second;
  };
  const auto judgedSize = [&rules](UINT size) {
    return std::find(rules.sizes.begin(), rules.sizes.end(), size) != rules.sizes.end();
  };

  // The refresh's last extraction answer per entry, every one per file in order, the files it
  // reached and their folders.
  std::map<std::pair<std::wstring, UINT>, const CacheRecord*> lastMade;
  std::map<std::wstring, std::vector<const CacheRecord*>> madeOf;
  std::set<std::wstring> reached, refreshed;
  for (const CacheRecord& made : rules.made) {
    lastMade[{made.key, made.size}] = &made;
    madeOf[made.key].push_back(&made);
    reached.insert(made.key);
    refreshed.insert(made.label);
  }
  // The answer an entry is judged by: its own last one; without one, its file's last at a larger
  // size, since a store replaces the file's record with that size and every smaller one.
  const auto madeFor = [&](const std::pair<std::wstring, UINT>& entry) -> const CacheRecord* {
    if (const auto found = lastMade.find(entry); found != lastMade.end()) return found->second;
    const auto file = madeOf.find(entry.first);
    if (file == madeOf.end()) return nullptr;
    for (auto it = file->second.rbegin(); it != file->second.rend(); ++it) {
      if ((*it)->size > entry.second) return *it;
    }
    return nullptr;
  };

  // The records of |after| per entry, in their order; each goes with one record of |before|.
  std::map<std::pair<std::wstring, UINT>, std::vector<size_t>> afterOf;
  for (size_t i = 0; i < after.size(); ++i) afterOf[{after[i].key, after[i].size}].push_back(i);
  std::map<std::pair<std::wstring, UINT>, size_t> takenOf;
  std::vector<bool> inBaseline(after.size(), false);  // goes with an entry cached before

  // The judged entries, and their files in the order they first appear.
  struct Judged {
    std::pair<std::wstring, UINT> entry;
    const CacheRecord* now = nullptr;  // its record afterwards, if any
  };
  struct File {
    size_t verdict = 0;
    std::wstring key;
    std::vector<Judged> entries;
    bool anyHeld = false;      // a judged entry is held afterwards
    bool heldCounted = false;  // ... and counts in heldAfter
    bool dropped = false;
  };
  std::vector<File> files;
  std::map<std::wstring, size_t> fileOf;

  for (const CacheRecord& old : before) {
    const size_t index = verdictOf(old.label);
    FolderVerdict& verdict = verdicts[index];
    const CacheRecord* now = nullptr;
    const std::pair<std::wstring, UINT> entry{old.key, old.size};
    if (const auto found = afterOf.find(entry); found != afterOf.end()) {
      size_t& taken = takenOf[entry];
      if (taken < found->second.size()) {
        const size_t at = found->second[taken++];
        now = &after[at];
        inBaseline[at] = old.cached;
      }
    }
    if (!judgedSize(old.size)) continue;
    if (!old.cached) {
      ++verdict.notHeldBefore;
      continue;
    }
    ++verdict.baseline;
    if (now && now->cached && now->id != old.id) ++verdict.idChanged;
    const Crop was = ClassifyCrop(old.key, old.w, old.h, zoomBefore, zoomAfter);
    if (was != Crop::kOld) {
      if (was != Crop::kIndistinguishable) ++verdict.baselineNotOld;
      continue;
    }
    const auto [at, added] = fileOf.try_emplace(old.key, files.size());
    if (added) files.push_back({index, old.key, {}, false, false, false});
    File& file = files[at->second];
    file.entries.push_back({entry, now});
    if (now && now->cached) file.anyHeld = true;
  }
  for (size_t i = 0; i < after.size(); ++i) {
    const size_t index = verdictOf(after[i].label);
    if (after[i].cached && !inBaseline[i]) ++verdicts[index].created;
  }

  // Per folder, per judged size, per page (portrait, landscape): the files judged before and the
  // files shown afterwards.
  using PageFiles = std::array<std::set<std::wstring>, 2>;
  std::vector<std::map<UINT, PageFiles>> judgedAt(verdicts.size()), shownAt(verdicts.size());

  // A folder refreshed in this process: a file of it without extraction answers was not reached.
  const auto inProcess = [&rules, &refreshed](const FolderVerdict& v) {
    return !v.control && (rules.inProcess || refreshed.count(v.label) > 0);
  };

  for (File& file : files) {
    FolderVerdict& v = verdicts[file.verdict];
    const bool withMade = inProcess(v);
    const bool fileReached = reached.count(file.key) > 0;
    // The page's place in PageFiles; none for a page the name does not give.
    Page page;
    const std::optional<size_t> side =
        PageOf(file.key, &page) ? std::optional<size_t>(page.w < page.h ? 0 : 1) : std::nullopt;
    for (const Judged& judged : file.entries) {
      if (side) judgedAt[file.verdict][judged.entry.second][*side].insert(file.key);
      const bool held = judged.now && judged.now->cached;
      const CacheRecord* made = madeFor(judged.entry);
      const bool madeNew =
          made && made->hr == S_OK && made->w > 0 && made->h > 0 &&
          ClassifyCrop(file.key, made->w, made->h, zoomBefore, zoomAfter) == Crop::kNew;
      const bool storedSaid = made && (made->cacheFlags & WTS_CACHED);
      if (withMade && (fileReached ? !madeNew : held)) {
        ++v.madeWrong;
        if (!held) {
          ++v.madeWrongGone;
        } else if (ClassifyCrop(file.key, judged.now->w, judged.now->h, zoomBefore, zoomAfter) ==
                   Crop::kOld) {
          ++v.madeWrongOld;
        }
        continue;
      }
      if (held) {
        ++v.heldAfter;
        file.heldCounted = true;
        const Crop crop =
            ClassifyCrop(file.key, judged.now->w, judged.now->h, zoomBefore, zoomAfter);
        switch (crop) {
          case Crop::kNew:
            ++v.newAfter;
            if (storedSaid) ++v.heldNewCached;
            break;
          case Crop::kOld:
            ++v.oldAfter;
            break;
          case Crop::kOther:
            ++v.otherAfter;
            break;
          case Crop::kIndistinguishable:
            break;
        }
        if (side && crop == (v.control ? Crop::kOld : Crop::kNew)) {
          shownAt[file.verdict][judged.entry.second][*side].insert(file.key);
        }
        continue;
      }
      const bool dropped =
          !file.anyHeld &&
          (v.control || (withMade ? !fileReached || storedSaid : rules.refreshClean));
      if (dropped) {
        ++v.droppedAfter;
        file.dropped = true;
      } else {
        ++v.missAfter;
      }
    }
    if (file.dropped) {
      v.filesDropped.push_back(file.key);
      if (withMade && !fileReached) ++v.filesGoneBefore;
    }
    if (file.heldCounted) ++v.filesHeldAfter;
  }

  for (size_t i = 0; i < verdicts.size(); ++i) {
    FolderVerdict& v = verdicts[i];
    // The fewest files of each page at any judged size.
    const auto fewest = [&rules](const std::map<UINT, PageFiles>& at, size_t side) {
      size_t least = SIZE_MAX;
      for (const UINT size : rules.sizes) {
        const auto found = at.find(size);
        least = std::min(least, found == at.end() ? 0 : found->second[side].size());
      }
      return least == SIZE_MAX ? 0 : least;
    };
    v.portraitBefore = fewest(judgedAt[i], 0);
    v.landscapeBefore = fewest(judgedAt[i], 1);
    v.portraitAfter = fewest(shownAt[i], 0);
    v.landscapeAfter = fewest(shownAt[i], 1);
    const bool enoughBefore =
        v.portraitBefore >= rules.filesPerPage && v.landscapeBefore >= rules.filesPerPage;
    const bool enoughAfter =
        v.portraitAfter >= rules.filesPerPage && v.landscapeAfter >= rules.filesPerPage;
    if (!v.control) {
      v.refreshFailed = !rules.refreshClean;
      // The files the refresh reached (every judged one, for a refresh in another process).
      const bool withMade = inProcess(v);
      size_t filesReached = 0, filesEmpty = 0;
      for (const File& file : files) {
        if (file.verdict != i || (withMade && !reached.count(file.key))) continue;
        ++filesReached;
        if (!file.anyHeld) ++filesEmpty;
      }
      v.nothingLeft = filesReached > 0 && filesEmpty == filesReached;
    }
    v.baselineValid = enoughBefore && v.baselineNotOld == 0;
    const size_t wrong =
        v.missAfter + v.otherAfter + v.madeWrong + (v.control ? v.newAfter : v.oldAfter);
    if (!v.baselineValid) {
      v.outcome = FolderOutcome::kInvalid;
    } else if (wrong > 0 || v.refreshFailed || v.nothingLeft) {
      v.outcome = FolderOutcome::kFail;
    } else if (enoughAfter) {
      v.outcome = FolderOutcome::kPass;
    } else {
      v.outcome = FolderOutcome::kUndecided;
    }
    v.passA = v.outcome == FolderOutcome::kPass;
  }
  return verdicts;
}

bool WriteRecords(const std::filesystem::path& file, std::span<const CacheRecord> records) {
  std::string text(kHeader);
  text += '\n';
  std::string label, key;
  for (const CacheRecord& record : records) {
    if (HasBreak(record.label) || HasBreak(record.key) || !ToUtf8(record.label, &label) ||
        !ToUtf8(record.key, &key)) {
      return false;
    }
    char numbers[96];
    std::snprintf(numbers, sizeof(numbers), "\t%u\t0x%08lX\t%d\t%d\t%d\t0x%lX\t", record.size,
                  static_cast<unsigned long>(record.hr), record.cached ? 1 : 0, record.w, record.h,
                  static_cast<unsigned long>(record.cacheFlags));
    text += label;
    text += '\t';
    text += key;
    text += numbers;
    for (const BYTE byte : record.id) {
      char hex[3];
      std::snprintf(hex, sizeof(hex), "%02x", byte);
      text += hex;
    }
    text += '\n';
  }
  return WriteWholeFile(file, text);
}

bool ReadRecords(const std::filesystem::path& file, std::vector<CacheRecord>* records) {
  records->clear();
  std::string text;
  if (!ReadWholeFile(file, &text)) return false;
  std::vector<CacheRecord> read;
  bool header = true;
  size_t start = 0;
  while (start < text.size()) {
    const size_t end = text.find('\n', start);
    if (end == std::string::npos) return false;  // every line ends with a line break
    std::string_view line(text.data() + start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    start = end + 1;
    if (header) {
      if (line != kHeader) return false;
      header = false;
      continue;
    }
    CacheRecord record;
    if (!ParseRecord(line, &record)) return false;
    read.push_back(std::move(record));
  }
  if (header) return false;
  *records = std::move(read);
  return true;
}

}  // namespace pdft::test
