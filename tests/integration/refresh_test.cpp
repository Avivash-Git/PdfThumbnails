// The PDF thumbnail refresh (refresh.h) on folders this test makes under %TEMP%\pdft-tests:
// finding the PDFs in a tree (subfolders, never into a junction or $Recycle.Bin, nor where a
// folder on the way became a junction, the long names only, offline files skipped and offline
// folders not entered, the limits by count, time and depth, the stop), checking a
// --folder, and the whole refresh of a temp folder against a fake thumbnail cache and a fake File
// Explorer (the log keeps counts, not names). Nothing here asks the real thumbnail cache or tells
// the real File Explorer anything; the files are never opened.

#include <windows.h>

#include "refresh.h"

#include <gtest/gtest.h>
#include <thumbcache.h>
#include <wil/resource.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stop_token>
#include <string>
#include <vector>

#include "log.h"
#include "paths.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

using std::filesystem::path;

bool MakeDir(const path& dir) { return CreateDirectoryW(dir.c_str(), nullptr) != FALSE; }

bool WriteFileText(const path& file, const char* text = "%PDF-1.4\n") {
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!handle) return false;
  DWORD written = 0;
  return WriteFile(handle.get(), text, static_cast<DWORD>(std::char_traits<char>::length(text)),
                   &written, nullptr) != FALSE;
}

// |files| relative to |root| ("sub\c.pdf"), in their order.
std::vector<std::wstring> Relative(const std::vector<path>& files, const path& root) {
  std::vector<std::wstring> names;
  for (const path& file : files) names.push_back(file.lexically_relative(root).native());
  return names;
}

std::vector<std::wstring> Sorted(std::vector<std::wstring> names) {
  std::sort(names.begin(), names.end());
  return names;
}

size_t IndexOf(const std::vector<std::wstring>& names, const std::wstring& name) {
  return static_cast<size_t>(std::find(names.begin(), names.end(), name) - names.begin());
}

HRESULT Find(const std::vector<path>& folders, const WalkLimits& limits, std::vector<path>* files,
             RefreshCounts* counts, std::stop_token stop = std::stop_token()) {
  return FindPdfsInFolders(folders, limits, stop, files, counts);
}

size_t Skipped(const RefreshCounts& counts, CandidateSkip skip) {
  return counts.skipped[static_cast<size_t>(skip) - 1];
}

// ---- FindPdfsInFolders ----

TEST(RefreshWalk, FindsTheNestedPdfsOnly) {
  test::TempDir dir(L"refresh-walk");
  test::TempDir outside(L"refresh-outside");
  ASSERT_TRUE(dir.ok());
  ASSERT_TRUE(outside.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"a.pdf"));
  ASSERT_TRUE(WriteFileText(root / L"b.PDF"));
  ASSERT_TRUE(WriteFileText(root / L"notes.txt"));
  // Its 8.3 alias (X~1.PDF) would match "*.pdf"; the listing has only the long name.
  ASSERT_TRUE(WriteFileText(root / L"x.pdfx"));
  ASSERT_TRUE(MakeDir(root / L"sub"));
  ASSERT_TRUE(WriteFileText(root / L"sub" / L"c.pdf"));
  ASSERT_TRUE(MakeDir(root / L"sub" / L"deeper"));
  ASSERT_TRUE(WriteFileText(root / L"sub" / L"deeper" / L"d.pdf"));
  ASSERT_TRUE(MakeDir(root / L"$Recycle.Bin"));
  ASSERT_TRUE(WriteFileText(root / L"$Recycle.Bin" / L"e.pdf"));
  ASSERT_TRUE(MakeDir(root / L"sub" / L"$RECYCLE.BIN"));
  ASSERT_TRUE(WriteFileText(root / L"sub" / L"$RECYCLE.BIN" / L"g.pdf"));
  ASSERT_TRUE(WriteFileText(path(outside.path()) / L"f.pdf"));
  ASSERT_TRUE(MakeDir(root / L"link"));
  ASSERT_TRUE(test::MakeJunction((root / L"link").native(), outside.path()));

  std::vector<path> files;
  RefreshCounts counts;
  ASSERT_EQ(Find({root}, WalkLimits{}, &files, &counts), S_OK);
  const std::vector<std::wstring> names = Relative(files, root);
  EXPECT_EQ(Sorted(names),
            (std::vector<std::wstring>{L"a.pdf", L"b.PDF", L"sub\\c.pdf", L"sub\\deeper\\d.pdf"}));
  // A folder's files before its subfolders.
  EXPECT_LT(IndexOf(names, L"a.pdf"), IndexOf(names, L"sub\\c.pdf"));
  EXPECT_LT(IndexOf(names, L"b.PDF"), IndexOf(names, L"sub\\c.pdf"));
  EXPECT_LT(IndexOf(names, L"sub\\c.pdf"), IndexOf(names, L"sub\\deeper\\d.pdf"));
  EXPECT_EQ(counts.fromWalk, 4u);
  EXPECT_TRUE(counts.walkUsed);
  EXPECT_FALSE(counts.capped);
  for (const size_t skipped : counts.skipped) EXPECT_EQ(skipped, 0u);
}

