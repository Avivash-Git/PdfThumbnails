// The Explorer test: one thumbnail extracted the way Explorer extracts it, through the thumbnail
// cache's out-of-process extraction in a fresh instance of the thumbnail surrogate (dllhost), so
// the registered DLLs are really loaded and the result shows what Explorer gets. The cache is
// bypassed and left as it was. The app itself never parses a PDF: the file is handed to the
// shell, and the handler parses it in the surrogate.
//
// The file is either a tiny blank probe PDF written to %TEMP% for the test (the same bytes 1.x
// wrote) and deleted afterwards, or a file the user names (--test-file), which must be an existing
// regular file.
//
// When not to run it (DecideLiveTestSkip): an elevated process would make the shell start an
// elevated surrogate that never loads per-user classes and that the handler refuses to work in,
// and a process whose registry is virtualized may see another handler than Explorer does; in both
// cases a failure would say nothing about Explorer. The same holds for --test-file.

#pragma once

#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace pdft {

struct LiveTest {
  HRESULT hr = E_PENDING;  // the extraction's result; HRESULT_FROM_WIN32(ERROR_TIMEOUT) when the
                           // watchdog gave up
  SIZE size{};             // the bitmap's size when hr is S_OK
  DWORD ms = 0;            // how long the extraction took (until the watchdog gave up)
};

// The thumbnail sizes the test asks for: the probe at 96, a test file at 256 (as 1.x did).
inline constexpr UINT kProbeThumbnailSize = 96;
inline constexpr UINT kTestFileThumbnailSize = 256;

// The watchdog's limit for one extraction.
inline constexpr DWORD kLiveTestTimeoutMs = 60000;

// Extracts the thumbnail of |file| (nullptr: a new probe PDF, deleted afterwards) at |size| pixels
// (1-2560) on a worker thread of its own (a single-threaded apartment), and waits at most
// |timeoutMs|. S_OK when the test was carried out, its outcome in *result (a failed or timed-out
// extraction is an outcome, not a failure of this function); a failure when it could not start
// (the probe could not be written, no thread). |file| is used as given: check a user's path with
// ValidateTestFile first.
//
// After a timeout the worker is left to finish on its own; it owns everything it uses and then
// deletes its probe. A worker that never returns leaves its probe behind for SweepStaleProbes.
[[nodiscard]] HRESULT RunLiveTest(UINT size, const std::filesystem::path* file, DWORD timeoutMs,
                                  LiveTest* result) noexcept;

// The flags of the extraction (WTS_FLAGS), exactly 1.x's: WTS_FORCEEXTRACTION (never answer from
// the cache) | WTS_EXTRACTDONOTCACHE (leave the cache unchanged) | WTS_INSTANCESURROGATE (a new
// surrogate process, so the DLLs are loaded afresh). Never WTS_EXTRACTINPROC: the extraction
// must not run in this process.
inline constexpr DWORD kLiveTestFlags = 0x4 | 0x20 | 0x400;

// ---- When to skip -------------------------------------------------------------------------

enum class LiveTestSkip {
  kNone,           // run it
  kNotRequested,   // --no-live-test (or no test file given)
  kElevated,       // this process is elevated
  kVirtualized,    // this process's registry view is not Explorer's
  kNotOurHandler,  // Windows resolves .pdf thumbnails to another handler: the probe would test
                   // that one (a test file is still run: it shows what Explorer does with it)
  kNoClass,        // no class is registered at all: the probe could only report that (a test
                   // file is still run)
};

struct LiveTestConditions {
  bool requested = true;
  bool testFile = false;  // a --test-file run rather than the probe
  bool elevated = false;
  bool virtualized = false;
  bool effectiveIsOurs = false;  // QueryEffectiveHandler named our class
  bool classRegistered = true;   // a class of ours is registered, per user or machine-wide
};

// Whether to skip, and why (pure). The first reason in the order of LiveTestSkip wins.
LiveTestSkip DecideLiveTestSkip(const LiveTestConditions& conditions) noexcept;

// ---- The probe PDF ------------------------------------------------------------------------

// The probe's bytes (pure): 1.x's PDF 1.4 with one blank 200 x 200 page, LF line ends, and an
// xref table with the objects' byte offsets. May throw std::bad_alloc.
std::string ProbePdfBytes();

// "<Product>-probe-<GUID>.pdf", the GUID in lower case without braces, as 1.x named its probes
// (pure). May throw std::bad_alloc.
std::wstring ProbeFileName(const GUID& id);

// A name ProbeFileName could have made (pure): the prefix (ignoring case), a GUID of 36
// characters (hex digits of either case and dashes at 8, 13, 18 and 23), ".pdf" (ignoring case).
bool IsProbeFileName(std::wstring_view name) noexcept;

// Writes a new probe into %TEMP% under a new ProbeFileName (never over an existing file) and
// returns its path.
[[nodiscard]] HRESULT WriteProbePdf(std::filesystem::path* path) noexcept;

// Probes older than this are left over (a crashed run or a surrogate that never returned).
inline constexpr ULONGLONG kStaleProbeAge100ns = 60ULL * 60 * 10'000'000;  // 1 hour

// True when a file written at |written| is older than kStaleProbeAge100ns at |now| (pure).
// A time in the future is not.
bool IsStaleProbe(const FILETIME& written, const FILETIME& now) noexcept;

// Deletes the files in |directory| (empty: %TEMP%) whose names pass IsProbeFileName and that are
// IsStaleProbe by their last write time. Folders and links are never deleted, nor anything else.
// |removed| (optional) receives how many went. A probe that cannot be deleted (still open) is
// left for the next run.
[[nodiscard]] HRESULT SweepStaleProbes(const std::filesystem::path& directory = {},
                                       int* removed = nullptr) noexcept;

// ---- --test-file --------------------------------------------------------------------------

enum class TestFileProblem {
  kNone,
  kEmpty,        // no path
  kBadName,      // not a usable path (an embedded null character, a name Windows rejects)
  kDevicePath,   // \\.\..., \\?\..., \??\...: a device or the raw object namespace
  kNotFound,     // nothing there (or a link to nothing)
  kDirectory,    // a folder
  kNotDiskFile,  // a pipe, a console, a device
  kUnreadable,   // there, but it could not be opened to check it
};

// What is wrong with the text of a path before it is opened (pure): kEmpty, kBadName for an
// embedded null character, kDevicePath (also spelled with forward slashes, which Windows takes as
// separators; the "\\?\" long-path form included, which nobody types), else kNone.
TestFileProblem TestFilePathProblem(std::wstring_view path) noexcept;

// What is wrong with an opened path (pure), from GetFileType and its attributes.
TestFileProblem OpenedTestFileProblem(DWORD fileType, DWORD attributes) noexcept;

// Checks a --test-file path and returns the file it ends at: TestFilePathProblem, then the path
// made absolute and opened for attributes only, following links and junctions (a PDF on a
// OneDrive placeholder or behind a junction is still the user's file; the shell would follow the
// link too), OpenedTestFileProblem, and the final path (Canonicalize), which the test then uses.
// S_OK with *problem kNone and *resolved set when the file is fine; otherwise a failure HRESULT
// and *problem says what is wrong.
[[nodiscard]] HRESULT ValidateTestFile(const std::filesystem::path& path,
                                       std::filesystem::path* resolved,
                                       TestFileProblem* problem) noexcept;

}  // namespace pdft