TEST(RefreshWalk, OfflineFilesAreSkippedAndCounted) {
  test::TempDir dir(L"refresh-offline");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"here.pdf"));
  const path offline = root / L"offline.pdf";
  ASSERT_TRUE(WriteFileText(offline));
  ASSERT_TRUE(SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_OFFLINE));

  std::vector<path> files;
  RefreshCounts counts;
  ASSERT_EQ(Find({root}, WalkLimits{}, &files, &counts), S_OK);
  EXPECT_EQ(Relative(files, root), std::vector<std::wstring>{L"here.pdf"});
  EXPECT_EQ(Skipped(counts, CandidateSkip::kOffline), 1u);
  EXPECT_EQ(counts.fromWalk, 1u);
  (void)SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST(RefreshWalk, OfflineFoldersAreNotEntered) {
  test::TempDir dir(L"refresh-offline-dir");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"here.pdf"));
  const path offline = root / L"offline";
  ASSERT_TRUE(MakeDir(offline));
  ASSERT_TRUE(WriteFileText(offline / L"inside.pdf"));
  ASSERT_TRUE(
      SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE));

  std::vector<path> files;
  RefreshCounts counts;
  ASSERT_EQ(Find({root}, WalkLimits{}, &files, &counts), S_OK);
  EXPECT_EQ(Relative(files, root), std::vector<std::wstring>{L"here.pdf"});
  // Given itself, it is not listed either.
  ASSERT_EQ(Find({offline}, WalkLimits{}, &files, &counts), S_OK);
  EXPECT_TRUE(files.empty());
  (void)SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY);
}

TEST(RefreshWalk, AFolderOnTheWaySwappedForAJunctionIsNotFollowed) {
  test::TempDir dir(L"refresh-swap");
  test::TempDir elsewhere(L"refresh-elsewhere");
  ASSERT_TRUE(dir.ok());
  ASSERT_TRUE(elsewhere.ok());
  const path root = dir.path();
  const path other = elsewhere.path();
  ASSERT_TRUE(WriteFileText(root / L"top.pdf"));
  ASSERT_TRUE(MakeDir(root / L"a"));
  ASSERT_TRUE(WriteFileText(root / L"a" / L"a.pdf"));
  ASSERT_TRUE(MakeDir(root / L"a" / L"b"));
  ASSERT_TRUE(WriteFileText(root / L"a" / L"b" / L"b.pdf"));
  ASSERT_TRUE(MakeDir(other / L"b"));
  ASSERT_TRUE(WriteFileText(other / L"b" / L"other.pdf"));

  // The walk looks at the clock before each folder it lists. Once a was listed (its PDF found)
  // and before a\b is opened, a is moved away and a junction to the other folder takes its place,
  // so the path a\b now leads to the other folder's b.
  std::vector<path> files;
  bool tried = false;
  bool swapped = false;
  WalkLimits limits;
  limits.maxMs = 60000;
  limits.clock = [&]() -> ULONGLONG {
    if (!tried && files.size() == 2) {
      tried = true;
      swapped = MoveFileExW((root / L"a").c_str(), (root / L"a-moved").c_str(), 0) &&
                MakeDir(root / L"a") && test::MakeJunction((root / L"a").native(), other.native());
    }
    return 0;
  };
  RefreshCounts counts;
  ASSERT_EQ(Find({root}, limits, &files, &counts), S_OK);
  ASSERT_TRUE(swapped);
  std::error_code error;
  EXPECT_TRUE(std::filesystem::exists(root / L"a" / L"b" / L"other.pdf", error));
  // It was not listed there.
  EXPECT_EQ(Sorted(Relative(files, root)), (std::vector<std::wstring>{L"a\\a.pdf", L"top.pdf"}));
}

TEST(RefreshWalk, StopsAtTheFileLimit) {
  test::TempDir dir(L"refresh-cap");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(WriteFileText(root / (std::to_wstring(i) + L".pdf")));
  }
  WalkLimits limits;
  limits.maxFiles = 3;
  std::vector<path> files;
  RefreshCounts counts;
  ASSERT_EQ(Find({root}, limits, &files, &counts), S_OK);
  EXPECT_EQ(files.size(), 3u);
  EXPECT_TRUE(counts.capped);
  EXPECT_EQ(counts.fromWalk, 3u);

  // Exactly as many as allowed: nothing was left out.
  limits.maxFiles = 5;
  RefreshCounts all;
  ASSERT_EQ(Find({root}, limits, &files, &all), S_OK);
  EXPECT_EQ(files.size(), 5u);
  EXPECT_FALSE(all.capped);
}

TEST(RefreshWalk, StopsAtTheTimeLimit) {
  test::TempDir dir(L"refresh-time");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"top.pdf"));
  for (int i = 0; i < 6; ++i) {
    const path sub = root / (L"s" + std::to_wstring(i));
    ASSERT_TRUE(MakeDir(sub));
    ASSERT_TRUE(WriteFileText(sub / L"x.pdf"));
  }
  // Every look at the clock takes 10 ms.
  ULONGLONG now = 0;
  WalkLimits limits;
  limits.maxMs = 25;
  limits.clock = [&now]() { return now += 10; };
  std::vector<path> files;
  RefreshCounts counts;
  ASSERT_EQ(Find({root}, limits, &files, &counts), S_OK);
  EXPECT_TRUE(counts.capped);
  EXPECT_GE(files.size(), 1u);
  EXPECT_LT(files.size(), 7u);

  // Without a limit, all of them.
  RefreshCounts all;
  limits.maxMs = 0;
  ASSERT_EQ(Find({root}, limits, &files, &all), S_OK);
  EXPECT_EQ(files.size(), 7u);
  EXPECT_FALSE(all.capped);
}

TEST(RefreshWalk, TheDepthLimit) {
  test::TempDir dir(L"refresh-depth");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"a.pdf"));
  ASSERT_TRUE(MakeDir(root / L"s"));
  ASSERT_TRUE(WriteFileText(root / L"s" / L"b.pdf"));
  ASSERT_TRUE(MakeDir(root / L"s" / L"t"));
  ASSERT_TRUE(WriteFileText(root / L"s" / L"t" / L"c.pdf"));

  const auto found = [&root](int depth) {
    WalkLimits limits;
    limits.maxDepth = depth;
    std::vector<path> files;
    RefreshCounts counts;
    EXPECT_EQ(Find({root}, limits, &files, &counts), S_OK);
    EXPECT_FALSE(counts.capped);
    return Sorted(Relative(files, root));
  };
  using Names = std::vector<std::wstring>;
  EXPECT_EQ(found(0), Names{L"a.pdf"});
  EXPECT_EQ(found(1), (Names{L"a.pdf", L"s\\b.pdf"}));
  EXPECT_EQ(found(WalkLimits{}.maxDepth), (Names{L"a.pdf", L"s\\b.pdf", L"s\\t\\c.pdf"}));
}

TEST(RefreshWalk, EachFileOnceWhereFoldersOverlap) {
  test::TempDir dir(L"refresh-overlap");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"a.pdf"));
  ASSERT_TRUE(MakeDir(root / L"s"));
  ASSERT_TRUE(WriteFileText(root / L"s" / L"b.pdf"));

  std::vector<path> files;
  RefreshCounts counts;
  ASSERT_EQ(Find({root / L"s", root, root}, WalkLimits{}, &files, &counts), S_OK);
  EXPECT_EQ(Sorted(Relative(files, root)), (std::vector<std::wstring>{L"a.pdf", L"s\\b.pdf"}));
  EXPECT_EQ(counts.fromWalk, 2u);
}

TEST(RefreshWalk, LinksAndMissingFoldersGiveNothing) {
  test::TempDir dir(L"refresh-links");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(MakeDir(root / L"target"));
  ASSERT_TRUE(WriteFileText(root / L"target" / L"a.pdf"));
  ASSERT_TRUE(MakeDir(root / L"link"));
  ASSERT_TRUE(test::MakeJunction((root / L"link").native(), (root / L"target").native()));

  std::vector<path> files;
  RefreshCounts counts;
  // A junction given as the folder is not walked either (--folder resolves links first).
  ASSERT_EQ(Find({root / L"link", root / L"missing"}, WalkLimits{}, &files, &counts), S_OK);
  EXPECT_TRUE(files.empty());
  // A file given as the folder.
  ASSERT_EQ(Find({root / L"target" / L"a.pdf"}, WalkLimits{}, &files, &counts), S_OK);
  EXPECT_TRUE(files.empty());
}

TEST(RefreshWalk, StopAndBadArguments) {
  test::TempDir dir(L"refresh-stop");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"a.pdf"));
  std::stop_source source;
  source.request_stop();
  std::vector<path> files;
  RefreshCounts counts;
  EXPECT_EQ(Find({root}, WalkLimits{}, &files, &counts, source.get_token()),
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_TRUE(files.empty());
  EXPECT_EQ(Find({L"relative\\folder"}, WalkLimits{}, &files, &counts), E_INVALIDARG);
}

// ---- ValidateRefreshFolder ----

TEST(RefreshFolder, AFolderIsTakenInItsFinalForm) {
  test::TempDir dir(L"refresh-folder");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  path expected;
  ASSERT_HRESULT_SUCCEEDED(Canonicalize(root, &expected));
  path folder;
  FolderProblem problem = FolderProblem::kBadName;
  ASSERT_EQ(ValidateRefreshFolder(root.native(), &folder, &problem), S_OK);
  EXPECT_EQ(problem, FolderProblem::kNone);
  EXPECT_EQ(folder, expected);

  // A junction is followed: the folder behind it is the user's.
  ASSERT_TRUE(MakeDir(root / L"target"));
  ASSERT_TRUE(MakeDir(root / L"link"));
  ASSERT_TRUE(test::MakeJunction((root / L"link").native(), (root / L"target").native()));
  ASSERT_HRESULT_SUCCEEDED(Canonicalize(root / L"target", &expected));
  ASSERT_EQ(ValidateRefreshFolder((root / L"link").native(), &folder, &problem), S_OK);
  EXPECT_EQ(folder, expected);
}

TEST(RefreshFolder, WhatIsWrongWithAFolder) {
  test::TempDir dir(L"refresh-folder");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"a.pdf"));
  const auto problemOf = [](std::wstring_view text) {
    path folder = L"C:\\stale";
    FolderProblem problem = FolderProblem::kNone;
    EXPECT_TRUE(FAILED(ValidateRefreshFolder(text, &folder, &problem)));
    EXPECT_TRUE(folder.empty());
    return problem;
  };
  EXPECT_EQ(problemOf((root / L"missing").native()), FolderProblem::kNotFound);
  EXPECT_EQ(problemOf((root / L"missing" / L"deeper").native()), FolderProblem::kNotFound);
  EXPECT_EQ(problemOf((root / L"a.pdf").native()), FolderProblem::kNotAFolder);
  EXPECT_EQ(problemOf(L""), FolderProblem::kBadName);
  EXPECT_EQ(problemOf(std::wstring_view(L"C:\\a\0b", 6)), FolderProblem::kBadName);
  EXPECT_EQ(problemOf(L"\\\\.\\C:"), FolderProblem::kDevicePath);
  EXPECT_EQ(problemOf(L"\\\\?\\C:\\Windows"), FolderProblem::kDevicePath);
  EXPECT_EQ(problemOf(L"NUL"), FolderProblem::kDevicePath);
  // Refused by its text: no computer of that name is ever asked.
  EXPECT_EQ(problemOf(L"\\\\pdft-no-such-host.invalid\\share"), FolderProblem::kNetwork);
  EXPECT_EQ(problemOf(L"//pdft-no-such-host.invalid/share"), FolderProblem::kNetwork);
}

// ---- RefreshPdfThumbnails against a fake cache ----

// A cache that holds every file at |sizes| and records what it is asked.
class FakeCache final : public ThumbnailCacheOps {
 public:
  HRESULT Select(const path& file) noexcept override {
    selected.push_back(file);
    return S_OK;
  }
  HRESULT Get(UINT size, DWORD flags, CacheAnswer* answer) noexcept override {
    *answer = {};
    ++gets;
    if (std::find(sizes.begin(), sizes.end(), size) == sizes.end()) return E_FAIL;
    answer->bitmap = true;
    if (flags & WTS_INCACHEONLY) answer->cacheFlags = WTS_CACHED;
    return S_OK;
  }
  std::vector<UINT> sizes = {256};
  std::vector<path> selected;
  size_t gets = 0;
};

class FakeNotifier final : public ShellNotifier {
 public:
  void ItemUpdated(const path& file) noexcept override { items.push_back(file); }
  void FolderUpdated(const path&) noexcept override {}
  void RefreshViews(std::span<const path>) noexcept override {}
  void AssociationsChanged() noexcept override {}
  std::vector<path> items;
};

std::string ReadLog(const path& log) {
  std::ifstream stream(log, std::ios::binary);
  std::stringstream text;
  text << stream.rdbuf();
  return text.str();
}

TEST(RefreshFake, RefreshesAFolderAndLogsCountsNotNames) {
  test::TempDir dir(L"refresh-run");
  ASSERT_TRUE(dir.ok());
  const path root = path(dir.path()) / L"very-private-folder";
  ASSERT_TRUE(MakeDir(root));
  ASSERT_TRUE(WriteFileText(root / L"secret-one.pdf"));
  ASSERT_TRUE(MakeDir(root / L"inner"));
  ASSERT_TRUE(WriteFileText(root / L"inner" / L"secret-two.pdf"));
  ASSERT_TRUE(WriteFileText(root / L"secret-three.pdf"));

  PdfRefreshOptions options;
  options.folders = {root};
  options.files.sizes = {96, 256};
  FakeCache cache;
  FakeNotifier notifier;
  std::vector<RefreshPhase> phases;
  const RefreshProgressFn progress = [&phases](const RefreshProgress& p) {
    phases.push_back(p.phase);
  };

  const path log = path(dir.path()) / L"refresh.log";
  SetLogFileForTesting(log, kLogRotateBytes);
  RefreshCounts counts;
  const HRESULT hr = RefreshPdfThumbnails(options, cache, notifier, progress, {}, &counts);
  SetLogFileForTesting({}, 0);
  ASSERT_EQ(hr, S_OK);

  EXPECT_EQ(counts.fromWalk, 3u);
  EXPECT_EQ(counts.files, 3u);
  EXPECT_EQ(counts.entriesCached, 3u);
  EXPECT_EQ(counts.refreshed, 3u);
  EXPECT_EQ(counts.notCached, 0u);
  // The files of one folder together, that folder's files before its subfolder's.
  ASSERT_EQ(cache.selected.size(), 3u);
  EXPECT_EQ(cache.selected[2], root / L"inner" / L"secret-two.pdf");
  EXPECT_EQ(notifier.items.size(), 3u);
  ASSERT_FALSE(phases.empty());
  EXPECT_EQ(phases.front(), RefreshPhase::kFinding);

  const std::string lines = ReadLog(log);
  EXPECT_NE(lines.find("refresh start source=folders zoom="), std::string::npos) << lines;
  EXPECT_NE(lines.find("refresh end files=3 cached=3 refreshed=3 failed=0 fail-hr=0x00000000 "
                       "gone=0 skipped=0,0,0,0,0,0 sources=0,0,0,3 index=no capped=no "
                       "stopped=no sizes-dropped=none ms="),
            std::string::npos)
      << lines;
  EXPECT_EQ(lines.find("very-private-folder"), std::string::npos) << lines;
  EXPECT_EQ(lines.find("secret"), std::string::npos) << lines;
  EXPECT_EQ(lines.find("pdft-tests"), std::string::npos) << lines;
}

TEST(RefreshFake, AStopWhileFindingRefreshesNothing) {
  test::TempDir dir(L"refresh-run");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"a.pdf"));
  PdfRefreshOptions options;
  options.folders = {root};
  FakeCache cache;
  FakeNotifier notifier;
  std::stop_source source;
  source.request_stop();
  RefreshCounts counts;
  EXPECT_EQ(RefreshPdfThumbnails(options, cache, notifier, {}, source.get_token(), &counts),
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_TRUE(counts.stopped);
  EXPECT_TRUE(cache.selected.empty());
  EXPECT_TRUE(notifier.items.empty());
}

TEST(RefreshFake, BadOptionsMakeNoCall) {
  test::TempDir dir(L"refresh-run");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(WriteFileText(root / L"a.pdf"));
  FakeCache cache;
  FakeNotifier notifier;
  RefreshCounts counts;

  PdfRefreshOptions none;  // no folder
  EXPECT_EQ(RefreshPdfThumbnails(none, cache, notifier, {}, {}, &counts), E_INVALIDARG);
  PdfRefreshOptions inProcess;
  inProcess.folders = {root};
  inProcess.files.extractFlags |= WTS_EXTRACTINPROC;
  EXPECT_EQ(RefreshPdfThumbnails(inProcess, cache, notifier, {}, {}, &counts), E_INVALIDARG);
  PdfRefreshOptions noStore;
  noStore.folders = {root};
  noStore.files.storeFlags = WTS_EXTRACTDONOTCACHE;
  EXPECT_EQ(RefreshPdfThumbnails(noStore, cache, notifier, {}, {}, &counts), E_INVALIDARG);
  EXPECT_TRUE(cache.selected.empty());
  EXPECT_EQ(cache.gets, 0u);
}

}  // namespace
}  // namespace pdft
