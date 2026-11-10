// thumbtest experiment: the one-sitting thumbnail refresh experiment (docs/TESTING.md), run on the
// user's own PC with synthetic PDFs only. It decides how the PDF-only refresh makes cached
// thumbnails again and how File Explorer is told: the extraction flags are judged automatically
// from the bitmap sizes the cache reports (refresh_judge.h), and, with the user at the PC, by eye
// which way of telling File Explorer makes an open folder window show the new thumbnails.
//
//   thumbtest experiment [--cli <PdfThumbnailsCli.exe>] [--root <dir>] [--non-interactive]
//                        [--keep] [--timing-files <n>] [--instance-files <n>] [--desktop]
//                        [--copy-check]
//
// What it changes, and how it is put back:
// - The saved zoom: 100, then 142, set through the command-line executable as a user sets it
//   (--cli, by default the one next to thumbtest: --zoom <n> --no-refresh). The zoom saved before
//   goes back at the end, after a failure or a stop, and from the console handler on Ctrl+C or
//   when the window closes; the command that puts it back by hand is printed first.
// - Files: a folder of its own (--root, which must not exist yet; by default
//   %TEMP%\PdfThumbnails-refresh-<UTC time>) of synthetic PDFs (the bytes of thumbtest
//   makepdfs), and with --desktop and a yes one more on the Desktop. They are deleted at the end
//   through their own handles, each only while its name still leads to the file written (--keep
//   keeps them).
// - The thumbnail cache: thumbnails of those files only are made, asked about and made again, in
//   the thumbnail process (never in this one). No cache file is deleted, and no File Explorer or
//   thumbnail process is ended.
// - File Explorer: only views that show one of the experiment's folders are refreshed or browsed
//   again (checked once more on the view itself), and thumbtest shows File Open dialogs of its
//   own.
// The report goes to out\refresh-experiment\<UTC time>\ (from thumbtest's folder,
// out\build\<preset>\bin), which git ignores: report.txt and the records it read (TSV). It names
// folders by their labels and files by their synthetic names: the root is "<root>", the Desktop
// "<Desktop>", and no path of the user's appears.
//
// The steps: a read-only pre-flight; the files; the baseline zoom; the cache filled at every
// size (two ladder files at one size each first, then the untouched files once at 256, which
// stand for the user's own thumbnails and are only asked about from then on) and asked at once
// (the baseline), and its answers to other flags recorded; the baseline checked, by the bitmaps'
// sizes (an answer counts only from its size's own file); the zoom flipped, and checked in the
// thumbnail process; each extraction variant the product's refresh allows judged right after its
// own refresh, and the control folder with it (the others are left out, with one line, and get
// no folder). The cache drops a file's thumbnails at every size at once while others are
// stored, so a folder is judged on the files it still holds: one portrait and one landscape page
// at every size (the control, which has 8 files: two of each), and a file dropped whole after
// the refresh made and stored it counts neither way. With the user: rounds judged by eye on the
// files the cache still holds (no notification, per file, per folder, then a ladder of further
// steps; the files' times touched; associations changed), the Desktop and another program's File
// Open dialog. Then the timing folder filled (only now, so its entries cannot push the judged
// ones out of the cache first; with the user, only while the untouched files are all held),
// timing, contention and how fast a refresh stops; with the user, the product's --refresh
// --folder from a Windows Terminal tab, with its exit code, and the copy progress window
// (--copy-check); File Explorer not restarted; the zoom put back, the files deleted and the
// report written. --non-interactive (CI) asks nothing: it leaves out the rounds, the Desktop, the
// dialog, the product check and the copy window, decides Pass A only, and fills the timing folder
// also without a variant that passed.

#include <windows.h>

#include <shlobj.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <tlhelp32.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "explorer_settings.h"
#include "file_ops.h"
#include "medium_il.h"
#include "paths.h"
#include "processes.h"
#include "project_config.h"
#include "rebuild.h"
#include "refresh.h"
#include "refresh_judge.h"
#include "refresh_modes.h"
#include "registration.h"
#include "registry_roots.h"
#include "sac.h"
#include "settings.h"
#include "shell_windows.h"
#include "test_pdfs.h"
#include "thumbtest.h"
#include "zoom.h"

namespace thumbtest {
namespace {

using pdft::test::CacheRecord;
using pdft::test::Crop;
using pdft::test::FolderVerdict;

// The zooms compared: an A4 page at 256 pixels is 181 x 256 at the first and 256 x 256 at the
// second, which is easy to see.
constexpr int kBaselineZoom = 100;
constexpr int kFlippedZoom = 142;

// The sizes the cache is filled at: the product's, and 1024, the largest size the documentation
// names. 1024 lands in 1280's file, so its entry is 1280's: it is reported and asked about, but
// neither judged nor refreshed.
constexpr UINT kExperimentSizes[] = {16, 32, 48, 96, 256, 768, 1024, 1280, 1920, 2560};

// A thumbnail made and stored, as File Explorer has one made (in the shared thumbnail process).
constexpr DWORD kPopulateFlags = static_cast<DWORD>(WTS_EXTRACT | WTS_FORCEEXTRACTION);
// A thumbnail made and not stored: the checks that must leave the cache alone.
constexpr DWORD kNoCacheFlags = static_cast<DWORD>(WTS_FORCEEXTRACTION | WTS_EXTRACTDONOTCACHE);

// An answer neither held nor refused is asked again after each of these waits in turn.
constexpr DWORD kAskAgainMs[] = {2000, 10000};

// The flag answers recorded after the baseline. None extracts in this process, and none but the
// last stores a thumbnail: the question scaled to the size asked; an extraction that stores
// nothing, with WTS_IDEALCACHESIZEONLY and with the product's own extraction flags; last, a
// plain extraction with WTS_IDEALCACHESIZEONLY, answered from the cache or made and stored at the
// baseline zoom (for the probe file, which is never judged).
constexpr DWORD kScaledProbeFlags = static_cast<DWORD>(WTS_INCACHEONLY | WTS_SCALETOREQUESTEDSIZE);
constexpr DWORD kIdealNoCacheFlags =
    static_cast<DWORD>(WTS_FORCEEXTRACTION | WTS_EXTRACTDONOTCACHE | WTS_IDEALCACHESIZEONLY);
constexpr DWORD kProductNoCacheFlags =
    static_cast<DWORD>(WTS_FORCEEXTRACTION | WTS_EXTRACTDONOTCACHE | WTS_REQUIRESURROGATE);
constexpr DWORD kIdealExtractFlags = static_cast<DWORD>(WTS_EXTRACT | WTS_IDEALCACHESIZEONLY);
// The probe file's sizes (filled at every size), and those of the product's extraction flags.
constexpr UINT kProbeAskSizes[] = {16, 100, 256, 1024, 2560};
constexpr UINT kProductExtractSizes[] = {256, 2560};

// The ladder: portraits each filled at one size only, before the rest, so that the flag answers
// show what the other sizes answer for them. The first one's answers tell whether the bitmap's
// size names the size that answered: a size whose file is below 256's is held with a bitmap of
// that size, 256's sizes (100, 256) are held, and a larger size gets the 256 entry with
// WTS_LOWQUALITY (a smaller size's) or nothing; no size gets a larger size's. The second's tell
// whether an entry is stored at the size asked (100) or at its file's (256).
struct LadderFile {
  const wchar_t* name;
  UINT size;                   // the one size it is filled at
  std::span<const UINT> asks;  // the sizes it is asked about
  bool scaled;                 // also asked with kScaledProbeFlags
};
constexpr UINT kAt256Asks[] = {16, 96, 100, 256, 300, 768, 1280, 2560};
constexpr UINT kAt100Asks[] = {16, 96, 100, 256, 768};
constexpr LadderFile kLadderFiles[] = {{L"portrait-at-256.pdf", 256, kAt256Asks, true},
                                       {L"portrait-at-100.pdf", 100, kAt100Asks, false}};

// The size the timing folder is filled at, and the size of the check that the thumbnail process
// renders at the new zoom.
constexpr UINT kTimingSize = 256;
constexpr UINT kSanitySize = 256;

// The contention probe: one thumbnail of this size, made and not stored, every
// kContentionEveryMs; its idle baseline is taken for kIdleMs.
constexpr UINT kContentionSize = 96;
constexpr DWORD kContentionEveryMs = 500;
constexpr DWORD kIdleMs = 5000;

// The timing folder's files by default and at most; the first kInstanceFiles of them (by
// default: --instance-files) are made again in a thumbnail process of their own.
constexpr long kDefaultTimingFiles = 2000;
constexpr long kMaxTimingFiles = 9999;
constexpr long kInstanceFiles = 200;

// The untouched files: portrait copies each stored once at kUntouchedSize, right after the
// ladder, and then only asked about at that size. They stand for the user's own thumbnails,
// stored once and never touched: with the user, the timing folder is left out once the cache has
// dropped one of them.
constexpr int kUntouchedFiles = 20;
constexpr UINT kUntouchedSize = 256;

// At every size judged, a folder needs this many portrait pages and as many landscape pages held:
// a variant's or a round's folder, and the control (until the product check, from where it is the
// folder refreshed).
constexpr size_t kFilesPerPage = 1;
constexpr size_t kControlFilesPerPage = 2;

// The slow folder's refresh is asked to stop after this long.
constexpr DWORD kCancelAfterMs = 1000;

// A window must turn square within this long.
constexpr double kSquareWithinSeconds = 10.0;

// How long File Explorer gets to answer for its views.
constexpr DWORD kViewTimeoutMs = 5000;

// How long the console handler waits for the command-line executable before it puts the zoom
// back: closing the window leaves a handler about 5 s.
constexpr DWORD kChildWaitOnExitMs = 3000;

// A File Open dialog's process that ends within this long did not show it; one that has not
// ended this long after it was asked to close is left to end when it is closed.
constexpr DWORD kDialogStartMs = 5000;
constexpr DWORD kDialogCloseMs = 10000;

// --copy-check: the file the user copies in File Explorer, and the free space it needs (the file,
// File Explorer's copy of it and some room).
constexpr ULONGLONG kCopyCheckBytes = 1ull << 30;
constexpr ULONGLONG kCopyCheckFreeBytes = 2 * kCopyCheckBytes + (256ull << 20);
constexpr size_t kCopyCheckChunk = 1 << 20;
// The class of File Explorer's copy progress window.
constexpr char kOperationStatusClass[] = "OperationStatusWindow";

// A root no longer than this leaves room for the longest file name below it within MAX_PATH.
constexpr size_t kMaxRootChars = MAX_PATH - 40;

// Moving a file's last write on by 2 s (in 100 ns units).
constexpr ULONGLONG kTouch100ns = 2 * 10'000'000ULL;

constexpr wchar_t kRootPrefix[] = L"PdfThumbnails-refresh-";
constexpr wchar_t kDesktopFileName[] = L"PdfThumbnails-refresh-test.pdf";
constexpr wchar_t kDesktopLabel[] = L"Desktop";
// The Desktop file holds an A4 portrait page, which its name does not say: its bitmaps are
// classified under this key (refresh_judge.h).
constexpr wchar_t kDesktopPageKey[] = L"Desktop/portrait-desktop.pdf";
constexpr wchar_t kCopyCheckName[] = L"copy-check.bin";
constexpr wchar_t kCopyFolderName[] = L"copy";

// The folders, named by their labels. Each judged folder holds portrait-001.pdf and
// portrait-002.pdf (an A4 portrait page) and landscape-001.pdf and landscape-002.pdf (A4
// landscape), the control kControlPages of each: the judge knows each page from its name.
constexpr wchar_t kControlLabel[] = L"C-control";  // never refreshed, until the product check
constexpr int kControlPages = 4;
constexpr wchar_t kNoneLabel[] = L"N-none";
constexpr wchar_t kItemLabel[] = L"I-item";
constexpr wchar_t kDirLabel[] = L"D-dir";
constexpr wchar_t kTouchLabel[] = L"T-touch";
constexpr wchar_t kAssocLabel[] = L"A-assoc";
constexpr wchar_t kDialogLabel[] = L"B-dialog";
constexpr wchar_t kTimingLabel[] = L"timing";  // portrait copies, filled at kTimingSize only
constexpr wchar_t kSlowLabel[] = L"slow";      // heavy pages and the largest page
constexpr wchar_t kProbeLabel[] = L"probe";    // one portrait for checks and flag answers
constexpr wchar_t kLadderLabel[] = L"ladder";  // kLadderFiles, never judged
// kUntouchedFiles portrait copies, stored once and then only asked about.
constexpr wchar_t kUntouchedLabel[] = L"untouched";

// The names of the slow folder's largest pages start with this. They are square, and the judge
// does not know them by their names; the heavy pages are A4 portrait (portrait-heavy-NNN.pdf).
constexpr wchar_t kLargestPrefix[] = L"square-largest-";

// The extraction variants, judged automatically; the best is the first that passes, in this
// order. Those the product's refresh refuses (RefreshAllows) are left out: they get no folder and
// are not judged. That is every one but P-product, the only one that asks for the thumbnail
// process (WTS_REQUIRESURROGATE).
struct Variant {
  const wchar_t* label;
  const char* flags;           // as the report names them
  DWORD extract;               // the extraction's WTS_FLAGS
  std::optional<DWORD> store;  // a second call after it, which stores the thumbnail
};
constexpr Variant kVariants[] = {
    {L"P-product", "FORCEEXTRACTION|REQUIRESURROGATE (the product's)", pdft::kRefreshExtractFlags,
     std::nullopt},
    {L"F-force", "FORCEEXTRACTION", static_cast<DWORD>(WTS_FORCEEXTRACTION), std::nullopt},
    {L"R-slowreclaim", "FORCEEXTRACTION|SLOWRECLAIM",
     static_cast<DWORD>(WTS_FORCEEXTRACTION | WTS_SLOWRECLAIM), std::nullopt},
    {L"S-instance", "FORCEEXTRACTION|INSTANCESURROGATE",
     static_cast<DWORD>(WTS_FORCEEXTRACTION | WTS_INSTANCESURROGATE), std::nullopt},
    {L"E-force-then-store", "FORCEEXTRACTION, then a plain EXTRACT",
     static_cast<DWORD>(WTS_FORCEEXTRACTION), static_cast<DWORD>(WTS_EXTRACT)},
};

// The own-process pass's extraction: the first timing files made again, each in a thumbnail
// process of its own.
constexpr DWORD kInstanceExtractFlags =
    static_cast<DWORD>(WTS_FORCEEXTRACTION | WTS_INSTANCESURROGATE);

// Why a way of extracting that the product's refresh refuses is left out.
constexpr char kSurrogateOnly[] =
    "the product only extracts in the thumbnail process (WTS_REQUIRESURROGATE)";

// Whether the product's refresh takes |extract| and |store| with its own question
// (AreSafeRefreshFlags): RefreshFiles refuses anything else before it asks the cache anything.
bool RefreshAllows(DWORD extract, std::optional<DWORD> store) {
  return pdft::AreSafeRefreshFlags(pdft::kRefreshProbeFlags, extract, store.value_or(0));
}

// The variants run (RefreshAllows), in kVariants' order.
std::vector<const Variant*> AllowedVariants() {
  std::vector<const Variant*> allowed;
  for (const Variant& variant : kVariants) {
    if (RefreshAllows(variant.extract, variant.store)) allowed.push_back(&variant);
  }
  return allowed;
}

// The labels of the variants left out, joined with ", "; empty when none is.
std::string LeftOutVariants() {
  std::string labels;
  for (const Variant& variant : kVariants) {
    if (RefreshAllows(variant.extract, variant.store)) continue;
    labels += (labels.empty() ? "" : ", ") + Utf8(variant.label);
  }
  return labels;
}

// What makes an open window show the new thumbnails, by eye: a round's notification, a step of
// the ladder, or associations changed after the update; the cheapest and least invasive first.
enum class Mechanism { kNone, kItem, kDir, kViews, kBrowse, kAssoc };

const char* MechanismText(Mechanism mechanism) {
  switch (mechanism) {
    case Mechanism::kNone:
      return "no notification";
    case Mechanism::kItem:
      return "SHCNE_UPDATEITEM per file";
    case Mechanism::kDir:
      return "SHCNE_UPDATEDIR per folder";
    case Mechanism::kViews:
      return "IShellView::Refresh of the views";
    case Mechanism::kBrowse:
      return "the views browsed to their folder again";
    case Mechanism::kAssoc:
      return "SHCNE_ASSOCCHANGED after the update";
  }
  return "?";
}

// The refresh's notification bits for |mechanism|; browsing again comes after a refresh that
// notifies nothing.
unsigned NotifyOf(Mechanism mechanism) {
  switch (mechanism) {
    case Mechanism::kItem:
      return pdft::kNotifyItems;
    case Mechanism::kDir:
      return pdft::kNotifyFolders;
    case Mechanism::kViews:
      return pdft::kRefreshViews;
    case Mechanism::kAssoc:
      return pdft::kNotifyAssociations;
    case Mechanism::kNone:
    case Mechanism::kBrowse:
      return 0;
  }
  return 0;
}

// printf into a string.
std::string Text(const char* format, ...) {
  va_list args;
  va_start(args, format);
  va_list again;
  va_copy(again, args);
  const int size = std::vsnprintf(nullptr, 0, format, args);
  va_end(args);
  std::string text(size > 0 ? static_cast<size_t>(size) : 0, '\0');
  if (size > 0) std::vsnprintf(text.data(), text.size() + 1, format, again);
  va_end(again);
  return text;
}

std::string Hr(HRESULT hr) { return Text("0x%08lX", static_cast<unsigned long>(hr)); }

// Application Control (Smart App Control) refused to start a program or to load the handler.
bool Blocked(HRESULT hr) {
  return pdft::IsLoadBlockedHresult(hr) || pdft::test::IsApplicationControlBlock(hr);
}

// ---- The zoom ---------------------------------------------------------------------------------

// The zoom saved before the experiment, and what puts it back on every way out: the end of the
// run, a failure or a stop, and the console handler (Ctrl+C, Ctrl+Break, closing the window).
// Signing out and shutting down never reach the handler: Windows sends them only to console
// processes that load neither user32.dll nor gdi32.dll, and thumbtest imports both, so those end
// it without notice and the printed command puts the zoom back.
struct ZoomGuard {
  std::mutex lock;
  bool changed = false;        // the saved zoom may differ from the one before, until put back
  bool closing = false;        // the console handler ran: no command-line executable starts now
  std::optional<DWORD> value;  // the REG_DWORD Zoom there was; none when there was no value
  HANDLE child = nullptr;      // the command-line executable while it changes the zoom
};
ZoomGuard g_zoomGuard;

// Puts the zoom back in this process (the caller holds g_zoomGuard.lock): the REG_DWORD as it
// was, or no value when there was none.
HRESULT PutZoomBackHere() noexcept {
  const HRESULT hr =
      g_zoomGuard.value
          ? pdft::SaveZoom(pdft::RegistryRoots{}, static_cast<int>(*g_zoomGuard.value))
          : pdft::DeleteRegistryValue(HKEY_CURRENT_USER, pdft::kRegKey, pdft::kZoomValueName);
  if (SUCCEEDED(hr)) g_zoomGuard.changed = false;
  return hr;
}

// The console handler: the zoom goes back before the process ends. The command-line executable
// changing it, which had the same Ctrl+C, is waited for first, so its zoom cannot land after,
// and none starts after this (the process ends when this returns).
BOOL WINAPI PutZoomBackOnExit(DWORD) noexcept {
  wil::unique_handle child;
  {
    std::lock_guard<std::mutex> held(g_zoomGuard.lock);
    g_zoomGuard.closing = true;
    if (!g_zoomGuard.changed) return FALSE;
    if (g_zoomGuard.child) {
      (void)DuplicateHandle(GetCurrentProcess(), g_zoomGuard.child, GetCurrentProcess(),
                            child.put(), SYNCHRONIZE, FALSE, 0);
    }
  }
  if (child) (void)WaitForSingleObject(child.get(), kChildWaitOnExitMs);
  std::lock_guard<std::mutex> held(g_zoomGuard.lock);
  if (g_zoomGuard.changed) {
    const HRESULT hr = PutZoomBackHere();
    if (SUCCEEDED(hr)) {
      std::printf("\nThe zoom is put back as it was before the experiment.\n");
    } else {
      std::printf(
          "\nThe zoom could not be put back (0x%08lX): run the command printed at the "
          "start.\n",
          static_cast<unsigned long>(hr));
    }
    std::fflush(stdout);
  }
  return FALSE;  // the default handler then ends the process
}

// ---- Programs ---------------------------------------------------------------------------------

// Runs |exe| with |arguments| and waits for it. Its standard output and error share one pipe,
// the only handle of this process it gets (no standard input), and are printed here, indented.
// With |zoomChild| the console handler knows it before it runs (it starts suspended), and it
// does not run at all once the handler has: ERROR_CANCELLED. *exitCode: its exit code.
HRESULT RunProgram(const std::filesystem::path& exe, const std::vector<std::wstring>& arguments,
                   bool zoomChild, DWORD* exitCode, std::string* output) {
  *exitCode = 0;
  std::wstring commandLine = pdft::test::QuoteArgument(exe.native());
  for (const std::wstring& argument : arguments) {
    commandLine += L" " + pdft::test::QuoteArgument(argument);
  }
  SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), nullptr, TRUE};
  wil::unique_handle readEnd, writeEnd;
  if (!CreatePipe(readEnd.put(), writeEnd.put(), &inheritable, 0) ||
      !SetHandleInformation(readEnd.get(), HANDLE_FLAG_INHERIT, 0)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  SIZE_T bytes = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
  std::vector<BYTE> buffer(bytes);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
  if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  const auto deleteList = wil::scope_exit([&] { DeleteProcThreadAttributeList(attributes); });
  HANDLE inherited[] = {writeEnd.get()};
  if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                 sizeof(inherited), nullptr, nullptr)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = writeEnd.get();
  startup.StartupInfo.hStdError = writeEnd.get();
  startup.lpAttributeList = attributes;
  wil::unique_process_information process;
  if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                      EXTENDED_STARTUPINFO_PRESENT | (zoomChild ? CREATE_SUSPENDED : 0), nullptr,
                      nullptr, &startup.StartupInfo, &process)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  writeEnd.reset();
  if (zoomChild) {
    bool closing = false;
    {
      std::lock_guard<std::mutex> held(g_zoomGuard.lock);
      closing = g_zoomGuard.closing;
      if (!closing) g_zoomGuard.child = process.hProcess;
    }
    DWORD error = ERROR_SUCCESS;
    if (closing) {
      error = ERROR_CANCELLED;
    } else if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
      error = GetLastError();
      if (error == ERROR_SUCCESS) error = ERROR_GEN_FAILURE;
    }
    if (error != ERROR_SUCCESS) {
      // It never ran, so it ends here: the handler has put the zoom back, or must not wait for it.
      (void)TerminateProcess(process.hProcess, error);
      std::lock_guard<std::mutex> held(g_zoomGuard.lock);
      g_zoomGuard.child = nullptr;
      return HRESULT_FROM_WIN32(error);
    }
  }
  std::string text;
  char chunk[4096];
  DWORD read = 0;
  while (ReadFile(readEnd.get(), chunk, sizeof(chunk), &read, nullptr) && read > 0) {
    text.append(chunk, read);
  }
  (void)WaitForSingleObject(process.hProcess, INFINITE);
  if (zoomChild) {
    std::lock_guard<std::mutex> held(g_zoomGuard.lock);
    g_zoomGuard.child = nullptr;
  }
  if (!GetExitCodeProcess(process.hProcess, exitCode)) return HRESULT_FROM_WIN32(GetLastError());
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(start, end - start);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::printf("    | %s\n", line.c_str());
    start = end + 1;
  }
  std::fflush(stdout);
  if (output) *output = std::move(text);
  return S_OK;
}

// Starts "thumbtest opendialog <folder>": a File Open dialog in a thumbtest process of its own.
HRESULT StartOpenDialog(const std::filesystem::path& self, const std::filesystem::path& folder,
                        wil::unique_process_information* process) {
  std::wstring commandLine = pdft::test::QuoteArgument(self.native()) + L" opendialog " +
                             pdft::test::QuoteArgument(folder.native());
  STARTUPINFOW startup = {sizeof(startup)};
  if (!CreateProcessW(self.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, nullptr, &startup, process)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  (void)AllowSetForegroundWindow(process->dwProcessId);
  return S_OK;
}

// ---- File Explorer ----------------------------------------------------------------------------

// An explorer.exe process: its pid and start time.
struct ExplorerProcess {
  DWORD pid = 0;
  ULONGLONG started = 0;  // the creation time as a FILETIME; 0 when it cannot be read
};

ULONGLONG ProcessStart(DWORD pid) {
  wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  FILETIME created = {}, exited = {}, kernel = {}, user = {};
  if (!process || !GetProcessTimes(process.get(), &created, &exited, &kernel, &user)) return 0;
  return (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

// The explorer.exe processes (by image name) in this session; std::nullopt when the processes
// cannot be listed.
std::optional<std::vector<ExplorerProcess>> ExplorersNow() {
  DWORD session = 0;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return std::nullopt;
  HANDLE rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (rawSnapshot == INVALID_HANDLE_VALUE) return std::nullopt;
  wil::unique_handle snapshot(rawSnapshot);
  PROCESSENTRY32W entry = {sizeof(entry)};
  std::vector<ExplorerProcess> found;
  for (BOOL more = Process32FirstW(snapshot.get(), &entry); more;
       more = Process32NextW(snapshot.get(), &entry)) {
    DWORD itsSession = 0;
    if (CompareStringOrdinal(entry.szExeFile, -1, L"explorer.exe", -1, TRUE) == CSTR_EQUAL &&
        ProcessIdToSessionId(entry.th32ProcessID, &itsSession) && itsSession == session) {
      found.push_back({entry.th32ProcessID, ProcessStart(entry.th32ProcessID)});
    }
  }
  return found;
}

// The classes of the visible top-level windows of the processes |pids| (class names only).
std::set<std::string> WindowClassesOf(const std::set<DWORD>& pids) {
  struct Search {
    const std::set<DWORD>* pids;
    std::set<std::string> classes;
  } search{&pids, {}};
  EnumWindows(
      [](HWND window, LPARAM parameter) -> BOOL {
        auto* s = reinterpret_cast<Search*>(parameter);
        DWORD pid = 0;
        if (!IsWindowVisible(window) || !GetWindowThreadProcessId(window, &pid) ||
            !s->pids->count(pid)) {
          return TRUE;
        }
        wchar_t name[256];
        const int length = GetClassNameW(window, name, ARRAYSIZE(name));
        try {
          if (length > 0) s->classes.insert(Utf8(std::wstring(name, length)));
        } catch (...) {
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&search));
  return search.classes;
}

// The file-system folder |view| shows; empty when it shows none or does not say.
std::wstring ViewFolder(IShellView* view) noexcept try {
  wil::com_ptr_nothrow<IFolderView> folderView;
  HRESULT hr = view->QueryInterface(IID_PPV_ARGS(folderView.put()));
  wil::com_ptr_nothrow<IPersistFolder2> folder;
  if (SUCCEEDED(hr)) hr = folderView->GetFolder(IID_PPV_ARGS(folder.put()));
  wil::unique_cotaskmem_ptr<ITEMIDLIST_ABSOLUTE> idList;
  if (SUCCEEDED(hr)) hr = folder->GetCurFolder(wil::out_param(idList));
  wil::unique_cotaskmem_string name;
  if (SUCCEEDED(hr) && idList)
    hr = SHGetNameFromIDList(idList.get(), SIGDN_FILESYSPATH, name.put());
  return SUCCEEDED(hr) && name ? std::wstring(name.get()) : std::wstring();
} catch (...) {
  return {};
}

// Calls |act| on each view that shows |folder| (ForEachViewShowing), after checking once more on
// the view itself that it does, so no other window is touched. *acted: on how many it succeeded.
HRESULT ActOnViewsOf(const std::filesystem::path& folder,
                     const std::function<HRESULT(IShellBrowser*, IShellView*)>& act,
                     size_t* acted) {
  // The action may still run on the watchdog's thread after a timeout: it owns what it uses.
  auto count = std::make_shared<std::atomic<size_t>>(0);
  const std::wstring wanted = folder.native();
  size_t found = 0;
  const HRESULT hr = pdft::ForEachViewShowing(
      std::span<const std::filesystem::path>(&folder, 1), false, kViewTimeoutMs,
      [wanted, act, count](IShellBrowser* browser, IShellView* view) {
        if (!pdft::SameImagePath(ViewFolder(view), wanted)) return;
        if (SUCCEEDED(act(browser, view))) ++*count;
      },
      &found);
  *acted = count->load();
  return hr;
}

// IShellView::Refresh of the views showing |folder|: F5, done by code.
HRESULT RefreshViewsOf(const std::filesystem::path& folder, size_t* refreshed) {
  return ActOnViewsOf(
      folder, [](IShellBrowser*, IShellView* view) { return view->Refresh(); }, refreshed);
}

// The views showing |folder| browsed to it again, in the same window.
HRESULT BrowseAgain(const std::filesystem::path& folder, size_t* browsed) {
  return ActOnViewsOf(
      folder,
      [](IShellBrowser* browser, IShellView* view) -> HRESULT {
        wil::com_ptr_nothrow<IFolderView> folderView;
        RETURN_IF_FAILED(view->QueryInterface(IID_PPV_ARGS(folderView.put())));
        wil::com_ptr_nothrow<IPersistFolder2> shown;
        RETURN_IF_FAILED(folderView->GetFolder(IID_PPV_ARGS(shown.put())));
        wil::unique_cotaskmem_ptr<ITEMIDLIST_ABSOLUTE> idList;
        RETURN_IF_FAILED(shown->GetCurFolder(wil::out_param(idList)));
        RETURN_HR_IF(E_UNEXPECTED, !idList);
        return browser->BrowseObject(idList.get(), SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
      },
      browsed);
}

// The thumbcache files in Windows Terminal's private copy of the user's Explorer folder (its
// packages' LocalCache), where a cache file made below a Terminal tab could land. Count only.
HRESULT CountTerminalCacheFiles(size_t* count) {
  *count = 0;
  wil::unique_cotaskmem_string localAppData;
  RETURN_IF_FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, nullptr,
                                        localAppData.put()));
  const std::filesystem::path local(localAppData.get());
  const std::filesystem::path packages = local / L"Packages";
  WIN32_FIND_DATAW data;
  HANDLE find =
      FindFirstFileExW((packages / L"Microsoft.WindowsTerminal*").c_str(), FindExInfoBasic, &data,
                       FindExSearchLimitToDirectories, nullptr, 0);
  if (find == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? S_OK
               : HRESULT_FROM_WIN32(error);
  }
  HRESULT result = S_OK;
  do {
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      continue;
    }
    std::vector<std::filesystem::path> files;
    const HRESULT hr = pdft::ListThumbnailCacheFiles(
        packages / data.cFileName / L"LocalCache\\Local\\Microsoft\\Windows\\Explorer", local,
        &files);
    if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    *count += files.size();
  } while (FindNextFileW(find, &data));
  FindClose(find);
  return result;
}

// ---- Files ------------------------------------------------------------------------------------

// A file the experiment wrote, and which file it is, so only that one is ever touched or deleted.
struct OwnFile {
  std::filesystem::path path;
  DWORD volume = 0;
  ULONGLONG index = 0;
};

// Deletes the open file |file| through its handle (POSIX semantics where the file system has
// them, else the classic way).
HRESULT DeleteThroughHandle(HANDLE file) {
  FILE_DISPOSITION_INFO_EX dispositionEx = {};
  dispositionEx.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
  if (SetFileInformationByHandle(file, FileDispositionInfoEx, &dispositionEx,
                                 sizeof(dispositionEx))) {
    return S_OK;
  }
  const DWORD error = GetLastError();
  if (error != ERROR_INVALID_PARAMETER && error != ERROR_NOT_SUPPORTED &&
      error != ERROR_INVALID_FUNCTION) {
    return HRESULT_FROM_WIN32(error);
  }
  FILE_DISPOSITION_INFO disposition = {};
  disposition.DeleteFile = TRUE;
  if (!SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition))) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  return S_OK;
}

// Writes |chunk| |repeat| times into the new file |path| (never over an existing one) and records
// which file it is. A file that cannot be written whole is deleted again.
HRESULT WriteOwnFile(const std::filesystem::path& path, std::string_view chunk, size_t repeat,
                     OwnFile* own) {
  wil::unique_hfile file(CreateFileW(
      path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  if (!file) return HRESULT_FROM_WIN32(GetLastError());
  for (size_t i = 0; i < repeat; ++i) {
    DWORD written = 0;
    if (!WriteFile(file.get(), chunk.data(), static_cast<DWORD>(chunk.size()), &written, nullptr) ||
        written != chunk.size()) {
      const DWORD error = GetLastError();
      (void)DeleteThroughHandle(file.get());
      return error ? HRESULT_FROM_WIN32(error) : HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }
  }
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!GetFileInformationByHandle(file.get(), &info)) {
    const DWORD error = GetLastError();
    (void)DeleteThroughHandle(file.get());
    return HRESULT_FROM_WIN32(error);
  }
  own->path = path;
  own->volume = info.dwVolumeSerialNumber;
  own->index = (static_cast<ULONGLONG>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  return S_OK;
}

// Opens |own| by its name (never through a link) with |access|, only while the name still leads
// to the file written: S_FALSE (no handle) when it is gone or is another file now.
HRESULT OpenOwnFile(const OwnFile& own, DWORD access, wil::unique_hfile* file) {
  file->reset(CreateFileW(own.path.c_str(), access | FILE_READ_ATTRIBUTES,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING,
                          FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS |
                              SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
                          nullptr));
  if (!*file) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? S_FALSE
               : HRESULT_FROM_WIN32(error);
  }
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!GetFileInformationByHandle(file->get(), &info)) {
    const DWORD error = GetLastError();
    file->reset();
    return HRESULT_FROM_WIN32(error);
  }
  const ULONGLONG index = (static_cast<ULONGLONG>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
  if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
      info.dwVolumeSerialNumber != own.volume || index != own.index) {
    file->reset();
    return S_FALSE;
  }
  return S_OK;
}

// ---- Answers ----------------------------------------------------------------------------------

enum class Answer { kYes, kNo, kStop };

// One line from standard input, trimmed and in lower case; std::nullopt at the end of the input.
std::optional<std::string> ReadAnswer() {
  std::string text;
  char chunk[256];
  for (;;) {
    if (!std::fgets(chunk, sizeof(chunk), stdin)) {
      if (text.empty()) return std::nullopt;
      break;
    }
    if (text.size() < 256) text += chunk;
    if (std::strchr(chunk, '\n')) break;
  }
  const size_t first = text.find_first_not_of(" \t\r\n");
  const size_t last = text.find_last_not_of(" \t\r\n");
  text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// Asks until the answer is y or n; q, or the end of the input, stops the experiment.
Answer AskYesNo(const std::string& question) {
  for (;;) {
    std::printf("%s [y/n, q stops] ", question.c_str());
    std::fflush(stdout);
    const std::optional<std::string> answer = ReadAnswer();
    if (!answer || *answer == "q") return Answer::kStop;
    if (*answer == "y" || *answer == "yes") return Answer::kYes;
    if (*answer == "n" || *answer == "no") return Answer::kNo;
  }
}

// Waits for Enter; false for q or the end of the input.
bool WaitForEnter(const std::string& prompt) {
  std::printf("%s (q stops) ", prompt.c_str());
  std::fflush(stdout);
  const std::optional<std::string> answer = ReadAnswer();
  return answer && *answer != "q";
}

// A whole number from 0 to 999; std::nullopt for q or the end of the input.
std::optional<long> AskCount(const std::string& question) {
  for (;;) {
    std::printf("%s (q stops) ", question.c_str());
    std::fflush(stdout);
    const std::optional<std::string> answer = ReadAnswer();
    if (!answer || *answer == "q") return std::nullopt;
    long count = 0;
    if (ParseNumber(std::wstring(answer->begin(), answer->end()), 0, 999, &count)) return count;
  }
}

// An exit code the user reads off and types, a whole number (a minus sign allowed): true with
// *code, or *code empty for ?; false for q or the end of the input.
bool AskExitCode(std::optional<long>* code) {
  for (;;) {
    std::printf("Type the number after exit= and press Enter (? if you cannot tell, q stops). ");
    std::fflush(stdout);
    const std::optional<std::string> answer = ReadAnswer();
    if (!answer || *answer == "q") return false;
    if (*answer == "?") {
      code->reset();
      return true;
    }
    const bool negative = answer->starts_with('-');
    const std::wstring digits(answer->begin() + (negative ? 1 : 0), answer->end());
    long value = 0;
    if (ParseNumber(digits, 0, LONG_MAX, &value)) {
      *code = negative ? -value : value;
      return true;
    }
  }
}

// After a change made at |start| (PreciseMs), times the answer: Enter as soon as the thumbnails
// turn square (*seconds since |start|), n when nothing changed within 10 seconds.
Answer TimeSquare(const std::string& where, ULONGLONG start, double* seconds) {
  for (;;) {
    std::printf(
        "Press Enter as soon as the thumbnails %s turn square; type n and Enter if nothing "
        "changes within 10 seconds (q stops). ",
        where.c_str());
    std::fflush(stdout);
    const std::optional<std::string> answer = ReadAnswer();
    const ULONGLONG now = PreciseMs();
    if (!answer || *answer == "q") return Answer::kStop;
    if (answer->empty()) {
      *seconds = static_cast<double>(now - start) / 1000.0;
      return Answer::kYes;
    }
    if (*answer == "n" || *answer == "no") return Answer::kNo;
  }
}

// ---- Measuring --------------------------------------------------------------------------------

// The cache, timing every question. The refresh times its extractions itself
// (RefreshFilesOptions::extractionMs), an extraction and the call that stores it as one.
class TimedCache final : public pdft::ThumbnailCacheOps {
 public:
  explicit TimedCache(pdft::ThumbnailCacheOps& cache) : cache_(cache) {}

  HRESULT Select(const std::filesystem::path& file) noexcept override {
    return cache_.Select(file);
  }

  HRESULT Get(UINT size, DWORD flags, pdft::CacheAnswer* answer) noexcept override {
    if (!(flags & WTS_INCACHEONLY)) return cache_.Get(size, flags, answer);
    const ULONGLONG start = PreciseMs();
    const HRESULT hr = cache_.Get(size, flags, answer);
    try {
      questions.push_back(static_cast<DWORD>(PreciseMs() - start));
    } catch (...) {
      // A time not kept changes no answer.
    }
    return hr;
  }

  std::vector<DWORD> questions;

 private:
  pdft::ThumbnailCacheOps& cache_;
};

// The cache, watching for Application Control: its first block asks the refresh to stop (between
// two requests), so no further request shows the user another notification. With |made| it also
// keeps every extraction's answer (each call without WTS_INCACHEONLY) as a record of the selected
// file, keyed as AskCache keys it.
class BlockWatch final : public pdft::ThumbnailCacheOps {
 public:
  BlockWatch(pdft::ThumbnailCacheOps& cache, std::stop_source stop,
             std::vector<CacheRecord>* made = nullptr)
      : cache_(cache), stop_(std::move(stop)), made_(made) {}

  HRESULT Select(const std::filesystem::path& file) noexcept override {
    if (made_) {
      try {
        selected_ = file;
      } catch (...) {
        selected_.clear();
      }
    }
    return Watch(cache_.Select(file));
  }

  HRESULT Get(UINT size, DWORD flags, pdft::CacheAnswer* answer) noexcept override {
    const HRESULT hr = Watch(cache_.Get(size, flags, answer));
    if (made_ && !(flags & WTS_INCACHEONLY)) Keep(size, hr, *answer);
    return hr;
  }

  HRESULT blocked = S_OK;  // the first block

 private:
  HRESULT Watch(HRESULT hr) noexcept {
    if (Blocked(hr) && SUCCEEDED(blocked)) {
      blocked = hr;
      stop_.request_stop();
    }
    return hr;
  }

  void Keep(UINT size, HRESULT hr, const pdft::CacheAnswer& answer) noexcept {
    try {
      CacheRecord record;
      record.label = selected_.parent_path().filename().native();
      record.key = record.label + L"/" + selected_.filename().native();
      record.size = size;
      record.hr = hr;
      if (answer.bitmap) {
        record.w = static_cast<int>(answer.dims.cx);
        record.h = static_cast<int>(answer.dims.cy);
      }
      record.cacheFlags = answer.cacheFlags;
      record.id = answer.id;
      record.cached = hr == S_OK && answer.bitmap && (answer.cacheFlags & WTS_CACHED);
      made_->push_back(std::move(record));
    } catch (...) {
      // An answer not kept counts as no extraction: the judge fails its entry.
    }
  }

  pdft::ThumbnailCacheOps& cache_;
  std::stop_source stop_;
  std::vector<CacheRecord>* made_;
  std::filesystem::path selected_;
};

// One thumbnail made again and again without being stored, roughly File Explorer's own work, on a
// thread of its own with a cache object of its own: how long it waits while a refresh runs.
class ContentionProbe {
 public:
  explicit ContentionProbe(std::filesystem::path file) : file_(std::move(file)) {}

  void Start() {
    thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
  }

  // Stops it; how long each extraction took. *failed counts those that failed (and a cache that
  // could not be made).
  std::vector<DWORD> Stop(size_t* failed) {
    thread_.request_stop();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> held(lock_);
    *failed = failed_;
    return samples_;
  }

 private:
  void Run(std::stop_token stop) noexcept {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
      std::lock_guard<std::mutex> held(lock_);
      ++failed_;
      return;
    }
    {
      std::unique_ptr<pdft::ThumbnailCacheOps> cache;
      if (FAILED(pdft::MakeThumbnailCacheOps(&cache)) || FAILED(cache->Select(file_))) {
        std::lock_guard<std::mutex> held(lock_);
        ++failed_;
      } else {
        std::mutex waitLock;
        std::condition_variable_any wake;
        while (!stop.stop_requested()) {
          const ULONGLONG start = PreciseMs();
          pdft::CacheAnswer answer;
          const HRESULT hr = cache->Get(kContentionSize, kNoCacheFlags, &answer);
          const ULONGLONG end = PreciseMs();
          {
            std::lock_guard<std::mutex> held(lock_);
            try {
              samples_.push_back(static_cast<DWORD>(end - start));
            } catch (...) {
              // A sample not kept changes nothing else.
            }
            if (FAILED(hr)) ++failed_;
          }
          // Each further request would show the user another notification.
          if (Blocked(hr)) break;
          if (end < start + kContentionEveryMs) {
            std::unique_lock<std::mutex> held(waitLock);
            (void)wake.wait_for(held, stop,
                                std::chrono::milliseconds(start + kContentionEveryMs - end),
                                [] { return false; });
          }
        }
      }
    }
    CoUninitialize();
  }

  const std::filesystem::path file_;
  std::mutex lock_;
  std::vector<DWORD> samples_;
  size_t failed_ = 0;
  std::jthread thread_;  // last: stopped and joined before the rest goes
};

double Mean(const std::vector<DWORD>& times) {
  if (times.empty()) return 0;
  ULONGLONG total = 0;
  for (const DWORD ms : times) total += ms;
  return static_cast<double>(total) / static_cast<double>(times.size());
}

// RefreshFiles asked to stop |afterMs| after it starts, by a timer thread, or at Application
// Control's first block (*blocked). *asked: when the timer asked (PreciseMs; 0 when the refresh
// had ended before), *returned: when it returned.
HRESULT RefreshStoppedAfter(std::span<const std::filesystem::path> files,
                            const pdft::RefreshFilesOptions& options,
                            pdft::ThumbnailCacheOps& cache, DWORD afterMs,
                            pdft::RefreshCounts* counts, ULONGLONG* asked, ULONGLONG* returned,
                            HRESULT* blocked) {
  std::stop_source stop;
  BlockWatch watched(cache, stop);
  std::atomic<ULONGLONG> askedAt{0};
  std::jthread timer([&stop, &askedAt, afterMs](std::stop_token own) {
    std::mutex lock;
    std::condition_variable_any wake;
    std::unique_lock<std::mutex> held(lock);
    (void)wake.wait_for(held, own, std::chrono::milliseconds(afterMs), [] { return false; });
    if (own.stop_requested()) return;
    askedAt = PreciseMs();
    stop.request_stop();
  });
  const HRESULT hr = pdft::RefreshFiles(files, options, watched, pdft::RealShellNotifier(), nullptr,
                                        stop.get_token(), counts);
  *returned = PreciseMs();
  timer.request_stop();
  timer.join();
  *asked = askedAt;
  *blocked = watched.blocked;
  return hr;
}

// The records of |labels| only.
std::vector<CacheRecord> Only(std::span<const CacheRecord> records,
                              std::span<const std::wstring> labels) {
  std::vector<CacheRecord> kept;
  for (const CacheRecord& record : records) {
    if (std::find(labels.begin(), labels.end(), record.label) != labels.end()) {
      kept.push_back(record);
    }
  }
  return kept;
}

// Whether |record| is of one of the slow folder's largest pages.
bool IsLargest(const CacheRecord& record) {
  const std::wstring_view key = record.key;
  return key.substr(key.find(L'/') + 1).starts_with(kLargestPrefix);
}

const FolderVerdict* VerdictOf(const std::vector<FolderVerdict>& verdicts,
                               const std::wstring& label) {
  for (const FolderVerdict& verdict : verdicts) {
    if (verdict.label == label) return &verdict;
  }
  return nullptr;
}

std::string CountsText(HRESULT hr, const pdft::RefreshCounts& counts) {
  std::string text =
      Text("%zu of %zu cached thumbnails made again in %zu files (%zu extractions), %zu failed",
           counts.refreshed, counts.entriesCached, counts.filesRefreshed, counts.extracted,
           counts.failed);
  if (counts.failed) text += " (first " + Hr(counts.firstFailure) + ")";
  if (counts.superseded) text += Text(", %zu gone after a larger size's store", counts.superseded);
  if (counts.otherSize) text += Text(", %zu answered from another size", counts.otherSize);
  if (!counts.sizesDropped.empty()) {
    text += "; sizes refused:";
    for (const UINT size : counts.sizesDropped) text += Text(" %u", size);
  }
  if (hr != S_OK) text += "; result " + Hr(hr);
  return text;
}

// Edges (the longer side of bitmaps) as "256" or "181/256"; "-" for none.
std::string EdgesText(const std::set<int>& edges) {
  std::string text;
  for (const int edge : edges) text += (text.empty() ? "" : "/") + std::to_string(edge);
  return text.empty() ? "-" : text;
}

// "S_OK", or the HRESULT in hexadecimal.
std::string HrOrOk(HRESULT hr) { return hr == S_OK ? "S_OK" : Hr(hr); }

// A bitmap's size as "181x256"; "no bitmap" without one.
std::string BitmapText(const pdft::CacheAnswer& answer) {
  return answer.bitmap ? Text("%ldx%ld", answer.dims.cx, answer.dims.cy) : "no bitmap";
}

// Why an answer is not held, for the "not held:" lines: a miss's HRESULT, else its class.
std::string WhyNotHeld(const CacheRecord& record) {
  const pdft::CacheHit hit = HitOf(record);
  return hit == pdft::CacheHit::kNotCached ? Hr(record.hr) : HitText(hit);
}

// A file of some records with answers that are not held.
struct NotHeldFile {
  std::wstring key;
  std::string sizes;             // "every size" when none of its answers is held, or "1920, 2560"
  std::vector<std::string> why;  // WhyNotHeld of those answers, each once
};

// The files of |records| (in the order they first appear) with an answer not held.
std::vector<NotHeldFile> NotHeldFiles(std::span<const CacheRecord> records) {
  struct Seen {
    NotHeldFile file;
    size_t asked = 0;
    size_t missed = 0;
  };
  std::vector<Seen> seen;  // in the order the files first appear
  std::map<std::wstring, size_t> at;
  for (const CacheRecord& record : records) {
    const auto [found, added] = at.try_emplace(record.key, seen.size());
    if (added) seen.push_back({NotHeldFile{record.key, {}, {}}});
    Seen& file = seen[found->second];
    ++file.asked;
    if (record.cached) continue;
    ++file.missed;
    file.file.sizes += (file.file.sizes.empty() ? "" : ", ") + std::to_string(record.size);
    const std::string why = WhyNotHeld(record);
    std::vector<std::string>& whys = file.file.why;
    if (std::find(whys.begin(), whys.end(), why) == whys.end()) whys.push_back(why);
  }
  std::vector<NotHeldFile> files;
  for (Seen& file : seen) {
    if (file.missed == 0) continue;
    if (file.missed == file.asked) file.file.sizes = "every size";
    files.push_back(std::move(file.file));
  }
  return files;
}

// The names of the files |records| does not hold, grouped by the sizes: "portrait-001.pdf,
// portrait-002.pdf at every size; landscape-001.pdf at 1920, 2560".
std::string NotHeldNames(std::span<const CacheRecord> records) {
  std::vector<std::pair<std::string, std::string>> groups;  // the sizes, then the names
  for (const NotHeldFile& file : NotHeldFiles(records)) {
    const std::wstring_view key = file.key;
    const std::string name = Utf8(std::wstring(key.substr(key.find(L'/') + 1)));
    const auto group = std::find_if(groups.begin(), groups.end(),
                                    [&file](const auto& g) { return g.first == file.sizes; });
    if (group == groups.end()) {
      groups.emplace_back(file.sizes, name);
    } else {
      group->second += ", " + name;
    }
  }
  std::string text;
  for (const auto& [sizes, names] : groups) {
    text += (text.empty() ? "" : "; ") + names + " at " + sizes;
  }
  return text;
}

// The file name in |key| ("<label>/<file name>").
std::string NameOf(const std::wstring& key) {
  return Utf8(key.substr(std::min(key.find(L'/') + 1, key.size())));
}

// The names of the files |keys| name, as "portrait-001.pdf, landscape-002.pdf" (with |labels|,
// the keys themselves), with |withAnd| "... and landscape-002.pdf"; "none" for none.
std::string NamesText(std::span<const std::wstring> keys, bool labels = false,
                      bool withAnd = false) {
  std::string text;
  for (size_t i = 0; i < keys.size(); ++i) {
    if (i > 0) text += withAnd && i + 1 == keys.size() ? " and " : ", ";
    text += labels ? Utf8(keys[i]) : NameOf(keys[i]);
  }
  return text.empty() ? "none" : text;
}

// The keys of the files of |records| with at least one held answer, in the order they first
// appear.
std::vector<std::wstring> HeldFiles(std::span<const CacheRecord> records) {
  std::vector<std::wstring> keys;
  for (const CacheRecord& record : records) {
    if (record.cached && std::find(keys.begin(), keys.end(), record.key) == keys.end()) {
      keys.push_back(record.key);
    }
  }
  return keys;
}

// The keys of |earlier|'s files with a held answer of which |now| holds none any more: dropped
// whole in between.
std::vector<std::wstring> DroppedBetween(std::span<const CacheRecord> earlier,
                                         std::span<const CacheRecord> now) {
  const std::vector<std::wstring> held = HeldFiles(now);
  std::vector<std::wstring> dropped;
  for (const std::wstring& key : HeldFiles(earlier)) {
    if (std::find(held.begin(), held.end(), key) == held.end()) dropped.push_back(key);
  }
  return dropped;
}

// "20261104-201530" (UTC).
std::wstring UtcStamp() {
  SYSTEMTIME now = {};
  GetSystemTime(&now);
  wchar_t text[32];
  std::swprintf(text, ARRAYSIZE(text), L"%04u%02u%02u-%02u%02u%02u", now.wYear, now.wMonth,
                now.wDay, now.wHour, now.wMinute, now.wSecond);
  return text;
}

// ---- The experiment ---------------------------------------------------------------------------

struct Options {
  std::filesystem::path self;  // thumbtest.exe
  std::filesystem::path cli;   // the command-line executable that sets the zoom
  std::optional<std::filesystem::path> root;
  bool interactive = true;
  bool keep = false;
  long timingFiles = kDefaultTimingFiles;
  long instanceFiles = kInstanceFiles;
  bool desktop = false;
  bool copyCheck = false;
};

// A round judged by eye.
struct Round {
  bool run = false;     // it was shown
  bool valid = false;   // the folder showed the old crop first
  bool square = false;  // ... and turned square within kSquareWithinSeconds
  std::optional<double> seconds;
  // The files to look at, held and old just before at every size they were judged at, and those
  // entries.
  std::vector<std::filesystem::path> look;
  std::vector<CacheRecord> entries;
};

class Experiment {
 public:
  explicit Experiment(Options options) : options_(std::move(options)) {}

  int Run() {
    started_ = PreciseMs();
    if (const std::optional<int> refused = PreFlight()) return *refused;
    // From here the zoom may change: the console handler puts it back on Ctrl+C and the like.
    SetConsoleCtrlHandler(PutZoomBackOnExit, TRUE);
    bool go = MakeFiles() && SetZoom(kBaselineZoom) && Populate() && FlagAnswers() &&
              CheckBaseline() && Flip() && JudgeVariants();
    if (go && best_) {
      if (options_.interactive) go = Rounds() && DesktopAndDialog();
      if (go) go = Timing();
      if (go && options_.interactive) go = ProductCheck();
    } else if (go && !options_.interactive) {
      // Without the user the timing fill runs all the same: whether many new thumbnails make the
      // cache drop others is worth knowing whichever variant passed.
      go = Timing();
    }
    if (go && options_.copyCheck) go = CopyCheck();
    return Finish();
  }

 private:
  // ---- Helpers ----

  void Say(const std::string& line) {
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    lines_.push_back(line);
  }

  void Note(const std::string& line) { lines_.push_back(line); }

  // The start of a phase, with the seconds since the run started.
  void Phase(const std::string& what) {
    Say(Text("elapsed %llu s: %s", (PreciseMs() - started_) / 1000, what.c_str()));
  }

  // Stops the run: what is left is the end (Finish). False, for the step's return.
  bool Stop(const std::string& why) {
    if (stop_.empty()) stop_ = why;
    Say("STOPPED: " + why);
    return false;
  }

  bool StopByUser() {
    stoppedByUser_ = true;
    return Stop("stopped at a question");
  }

  std::filesystem::path Folder(const std::wstring& label) const { return root_ / label; }

  const std::vector<std::filesystem::path>& FilesOf(const std::wstring& label) {
    return files_[label];
  }

  bool Excluded(const std::wstring& label) const { return excluded_.count(label) > 0; }

  // Writes |text| as |name| into the report folder.
  void SaveText(const std::wstring& name, const std::string& text) {
    if (!WriteText((reportDir_ / name).native(), text)) {
      Say("cannot write " + Utf8(name) + " into the report folder");
    }
  }

  void SaveRecords(const std::wstring& name, std::span<const CacheRecord> records) {
    if (!pdft::test::WriteRecords(reportDir_ / name, records)) {
      Say("cannot write " + Utf8(name) + " into the report folder");
    }
  }

  // AskCache keys a file by its folder's name: the Desktop file's records go by its label
  // instead, as the fill keys them, so that no folder of the user's is named.
  void LabelDesktop(std::vector<CacheRecord>* records) const {
    if (!desktopFile_) return;
    const std::wstring asked =
        desktopFile_->parent_path().filename().native() + L"/" + kDesktopFileName;
    for (CacheRecord& record : *records) {
      if (record.key != asked) continue;
      record.label = kDesktopLabel;
      record.key = std::wstring(kDesktopLabel) + L"/" + kDesktopFileName;
    }
  }

  // The product's question about |files| at sizes_ (AskCache). An answer neither held nor
  // refused is asked again after 2 s, and again 10 s later, and a held answer then takes its
  // place; |first| (when set) sees the first answers before that, and a line says each time how
  // many of |what|'s answers are held now. *again: how many were held only when asked again. An
  // entry is held only through a held answer, and its record is that answer. The |baseline| asks
  // every such answer again; a later call only once asking again has found an entry at the
  // baseline, and then only for files partly held in the first answers (a file the cache dropped
  // whole does not come back).
  std::vector<CacheRecord> AskHeld(std::span<const std::filesystem::path> files,
                                   const std::string& what,
                                   const std::function<void(std::span<const CacheRecord>)>& first,
                                   size_t* again, bool* unopened, bool baseline = false) {
    std::vector<CacheRecord> records =
        AskCache(*cache_, files, sizes_, pdft::kRefreshProbeFlags, std::nullopt, false, unopened);
    LabelDesktop(&records);
    if (first) first(records);
    *again = 0;
    if (!baseline && !askAgain_) return records;
    const auto pending = [](const CacheRecord& record) {
      const pdft::CacheHit hit = HitOf(record);
      return hit != pdft::CacheHit::kHeld && hit != pdft::CacheHit::kRefused;
    };
    // AskCache's records: each file at each of sizes_, in order.
    const size_t perFile = sizes_.size();
    std::vector<bool> askFile(files.size(), false);
    for (size_t f = 0; f < files.size(); ++f) {
      bool held = false, missing = false;
      for (size_t s = 0; s < perFile && f * perFile + s < records.size(); ++s) {
        const CacheRecord& record = records[f * perFile + s];
        if (record.cached) held = true;
        if (pending(record)) missing = true;
      }
      askFile[f] = missing && (baseline || held);
    }
    for (const DWORD waitMs : kAskAgainMs) {
      size_t asked = 0;
      for (size_t f = 0; f < files.size(); ++f) {
        for (size_t s = 0; askFile[f] && s < perFile && f * perFile + s < records.size(); ++s) {
          if (pending(records[f * perFile + s])) ++asked;
        }
      }
      if (asked == 0) break;
      Sleep(waitMs);
      size_t heldNow = 0;
      for (size_t f = 0; f < files.size(); ++f) {
        if (!askFile[f]) continue;
        std::vector<UINT> sizes;
        std::vector<size_t> at;
        for (size_t s = 0; s < perFile && f * perFile + s < records.size(); ++s) {
          if (!pending(records[f * perFile + s])) continue;
          sizes.push_back(sizes_[s]);
          at.push_back(f * perFile + s);
        }
        if (sizes.empty()) continue;
        std::vector<CacheRecord> now =
            AskCache(*cache_, files.subspan(f, 1), sizes, pdft::kRefreshProbeFlags, std::nullopt,
                     false, unopened);
        LabelDesktop(&now);
        for (size_t k = 0; k < now.size() && k < at.size(); ++k) {
          if (!now[k].cached) continue;
          records[at[k]] = std::move(now[k]);
          ++heldNow;
        }
      }
      *again += heldNow;
      Say(Text("asked again after %lu s: %zu of %zu held now (%s)", waitMs / 1000, heldNow, asked,
               what.c_str()));
    }
    if (baseline && *again > 0) askAgain_ = true;
    return records;
  }

  // Per size of sizes_, how |records| answered, as "<what> size  256: 48 asked: 48 held, 0 a
  // larger size's, 0 a smaller size's, 0 not cached, 0 refused; edges 181/256", the misses by
  // HRESULT, and S_OK answers that are no cached thumbnail counted when there are any.
  void SayAnswers(const std::string& what, std::span<const CacheRecord> records) {
    for (const UINT size : sizes_) {
      size_t asked = 0, withoutCached = 0, withoutBitmap = 0;
      std::map<pdft::CacheHit, size_t> hits;
      std::map<HRESULT, size_t> misses;
      std::set<int> edges;
      for (const CacheRecord& record : records) {
        if (record.size != size) continue;
        ++asked;
        const pdft::CacheHit hit = HitOf(record);
        ++hits[hit];
        const bool bitmap = record.w > 0 || record.h > 0;
        if (bitmap) edges.insert(std::max(record.w, record.h));
        if (hit != pdft::CacheHit::kNotCached) continue;
        ++misses[record.hr];
        if (record.hr == S_OK && !bitmap) ++withoutBitmap;
        if (record.hr == S_OK && bitmap) ++withoutCached;
      }
      std::string byHr;
      for (const auto& [hr, count] : misses) {
        byHr += (byHr.empty() ? " (by hr: " : ", ") + Hr(hr) + Text(" x%zu", count);
      }
      if (!byHr.empty()) byHr += ")";
      std::string text = Text(
          "%s size %4u: %zu asked: %zu held, %zu a larger size's, %zu a smaller size's, %zu "
          "not cached%s, %zu refused; edges %s",
          what.c_str(), size, asked, hits[pdft::CacheHit::kHeld], hits[pdft::CacheHit::kLarger],
          hits[pdft::CacheHit::kSmaller], hits[pdft::CacheHit::kNotCached], byHr.c_str(),
          hits[pdft::CacheHit::kRefused], EdgesText(edges).c_str());
      if (withoutCached) text += Text("; S_OK without WTS_CACHED x%zu", withoutCached);
      if (withoutBitmap) text += Text("; S_OK without a bitmap x%zu", withoutBitmap);
      Say(text);
    }
  }

  // One line per file of |records| with an answer not held: "not held: <label>/<file> at every
  // size (<why>)", or at the sizes it missed.
  void SayNotHeld(std::span<const CacheRecord> records) {
    for (const NotHeldFile& file : NotHeldFiles(records)) {
      std::string why;
      for (const std::string& each : file.why) why += (why.empty() ? "" : ", ") + each;
      Say("not held: " + Utf8(file.key) + " at " + file.sizes + " (" + why + ")");
    }
  }

  // The best variant's extraction, the experiment's sizes and the product's question, with
  // |notify|.
  pdft::RefreshFilesOptions BestOptions(unsigned notify) const {
    pdft::RefreshFilesOptions options;
    options.sizes = sizes_;
    options.probeFlags = pdft::kRefreshProbeFlags;
    options.extractFlags = best_ ? best_->extract : pdft::kRefreshExtractFlags;
    options.storeFlags = best_ ? best_->store : std::nullopt;
    options.notify = notify;
    options.clock = PreciseMs;
    return options;
  }

  // RefreshFiles with the real notifications; false (stopped) when Application Control blocked
  // the handler, which also stops the refresh at once. |made| (when set) receives its extraction
  // answers (BlockWatch).
  bool Refresh(std::span<const std::filesystem::path> files,
               const pdft::RefreshFilesOptions& options, pdft::ThumbnailCacheOps& cache,
               pdft::RefreshCounts* counts, HRESULT* result,
               std::vector<CacheRecord>* made = nullptr) {
    *counts = {};
    std::stop_source stop;
    BlockWatch watched(cache, stop, made);
    *result = pdft::RefreshFiles(files, options, watched, pdft::RealShellNotifier(), nullptr,
                                 stop.get_token(), counts);
    return StopIfBlocked(watched.blocked);
  }

  // False (stopped) when |blocked| is Application Control's block.
  bool StopIfBlocked(HRESULT blocked) {
    if (SUCCEEDED(blocked)) return true;
    return Stop("Application Control blocked the handler (" + Hr(blocked) +
                "): stopped at the first block");
  }

  bool MakeFolder(const std::filesystem::path& folder) {
    if (!CreateDirectoryW(folder.c_str(), nullptr)) {
      return Stop(Text("cannot create the folder %s (error %lu)",
                       Utf8(folder.filename().native()).c_str(), GetLastError()));
    }
    ownDirs_.push_back(folder);
    return true;
  }

  bool MakeFile(const std::wstring& label, const std::filesystem::path& path,
                std::string_view bytes) {
    OwnFile own;
    const HRESULT hr = WriteOwnFile(path, bytes, 1, &own);
    if (FAILED(hr)) {
      return Stop("cannot write " + Utf8(label) + "/" + Utf8(path.filename().native()) + ": " +
                  Hr(hr));
    }
    ownFiles_.push_back(std::move(own));
    files_[label].push_back(path);
    return true;
  }

  // ---- Pre-flight (read-only) ----

  // Nothing changes here: std::nullopt to go on, else the exit code.
  std::optional<int> PreFlight() {
    pdft::ProcessContext context;
    if (const std::optional<int> refused = Refusal(&context)) return refused;
    if (options_.interactive) {
      if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_CHAR) {
        std::printf(
            "refused: the experiment asks its questions on the console, and standard input is not "
            "one: run it from a Command Prompt, or with --non-interactive.\n");
        return 2;
      }
      if (!GetShellWindow()) {
        std::printf("refused: there is no File Explorer on this desktop to look at.\n");
        return kRefused;
      }
      if (context.differentFromShellUser) {
        std::printf(
            "refused: File Explorer on this desktop is another account's, and the thumbnail cache\n"
            "is each account's own: run the experiment as the signed-in user.\n");
        return kRefused;
      }
    }
    otherShellUser_ = context.differentFromShellUser;

    // The zoom saved now goes back at the end.
    pdft::Settings settings;
    pdft::ZoomDetails details;
    HRESULT hr = pdft::LoadSettings(pdft::RegistryRoots{}, &settings, &details);
    if (FAILED(hr)) {
      std::printf("cannot read the zoom: 0x%08lX\n", static_cast<unsigned long>(hr));
      return 2;
    }
    if (pdft::IsZoomValueInvalid(details.decision.registry)) {
      std::printf(
          "refused: the saved Zoom value is not one the settings app writes, so it could not be\n"
          "put back as it is: set the zoom with --zoom <percent> first.\n");
      return 2;
    }
    savedZoom_ = details.decision.zoom;
    g_zoomGuard.value = details.decision.registry == pdft::ZoomValueState::kValid
                            ? details.registryRaw
                            : std::optional<DWORD>();
    std::printf("If this stops early, put your zoom back with:\n");
    if (g_zoomGuard.value) {
      std::printf("  \"%s\" --zoom %lu --no-refresh\n", Utf8(options_.cli.native()).c_str(),
                  *g_zoomGuard.value);
    } else {
      std::printf("  reg delete \"HKCU\\%s\" /v %s /f\n", Utf8(pdft::kRegKey).c_str(),
                  Utf8(pdft::kZoomValueName).c_str());
    }
    std::printf("(Ctrl+C and closing this window put it back by themselves.)\n\n");

    // Smart App Control: each block shows the user a notification.
    pdft::SacReading sac;
    if (SUCCEEDED(pdft::ReadSacState(pdft::RegistryRoots{}, &sac)) &&
        (sac.state == pdft::SacState::kOn || sac.state == pdft::SacState::kEvaluation)) {
      if (!options_.interactive) {
        std::printf(
            "refused: Smart App Control is on, and every block it makes shows a notification: run\n"
            "the experiment with someone at the PC (without --non-interactive).\n");
        return 2;
      }
      std::printf(
          "Smart App Control is on: it may block the test builds, and each block shows a\n"
          "notification. The experiment stops at the first block.\n");
      const Answer go = AskYesNo("Go on anyway?");
      if (go != Answer::kYes) {
        std::printf("Nothing changed.\n");
        return 3;
      }
      sacOn_ = true;
    }

    // The handler Windows uses for .pdf thumbnails must be this product's.
    std::optional<std::wstring> clsid;
    hr = pdft::QueryEffectiveHandler(pdft::SystemAssociationQueries(), &clsid);
    if (FAILED(hr) || !clsid || !pdft::IsOurClsid(*clsid)) {
      std::printf(
          "cannot run: Windows does not use %s for .pdf thumbnails here, so the experiment would\n"
          "test another handler: install it (and --fix --take-back) first.\n",
          Utf8(pdft::kProductName).c_str());
      return 2;
    }
    pdft::ExplorerSettings explorer;
    hr = pdft::ReadExplorerSettings(pdft::RegistryRoots{}, &explorer);
    thumbnailsOff_ = SUCCEEDED(hr) && (explorer.iconsOnly || explorer.thumbnailsOffByUserPolicy ||
                                       explorer.thumbnailsOffByMachinePolicy);
    if (thumbnailsOff_ && options_.interactive) {
      std::printf(
          "cannot run: File Explorer shows icons only (Folder Options, or a policy), so no window\n"
          "could show a thumbnail.\n");
      return 2;
    }
    (void)pdft::ReadOsBuild(pdft::RegistryRoots{}, &build_);
    explorersBefore_ = ExplorersNow();
    stamp_ = UtcStamp();

    Note("PdfThumbnails refresh experiment, " + Utf8(stamp_) + " UTC");
    Note(Text("Windows %s; %s; File Explorer: %s", build_.empty() ? "?" : Utf8(build_).c_str(),
              options_.interactive ? "with the user at the PC" : "--non-interactive",
              otherShellUser_ ? "another account's" : "this user's"));
    Note(Text("zoom saved before: %d %%%s; baseline %d %%, after the flip %d %%", savedZoom_,
              g_zoomGuard.value ? "" : " (no value of its own)", kBaselineZoom, kFlippedZoom));
    Note(Text("Smart App Control: %s; thumbnails shown: %s; the .pdf thumbnail handler: %s's",
              sacOn_ ? "on" : "off", thumbnailsOff_ ? "no (icons only)" : "yes",
              Utf8(pdft::kProductName).c_str()));
    if (explorersBefore_) {
      for (const ExplorerProcess& process : *explorersBefore_) {
        Note(Text("explorer.exe pid %lu, started %s", process.pid,
                  process.started ? TimeText(process.started).c_str() : "(unreadable)"));
      }
    } else {
      Note("explorer.exe: the processes cannot be listed");
    }

    // What the cache rebuild would find, read-only, in a thumbtest process of its own.
    std::printf("thumbtest shellinfo --windows --holders\n");
    DWORD exitCode = 0;
    std::string output;
    hr = RunProgram(options_.self, {L"shellinfo", L"--windows", L"--holders"}, false, &exitCode,
                    &output);
    Note("shellinfo --windows --holders (exit " + std::to_string(exitCode) + "):");
    size_t start = 0;
    while (start < output.size()) {
      size_t end = output.find('\n', start);
      if (end == std::string::npos) end = output.size();
      std::string line = output.substr(start, end - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      Note("  " + line);
      start = end + 1;
    }
    if (FAILED(hr)) Note("  could not be started: " + Hr(hr));

    // How tabs show in the shell's window list: the user's count against its entries.
    if (options_.interactive) {
      std::vector<pdft::FolderWindow> windows;
      hr = pdft::ListFolderWindows(kViewTimeoutMs, &windows);
      std::set<ULONG_PTR> frames;
      for (const pdft::FolderWindow& window : windows) frames.insert(window.frame);
      const std::optional<long> userWindows =
          AskCount("How many File Explorer windows do you have open now?");
      const std::optional<long> userTabs =
          userWindows ? AskCount("And how many tabs in all of them?") : std::nullopt;
      if (!userTabs) {
        std::printf("Nothing changed.\n");
        return 3;
      }
      Note(
          Text("folder windows: %ld windows and %ld tabs by the user's count; the shell lists %zu "
               "entries in %zu frames (%s)",
               *userWindows, *userTabs, windows.size(), frames.size(), Hr(hr).c_str()));
    }

    // The report folder: out\refresh-experiment\<stamp> from thumbtest's folder.
    reportDir_ =
        (options_.self.parent_path() / L".." / L".." / L".." / L"refresh-experiment" / stamp_)
            .lexically_normal();
    const int made = SHCreateDirectoryExW(nullptr, reportDir_.c_str(), nullptr);
    if (made != ERROR_SUCCESS) {
      std::printf("cannot create the report folder %s (error %d)\n",
                  Utf8(reportDir_.native()).c_str(), made);
      return 2;
    }
    return std::nullopt;
  }

  // ---- Files ----

  bool MakeFiles() {
    Say("");
    Say("Files");
    std::filesystem::path root;
    if (options_.root) {
      root = *options_.root;
    } else {
      wchar_t temp[MAX_PATH + 1] = {};
      const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
      if (length == 0 || length > MAX_PATH)
        return Stop("cannot find the folder for temporary files");
      root = std::filesystem::path(temp) / (kRootPrefix + stamp_);
    }
    // A new folder, so everything below it is the experiment's own.
    if (!CreateDirectoryW(root.c_str(), nullptr)) {
      return Stop(Text("cannot create the experiment's folder (error %lu)", GetLastError()));
    }
    HRESULT hr = pdft::Canonicalize(root, &root_);
    if (FAILED(hr)) {
      (void)RemoveDirectoryW(root.c_str());
      return Stop("cannot read the experiment's folder back: " + Hr(hr));
    }
    ownDirs_.push_back(root_);
    std::printf("The experiment's folder: %s\n", Utf8(root_.native()).c_str());
    // OneDrive may turn files into placeholders, which the refresh leaves alone.
    for (const wchar_t* variable : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
      wchar_t value[MAX_PATH + 1] = {};
      const DWORD length = GetEnvironmentVariableW(variable, value, ARRAYSIZE(value));
      if (length > 0 && length <= MAX_PATH &&
          (pdft::IsUnder(root_, value) || pdft::SameImagePath(root_.native(), value))) {
        return Stop("the experiment's folder is in OneDrive: pass --root with a folder outside it");
      }
    }
    if (root_.native().size() > kMaxRootChars) {
      return Stop("the experiment's folder's path is too long: pass a shorter --root");
    }

    const std::vector<pdft::test::GeneratedFile> pdfs = pdft::test::MakeTestPdfs();
    const auto bytesOf = [&pdfs](std::wstring_view name) -> std::string_view {
      for (const pdft::test::GeneratedFile& file : pdfs) {
        if (file.name == name) return file.bytes;
      }
      return {};
    };
    const std::string_view portrait = bytesOf(L"01-portrait.pdf");
    const std::string_view landscape = bytesOf(L"02-landscape.pdf");
    const std::string_view heavy = bytesOf(L"08-heavy.pdf");
    const std::string_view largest = bytesOf(L"09-max-page-14400.pdf");
    if (portrait.empty() || landscape.empty() || heavy.empty() || largest.empty()) {
      return Stop("the test PDFs are not the ones expected");
    }

    // The judged folders, in the order the report names them: of the variants, only those run.
    judged_ = {kControlLabel};
    for (const Variant* variant : variants_) judged_.push_back(variant->label);
    const wchar_t* const rounds[] = {kNoneLabel,  kItemLabel,  kDirLabel,
                                     kTouchLabel, kAssocLabel, kDialogLabel};
    for (const wchar_t* label : rounds) judged_.push_back(label);
    // A judged folder: the control's pages in turn, so that neither page is all at the front.
    const auto makeJudged = [&](const std::wstring& label) {
      const std::filesystem::path folder = Folder(label);
      if (!MakeFolder(folder)) return false;
      if (label == kControlLabel) {
        for (int n = 1; n <= kControlPages; ++n) {
          wchar_t portraitName[32], landscapeName[32];
          std::swprintf(portraitName, ARRAYSIZE(portraitName), L"portrait-%03d.pdf", n);
          std::swprintf(landscapeName, ARRAYSIZE(landscapeName), L"landscape-%03d.pdf", n);
          if (!MakeFile(label, folder / portraitName, portrait) ||
              !MakeFile(label, folder / landscapeName, landscape)) {
            return false;
          }
        }
        return true;
      }
      return MakeFile(label, folder / L"portrait-001.pdf", portrait) &&
             MakeFile(label, folder / L"portrait-002.pdf", portrait) &&
             MakeFile(label, folder / L"landscape-001.pdf", landscape) &&
             MakeFile(label, folder / L"landscape-002.pdf", landscape);
    };

    // The files are made and filled in this order: the slow folder, the probe, the rounds'
    // folders (then the Desktop file), the variants' (the product's flags last) and the control
    // last, since on CI the cache dropped files filled early and none of those filled last. Then
    // the timing, ladder and untouched files, which the fill leaves out.
    if (!MakeFolder(Folder(kSlowLabel))) return false;
    for (int n = 1; n <= 3; ++n) {
      wchar_t heavyName[32], largestName[32];
      std::swprintf(heavyName, ARRAYSIZE(heavyName), L"portrait-heavy-%03d.pdf", n);
      std::swprintf(largestName, ARRAYSIZE(largestName), L"%ls%03d.pdf", kLargestPrefix, n);
      if (!MakeFile(kSlowLabel, Folder(kSlowLabel) / heavyName, heavy) ||
          !MakeFile(kSlowLabel, Folder(kSlowLabel) / largestName, largest)) {
        return false;
      }
    }
    if (!MakeFolder(Folder(kProbeLabel)) ||
        !MakeFile(kProbeLabel, Folder(kProbeLabel) / L"portrait-001.pdf", portrait)) {
      return false;
    }
    probeFile_ = files_[kProbeLabel].front();
    fillOrder_ = {kSlowLabel, kProbeLabel};
    for (const wchar_t* label : rounds) {
      if (!makeJudged(label)) return false;
      fillOrder_.push_back(label);
    }
    if (options_.desktop && !MakeDesktopFile(portrait)) return false;
    fillOrder_.push_back(kDesktopLabel);
    for (auto variant = variants_.rbegin(); variant != variants_.rend(); ++variant) {
      if (!makeJudged((*variant)->label)) return false;
      fillOrder_.push_back((*variant)->label);
    }
    if (!makeJudged(kControlLabel)) return false;
    fillOrder_.push_back(kControlLabel);
    if (!MakeFolder(Folder(kTimingLabel))) return false;
    for (long n = 1; n <= options_.timingFiles; ++n) {
      wchar_t name[32];
      std::swprintf(name, ARRAYSIZE(name), L"portrait-%04ld.pdf", n);
      if (!MakeFile(kTimingLabel, Folder(kTimingLabel) / name, portrait)) return false;
    }
    if (!MakeFolder(Folder(kLadderLabel))) return false;
    for (const LadderFile& ladder : kLadderFiles) {
      if (!MakeFile(kLadderLabel, Folder(kLadderLabel) / ladder.name, portrait)) return false;
    }
    if (!MakeFolder(Folder(kUntouchedLabel))) return false;
    for (int n = 1; n <= kUntouchedFiles; ++n) {
      wchar_t name[32];
      std::swprintf(name, ARRAYSIZE(name), L"portrait-%03d.pdf", n);
      if (!MakeFile(kUntouchedLabel, Folder(kUntouchedLabel) / name, portrait)) return false;
    }
    // Each folder as the refresh sees it: not on the network, not too long a path.
    for (const auto& [label, files] : files_) {
      if (files.empty()) continue;
      pdft::CandidateSkip skip = pdft::CandidateSkip::kNone;
      hr = SkipOf(files.front(), &skip);
      if (FAILED(hr) || skip != pdft::CandidateSkip::kNone) {
        return Stop("the files in " + Utf8(label) + " are " +
                    (FAILED(hr) ? "unreadable (" + Hr(hr) + ")" : std::string(SkipText(skip))) +
                    ", and the refresh leaves such files alone");
      }
    }
    Say(Text(
        "1 folder of %d files, %zu folders of 4 files, %ld timing files, 6 slow files, 1 probe "
        "file, %zu ladder files, %d untouched files",
        2 * kControlPages, judged_.size() - 1, options_.timingFiles, std::size(kLadderFiles),
        kUntouchedFiles));
    return true;
  }

  // With --desktop and a yes: PdfThumbnails-refresh-test.pdf on the Desktop.
  bool MakeDesktopFile(std::string_view portrait) {
    wil::unique_cotaskmem_string known;
    std::filesystem::path desktop;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, known.put());
    if (SUCCEEDED(hr)) hr = pdft::Canonicalize(known.get(), &desktop);
    if (FAILED(hr)) {
      Say("Desktop: not used (it cannot be found: " + Hr(hr) + ")");
      return true;
    }
    for (const wchar_t* variable : {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"}) {
      wchar_t value[MAX_PATH + 1] = {};
      const DWORD length = GetEnvironmentVariableW(variable, value, ARRAYSIZE(value));
      if (length > 0 && length <= MAX_PATH &&
          (pdft::IsUnder(desktop, value) || pdft::SameImagePath(desktop.native(), value))) {
        Say("Desktop: not used (it is in OneDrive)");
        return true;
      }
    }
    const Answer put = AskYesNo(
        options_.keep
            ? "Put one test PDF, PdfThumbnails-refresh-test.pdf, on your Desktop (kept at the end, "
              "with --keep)?"
            : "Put one test PDF, PdfThumbnails-refresh-test.pdf, on your Desktop (deleted at the "
              "end)?");
    if (put == Answer::kStop) return StopByUser();
    if (put == Answer::kNo) {
      Say("Desktop: not used (the user said no)");
      return true;
    }
    OwnFile own;
    const std::filesystem::path file = desktop / kDesktopFileName;
    hr = WriteOwnFile(file, portrait, 1, &own);
    if (FAILED(hr)) {
      Say("Desktop: not used (<Desktop>\\" + Utf8(kDesktopFileName) +
          " cannot be written: " + Hr(hr) + ")");
      return true;
    }
    ownFiles_.push_back(std::move(own));
    desktopFile_ = file;
    Say("Desktop: <Desktop>\\" + Utf8(kDesktopFileName));
    return true;
  }

  // ---- The zoom ----

  // Saves |zoom| through the command-line executable, as a user saves it.
  bool SetZoom(int zoom) {
    bool closing = false;
    {
      std::lock_guard<std::mutex> held(g_zoomGuard.lock);
      closing = g_zoomGuard.closing;
      // From here on it may differ from the zoom saved before.
      if (!closing) g_zoomGuard.changed = true;
    }
    if (closing) return Stop("the experiment is ending (Ctrl+C, or its window closed)");
    zoomSet_ = true;
    Say("");
    Say(Text("Zoom %d %%: PdfThumbnailsCli --zoom %d --no-refresh", zoom, zoom));
    DWORD exitCode = 0;
    const HRESULT hr = RunProgram(options_.cli, {L"--zoom", std::to_wstring(zoom), L"--no-refresh"},
                                  true, &exitCode, nullptr);
    if (FAILED(hr)) {
      // Started again to put the zoom back, it would show the user another notification.
      if (Blocked(hr)) cliBlocked_ = true;
      return Stop("the command-line executable could not be started: " + Hr(hr) +
                  (Blocked(hr) ? " (blocked by Application Control)" : ""));
    }
    if (exitCode != 0) return Stop(Text("--zoom %d ended with exit code %lu", zoom, exitCode));
    return true;
  }

  // Puts the zoom saved before back: through the command-line executable when it was a value of
  // its own and Application Control did not block that executable, else (or when that fails) in
  // this process; then reads it back.
  bool PutZoomBack() {
    std::optional<DWORD> value;
    {
      std::lock_guard<std::mutex> held(g_zoomGuard.lock);
      if (!g_zoomGuard.changed) {
        Say("zoom: not changed");
        return true;
      }
      value = g_zoomGuard.value;
    }
    bool done = false;
    if (value && cliBlocked_) {
      Say("zoom: put back by this process (Application Control blocked the command-line "
          "executable)");
    } else if (value) {
      DWORD exitCode = 0;
      const HRESULT hr =
          RunProgram(options_.cli, {L"--zoom", std::to_wstring(*value), L"--no-refresh"}, true,
                     &exitCode, nullptr);
      done = SUCCEEDED(hr) && exitCode == 0;
      if (!done) {
        Say(
            Text("zoom: the command-line executable did not put it back (%s, exit %lu), so this "
                 "process does",
                 Hr(hr).c_str(), exitCode));
      }
    }
    {
      std::lock_guard<std::mutex> held(g_zoomGuard.lock);
      if (done) {
        g_zoomGuard.changed = false;
      } else if (g_zoomGuard.changed) {
        const HRESULT hr = PutZoomBackHere();
        if (FAILED(hr)) Say("zoom: cannot be put back here either: " + Hr(hr));
      }
    }
    pdft::Settings settings;
    pdft::ZoomDetails details;
    const HRESULT read = pdft::LoadSettings(pdft::RegistryRoots{}, &settings, &details);
    const bool back =
        SUCCEEDED(read) && (value ? details.decision.registry == pdft::ZoomValueState::kValid &&
                                        details.registryRaw == value
                                  : details.decision.registry == pdft::ZoomValueState::kMissing);
    if (back) {
      Say(Text("zoom: put back to %d %%%s", savedZoom_, value ? "" : " (no value of its own)"));
    } else {
      Say("zoom: NOT put back (" + Hr(read) + "): run the command printed at the start");
    }
    return back;
  }

  // ---- The cache filled, and the baseline ----

  // The cache files now, by name and size (metadata only): "thumbcache_16.db 1048576, ...".
  static std::string CacheFilesText() {
    std::map<std::string, CacheFile> files;
    std::map<std::string, DWORD> unreadable;
    const HRESULT hr = ListCacheFiles(&files, &unreadable);
    if (FAILED(hr)) return "not listed (" + Hr(hr) + ")";
    std::string text;
    for (const auto& [name, file] : files) {
      text += (text.empty() ? "" : ", ") + name + " " + std::to_string(file.size);
    }
    for (const auto& [name, error] : unreadable) {
      text += (text.empty() ? "" : ", ") + name + Text(" (unreadable: error %lu)", error);
    }
    return text.empty() ? "none" : text;
  }

  // The answers of |records| that are not held, by kind: "MISS 0x8004B200 x3, a smaller size's
  // x2".
  static std::string ShortfallText(std::span<const CacheRecord> records) {
    std::vector<std::pair<std::string, size_t>> kinds;
    for (const CacheRecord& record : records) {
      if (record.cached) continue;
      const pdft::CacheHit hit = HitOf(record);
      const std::string kind = hit == pdft::CacheHit::kNotCached ? "MISS " + Hr(record.hr)
                               : hit == pdft::CacheHit::kRefused ? std::string("REFUSED")
                                                                 : std::string(HitText(hit));
      const auto found = std::find_if(kinds.begin(), kinds.end(),
                                      [&kind](const auto& known) { return known.first == kind; });
      if (found == kinds.end()) {
        kinds.emplace_back(kind, 1);
      } else {
        ++found->second;
      }
    }
    std::string text;
    for (const auto& [kind, count] : kinds) {
      text += (text.empty() ? "" : ", ") + kind + Text(" x%zu", count);
    }
    return text;
  }

  // The cache files' bytes in all.
  static ULONGLONG TotalBytes(const std::map<std::string, CacheFile>& files) {
    ULONGLONG total = 0;
    for (const auto& [name, file] : files) total += file.size;
    return total;
  }

  // The untouched files, each stored once at kUntouchedSize, right after the ladder, then asked
  // about at once: the ones held then are watched from here on (CheckUntouched).
  bool StoreUntouched() {
    const std::vector<std::filesystem::path>& files = FilesOf(kUntouchedLabel);
    size_t made = 0, failed = 0;
    for (const std::filesystem::path& file : files) {
      pdft::CacheAnswer answer;
      HRESULT hr = cache_->Select(file);
      if (SUCCEEDED(hr)) hr = cache_->Get(kUntouchedSize, kPopulateFlags, &answer);
      if (Blocked(hr)) return StopIfBlocked(hr);
      if (SUCCEEDED(hr) && answer.bitmap) {
        ++made;
      } else {
        ++failed;
      }
    }
    bool unopened = false;
    const UINT sizes[] = {kUntouchedSize};
    const std::vector<CacheRecord> now =
        AskCache(*cache_, files, sizes, pdft::kRefreshProbeFlags, std::nullopt, false, &unopened);
    std::vector<std::wstring> notHeld;
    for (const CacheRecord& record : now) {
      untouched_.push_back({record.key, record.cached, {}});
      if (!record.cached) notHeld.push_back(record.key);
    }
    Say(
        Text("untouched files stored once at %u: %zu made, %zu failed; held right after: %zu of "
             "%zu%s",
             kUntouchedSize, made, failed, now.size() - notHeld.size(), now.size(),
             notHeld.empty() ? "" : (" (not held: " + NamesText(notHeld) + ")").c_str()));
    return true;
  }

  // The untouched files asked about at kUntouchedSize |when| ("after the fill"), said as one line;
  // the number of those held right after their store that the cache has dropped since.
  size_t CheckUntouched(const std::string& when) {
    if (untouched_.empty() || !cache_) return 0;
    bool unopened = false;
    const UINT sizes[] = {kUntouchedSize};
    const std::vector<CacheRecord> now =
        AskCache(*cache_, FilesOf(kUntouchedLabel), sizes, pdft::kRefreshProbeFlags, std::nullopt,
                 false, &unopened);
    size_t watched = 0, kept = 0;
    std::string lost;
    std::vector<std::wstring> never;
    for (size_t i = 0; i < untouched_.size(); ++i) {
      Untouched& file = untouched_[i];
      if (!file.watched) {
        never.push_back(file.key);
        continue;
      }
      ++watched;
      if (file.lost.empty() && !(i < now.size() && now[i].cached)) file.lost = when;
      if (file.lost.empty()) {
        ++kept;
      } else {
        lost += (lost.empty() ? "" : "; ") + NameOf(file.key) + ", " + file.lost;
      }
    }
    std::string line =
        Text("untouched records kept: %zu of %zu held since their store", kept, watched);
    if (!lost.empty()) line += " (lost: " + lost + ")";
    if (!never.empty()) line += "; not held right after its store: " + NamesText(never);
    Say(line);
    return watched - kept;
  }

  bool Populate() {
    Say("");
    Phase("the fill");
    Say("Filling the cache at every size (made and stored in the shared thumbnail process)");
    std::unique_ptr<pdft::ThumbnailCacheOps> cache;
    HRESULT hr = pdft::MakeThumbnailCacheOps(&cache);
    if (FAILED(hr)) return Stop("cannot create the thumbnail cache: " + Hr(hr));
    cache_ = std::move(cache);

    std::map<std::string, CacheFile> before, after;
    std::map<std::string, DWORD> unreadable;
    const bool listed = SUCCEEDED(ListCacheFiles(&before, &unreadable));
    if (listed) cacheBytesAtStart_ = TotalBytes(before);
    // The cache files written since the last listing; |record| (when named) keeps the listing.
    const auto written = [&](const std::wstring& record) {
      if (!listed || FAILED(ListCacheFiles(&after, &unreadable))) return std::string("not listed");
      const std::string names = ChangedFiles(before, after);
      if (!record.empty()) SaveText(record, CacheFilesRecord(after));
      before = after;
      return names;
    };
    // The index file's size in the last listing.
    const auto indexBytes = [&]() {
      const auto index = before.find("thumbcache_idx.db");
      return listed && index != before.end() ? std::to_string(index->second.size)
                                             : std::string("-");
    };

    // The ladder first, each file at its one size only, and the cache files' sizes right after
    // each store: on a new cache the first is its only thumbnail, so the files of the sizes below
    // 256 show whether a store at 256 writes them too.
    const std::vector<std::filesystem::path>& ladder = FilesOf(kLadderLabel);
    for (size_t i = 0; i < ladder.size() && i < std::size(kLadderFiles); ++i) {
      const LadderFile& file = kLadderFiles[i];
      pdft::CacheAnswer answer;
      hr = cache_->Select(ladder[i]);
      if (SUCCEEDED(hr)) hr = cache_->Get(file.size, kPopulateFlags, &answer);
      if (Blocked(hr)) return StopIfBlocked(hr);
      const std::string went = written(L"");
      Say(Text("fill %s/%s made at %u: %s, %s, out 0x%lX; cache files written: %s",
               Utf8(kLadderLabel).c_str(), Utf8(file.name).c_str(), file.size, HrOrOk(hr).c_str(),
               BitmapText(answer).c_str(), static_cast<unsigned long>(answer.cacheFlags),
               went.c_str()));
      Say("cache files now: " + CacheFilesText());
    }
    if (!StoreUntouched()) return false;
    (void)written(L"");  // the fill's first size names only the files it wrote

    std::vector<std::pair<std::wstring, std::filesystem::path>> all;
    for (const std::wstring& label : fillOrder_) {
      if (label == kDesktopLabel) {
        if (desktopFile_) all.push_back({kDesktopLabel, *desktopFile_});
        continue;
      }
      for (const std::filesystem::path& file : FilesOf(label)) all.push_back({label, file});
    }
    std::vector<std::filesystem::path> allFiles;
    for (const auto& [label, file] : all) allFiles.push_back(file);

    std::vector<CacheRecord> records;
    for (const UINT size : kExperimentSizes) {
      const std::vector<UINT> filledBefore = sizes_;  // the sizes judged filled before this one
      size_t made = 0, failed = 0;
      HRESULT firstFailure = S_OK;
      bool refused = false;
      std::set<int> edges;
      std::map<DWORD, size_t> outFlags;  // the answers' WTS_CACHEFLAGS
      for (size_t i = 0; i < all.size(); ++i) {
        CacheRecord record;
        record.label = all[i].first;
        record.key = record.label + L"/" + all[i].second.filename().native();
        record.size = size;
        pdft::CacheAnswer answer;
        record.hr = cache_->Select(all[i].second);
        if (SUCCEEDED(record.hr)) record.hr = cache_->Get(size, kPopulateFlags, &answer);
        record.cacheFlags = answer.cacheFlags;
        record.id = answer.id;
        if (answer.bitmap) {
          record.w = static_cast<int>(answer.dims.cx);
          record.h = static_cast<int>(answer.dims.cy);
        }
        record.cached = record.hr == S_OK && answer.bitmap && (answer.cacheFlags & WTS_CACHED);
        const HRESULT got = record.hr;
        records.push_back(std::move(record));
        if (Blocked(got)) return StopIfBlocked(got);
        if (got == E_INVALIDARG && i == 0) {
          refused = true;
          break;
        }
        if (SUCCEEDED(got) && answer.bitmap) {
          ++made;
          edges.insert(static_cast<int>(std::max(answer.dims.cx, answer.dims.cy)));
          ++outFlags[answer.cacheFlags];
        } else {
          ++failed;
          if (SUCCEEDED(firstFailure)) firstFailure = FAILED(got) ? got : E_FAIL;
        }
      }
      if (made == 0) {
        refusedSizes_.push_back(size);
      } else if (std::find(std::begin(pdft::kRefreshSizes), std::end(pdft::kRefreshSizes), size) !=
                 std::end(pdft::kRefreshSizes)) {
        // Judged and refreshed: the sizes with a file of their own.
        sizes_.push_back(size);
      }
      const std::string went = written(L"cachefiles-" + std::to_wstring(size) + L".tsv");
      if (refused) {
        Say(Text("size %4u: refused (E_INVALIDARG)", size));
        continue;
      }
      // Whether the cache holds what it has just made: asked once, at once. The files it does not
      // hold are asked at the sizes filled before: the cache dropped them whole, or at this size
      // only.
      std::string heldNow;
      if (made > 0) {
        bool unopened = false;
        const UINT asked[] = {size};
        std::vector<CacheRecord> now = AskCache(*cache_, allFiles, asked, pdft::kRefreshProbeFlags,
                                                std::nullopt, false, &unopened);
        LabelDesktop(&now);
        const size_t held = static_cast<size_t>(std::count_if(
            now.begin(), now.end(), [](const CacheRecord& record) { return record.cached; }));
        heldNow = Text("; held right after: %zu of %zu", held, now.size());
        if (held < now.size()) {
          heldNow += " (" + ShortfallText(now) + ")";
          std::vector<std::filesystem::path> missing;
          std::vector<std::wstring> missingKeys;
          for (size_t i = 0; i < now.size() && i < allFiles.size(); ++i) {
            if (now[i].cached) continue;
            missing.push_back(allFiles[i]);
            missingKeys.push_back(now[i].key);
          }
          if (filledBefore.empty()) {
            heldNow +=
                Text("; not held: %zu (%s)", missing.size(), NamesText(missingKeys, true).c_str());
          } else {
            std::vector<CacheRecord> earlier =
                AskCache(*cache_, missing, filledBefore, pdft::kRefreshProbeFlags, std::nullopt,
                         false, &unopened);
            LabelDesktop(&earlier);
            const std::vector<std::wstring> still = HeldFiles(earlier);
            std::vector<std::wstring> whole;
            for (const std::wstring& key : missingKeys) {
              if (std::find(still.begin(), still.end(), key) == still.end()) whole.push_back(key);
            }
            heldNow += Text("; not held: %zu, dropped whole %zu (%s), at this size only %zu",
                            missing.size(), whole.size(), NamesText(whole, true).c_str(),
                            missing.size() - whole.size());
          }
        }
      }
      std::string out;
      for (const auto& [flags, count] : outFlags) {
        out += (out.empty() ? "" : ", ") +
               Text("0x%lX x%zu", static_cast<unsigned long>(flags), count);
      }
      Say(Text(
          "size %4u: %zu made, %zu failed%s; edges %s; out %s%s; cache files written: %s; idx %s",
          size, made, failed, failed ? (" (first " + Hr(firstFailure) + ")").c_str() : "",
          EdgesText(edges).c_str(), out.empty() ? "-" : out.c_str(), heldNow.c_str(), went.c_str(),
          indexBytes().c_str()));
    }
    SaveRecords(L"populate.tsv", records);
    // The fill's order, with the thumbnail ID each file's first store answered (the report only).
    size_t position = 0;
    for (const CacheRecord& record : records) {
      if (record.size != kExperimentSizes[0]) continue;
      Note(Text("fill %2zu %s id %s", ++position, Utf8(record.key).c_str(),
                IdHex(record.id).substr(0, 16).c_str()));
    }
    Say("cache files after the fill: " + CacheFilesText());
    if (sizes_.empty())
      return Stop("the cache made no thumbnail at any size the refresh asks about");

    // The baseline, asked straight after the fill: what the cache holds now.
    Phase("the baseline");
    std::vector<std::filesystem::path> judgedFiles;
    for (const std::wstring& label : judged_) {
      for (const std::filesystem::path& file : FilesOf(label)) judgedFiles.push_back(file);
    }
    Say("Baseline: the product's question (WTS_INCACHEONLY) at each size judged");
    bool unopened = false;
    size_t again = 0;
    baseline_ = AskHeld(
        judgedFiles, "the baseline",
        [this](std::span<const CacheRecord> first) {
          SayAnswers("baseline", first);
          SayNotHeld(first);
        },
        &again, &unopened, true);
    const size_t held =
        static_cast<size_t>(std::count_if(baseline_.begin(), baseline_.end(),
                                          [](const CacheRecord& record) { return record.cached; }));
    Say(Text("baseline: %zu of %zu entries held (%zu only when asked again)", held,
             baseline_.size(), again));
    slowBefore_ = AskHeld(FilesOf(kSlowLabel), "the slow folder", {}, &again, &unopened);
    SaveRecords(L"baseline.tsv", baseline_);
    SaveRecords(L"slow-before.tsv", slowBefore_);
    if (unopened) Say("some files could not be opened for the questions");
    (void)CheckUntouched("after the fill");
    return true;
  }

  static std::string ChangedFiles(const std::map<std::string, CacheFile>& before,
                                  const std::map<std::string, CacheFile>& after) {
    std::string names;
    for (const auto& [name, file] : after) {
      const auto was = before.find(name);
      if (was == before.end() || was->second.size != file.size ||
          was->second.written != file.written) {
        names += (names.empty() ? "" : ", ") + name;
      }
    }
    return names.empty() ? "none" : names;
  }

  // ---- The cache's answers to other flags ----

  // One question about |file| of |label| at |size| with |flags|, said as one line; false
  // (stopped) at Application Control's block.
  bool AskFlags(const std::wstring& label, const std::filesystem::path& file, UINT size,
                DWORD flags, pdft::CacheAnswer* answer, HRESULT* hr) {
    *answer = {};
    *hr = cache_->Select(file);
    if (SUCCEEDED(*hr)) *hr = cache_->Get(size, flags, answer);
    if (Blocked(*hr)) return StopIfBlocked(*hr);
    std::string what;
    if (flags & WTS_INCACHEONLY) {
      what = HitText(pdft::ClassifyCacheAnswer(size, *hr, *answer));
    } else if (*hr == E_INVALIDARG) {
      what = "refused";
    } else if (FAILED(*hr)) {
      what = "failed";
    } else if (answer->bitmap && (answer->cacheFlags & WTS_CACHED)) {
      what = "answered from the cache";
    } else {
      what = answer->bitmap ? "made" : "nothing came back";
    }
    const std::string key = Utf8(label + L"/" + file.filename().native());
    std::string bitmap = "no bitmap";
    if (answer->bitmap) {
      bitmap = BitmapText(*answer) + Text(", out 0x%lX, id %s",
                                          static_cast<unsigned long>(answer->cacheFlags),
                                          IdHex(answer->id).c_str());
    }
    Say(Text("flags 0x%05lX %s %4u: hr %s, %s: %s", static_cast<unsigned long>(flags), key.c_str(),
             size, Hr(*hr).c_str(), bitmap.c_str(), what.c_str()));
    return true;
  }

  // How the cache answers other flags, straight after the baseline: the probe file (filled at
  // every size) and the ladder (each file at one size). Synthetic files only, never extracted in
  // this process. Then what the answers show: whether the old question and the other flags with
  // WTS_IDEALCACHESIZEONLY are refused, whether the bitmap's size tells which size answered, what
  // the question scaled to the size asked answers, whether the product's extraction flags make a
  // thumbnail, whether an entry is stored at its file's size, and whether the thumbnail ID names
  // the file.
  bool FlagAnswers() {
    Say("");
    Phase("the flag answers");
    Say("Flag answers (the probe and ladder files only, which are never judged)");
    struct Answered {
      UINT size = 0;
      DWORD flags = 0;
      HRESULT hr = S_OK;
      pdft::CacheAnswer answer;
    };
    std::vector<Answered> probe;
    std::vector<std::vector<Answered>> ladderAnswers(std::size(kLadderFiles));
    const auto ask = [this](const std::wstring& label, const std::filesystem::path& file, UINT size,
                            DWORD flags, std::vector<Answered>* into) {
      Answered answered{size, flags, S_OK, {}};
      if (!AskFlags(label, file, size, flags, &answered.answer, &answered.hr)) return false;
      into->push_back(answered);
      return true;
    };
    for (const DWORD flags :
         {pdft::kRefreshProbeFlags, kIdealProbeFlags, kScaledProbeFlags, kIdealNoCacheFlags}) {
      for (const UINT size : kProbeAskSizes) {
        if (!ask(kProbeLabel, probeFile_, size, flags, &probe)) return false;
      }
    }
    for (const UINT size : kProductExtractSizes) {
      if (!ask(kProbeLabel, probeFile_, size, kProductNoCacheFlags, &probe)) return false;
    }
    const std::vector<std::filesystem::path>& ladder = FilesOf(kLadderLabel);
    for (size_t i = 0; i < ladder.size() && i < std::size(kLadderFiles); ++i) {
      const LadderFile& file = kLadderFiles[i];
      for (const DWORD flags : {pdft::kRefreshProbeFlags, kScaledProbeFlags}) {
        if (flags == kScaledProbeFlags && !file.scaled) continue;
        for (const UINT size : file.asks) {
          if (!ask(kLadderLabel, ladder[i], size, flags, &ladderAnswers[i])) return false;
        }
      }
    }
    // Last, since it may store the probe file's thumbnail.
    for (const UINT size : kProbeAskSizes) {
      if (!ask(kProbeLabel, probeFile_, size, kIdealExtractFlags, &probe)) return false;
    }

    const size_t oldRefused =
        static_cast<size_t>(std::count_if(probe.begin(), probe.end(), [](const Answered& a) {
          return a.flags == kIdealProbeFlags && a.hr == E_INVALIDARG;
        }));
    Say(Text("the old question (INCACHEONLY|IDEALCACHESIZEONLY): refused at %zu of %zu sizes",
             oldRefused, std::size(kProbeAskSizes)));

    // Every question and extraction with WTS_IDEALCACHESIZEONLY.
    std::string idealFlags, idealRefused;
    bool idealEverywhere = true;
    for (const DWORD flags : {kIdealProbeFlags, kIdealNoCacheFlags, kIdealExtractFlags}) {
      size_t asked = 0, refused = 0;
      for (const Answered& a : probe) {
        if (a.flags != flags) continue;
        ++asked;
        if (a.hr == E_INVALIDARG) ++refused;
      }
      idealEverywhere = idealEverywhere && asked > 0 && refused == asked;
      idealFlags += (idealFlags.empty() ? "" : ", ") + Text("0x%lX", flags);
      idealRefused += (idealRefused.empty() ? "" : ", ") +
                      Text("0x%lX at %zu of %zu sizes", flags, refused, asked);
    }
    Say(idealEverywhere ? "WTS_IDEALCACHESIZEONLY: refused with every flag it was asked with (" +
                              idealFlags + ")"
                        : "WTS_IDEALCACHESIZEONLY: refused with " + idealRefused);

    // The first ladder file, stored at 256 only: a size whose file is below 256's must be held
    // (with a bitmap of that size's file), 256's sizes held, and a larger size answered by its
    // 256 entry with WTS_LOWQUALITY (a smaller size's) or not at all; no size by a larger size's.
    // Without its entry at its own size the answers tell nothing.
    const LadderFile& first = kLadderFiles[0];
    const UINT own = pdft::CacheSizeOf(first.size);
    bool told = true, there = false;
    size_t aboveSmaller = 0, aboveNone = 0;
    std::vector<UINT> below;  // the sizes asked whose file is below the stored one's
    std::string came;
    const auto edgeOf = [](const Answered& a) {
      return a.answer.bitmap ? std::max(a.answer.dims.cx, a.answer.dims.cy) : 0;
    };
    for (const Answered& a : ladderAnswers[0]) {
      if (a.flags != pdft::kRefreshProbeFlags) continue;
      const pdft::CacheHit hit = pdft::ClassifyCacheAnswer(a.size, a.hr, a.answer);
      const LONG edge = edgeOf(a);
      const UINT file = pdft::CacheSizeOf(a.size);
      const bool lowSmaller =
          hit == pdft::CacheHit::kSmaller && (a.answer.cacheFlags & WTS_LOWQUALITY);
      bool expected = hit == pdft::CacheHit::kHeld;
      if (file < own) {
        below.push_back(a.size);
      } else if (file > own) {
        expected = lowSmaller || hit == pdft::CacheHit::kNotCached;
        if (lowSmaller) ++aboveSmaller;
        if (hit == pdft::CacheHit::kNotCached) ++aboveNone;
      }
      if (a.size == first.size && hit == pdft::CacheHit::kHeld) there = true;
      told = told && expected;
      came += (came.empty() ? "" : ", ") +
              (a.answer.bitmap ? Text("%u: edge %ld, out 0x%lX, %s", a.size, edge,
                                      static_cast<unsigned long>(a.answer.cacheFlags), HitText(hit))
                               : Text("%u: %s (%s)", a.size, HitText(hit), Hr(a.hr).c_str()));
    }
    if (!there) {
      Say(
          Text("which size answered can be told from the bitmap's size: not known (the ladder "
               "file filled at %u only is not held at %u: %s)",
               first.size, first.size, came.empty() ? "not asked" : came.c_str()));
    } else if (told) {
      const std::string above =
          aboveNone == 0 ? Text("its %u entry answers with WTS_LOWQUALITY", first.size)
          : aboveSmaller == 0
              ? std::string("nothing answers")
              : Text("its %u entry answers with WTS_LOWQUALITY, or nothing", first.size);
      std::string sizes;  // "16 and 96"
      for (size_t i = 0; i < below.size(); ++i) {
        sizes += (i == 0 ? "" : i + 1 == below.size() ? " and " : ", ") + std::to_string(below[i]);
      }
      Say(
          Text("which size answered can be told from the bitmap's size: yes (a file stored at %u "
               "is held at %s with bitmaps of those sizes; above %u %s)",
               first.size, sizes.empty() ? "no smaller size" : sizes.c_str(), first.size,
               above.c_str()));
    } else {
      Say("which size answered can be told from the bitmap's size: no (" + came + ")");
    }

    // The same file asked to scale to the size asked: at the sizes of its own file and below, a
    // bitmap of the size asked; above, its own entry with WTS_LOWQUALITY.
    std::string scaled, unexpected;
    for (const Answered& a : ladderAnswers[0]) {
      if (a.flags != kScaledProbeFlags) continue;
      const LONG edge = edgeOf(a);
      const bool expected = a.size <= first.size
                                ? a.answer.bitmap && edge == static_cast<LONG>(a.size)
                                : a.answer.bitmap && edge == static_cast<LONG>(first.size) &&
                                      (a.answer.cacheFlags & WTS_LOWQUALITY);
      const std::string what = Text("%u: %s, out 0x%lX", a.size, BitmapText(a.answer).c_str(),
                                    static_cast<unsigned long>(a.answer.cacheFlags));
      if (!expected) {
        unexpected += (unexpected.empty() ? "" : ", ") + what;
        continue;
      }
      // Where it differs from the plain question's answer.
      const auto plain =
          std::find_if(ladderAnswers[0].begin(), ladderAnswers[0].end(), [&a](const Answered& p) {
            return p.flags == pdft::kRefreshProbeFlags && p.size == a.size;
          });
      if (a.size <= first.size && plain != ladderAnswers[0].end() && edgeOf(*plain) != edge) {
        scaled +=
            (scaled.empty() ? "" : ", ") + Text("%u: %s", a.size, BitmapText(a.answer).c_str());
      }
    }
    if (!there) {
      Say("scaled to the size asked (0x41): not known (see the line before)");
    } else if (unexpected.empty()) {
      Say(
          Text("scaled to the size asked (0x41): only below the stored size (%s; above %u: the %u "
               "entry with WTS_LOWQUALITY)",
               scaled.empty() ? "the same as without it" : scaled.c_str(), first.size, first.size));
    } else {
      Say("scaled to the size asked (0x41): not as the stored size's file says (" + unexpected +
          ")");
    }

    size_t productAsked = 0, productMade = 0;
    for (const Answered& a : probe) {
      if (a.flags != kProductNoCacheFlags) continue;
      ++productAsked;
      if (a.hr == S_OK && a.answer.bitmap) ++productMade;
    }
    Say(Text("the product's extraction flags (with EXTRACTDONOTCACHE): made at %zu of %zu sizes",
             productMade, productAsked));

    // The second ladder file, stored at 100: an entry of its file's size (256) is stored at that
    // size, one of 100 at the size asked.
    const LadderFile& second = kLadderFiles[1];
    const auto atOwn = std::find_if(
        ladderAnswers[1].begin(), ladderAnswers[1].end(), [&second](const Answered& a) {
          return a.flags == pdft::kRefreshProbeFlags && a.size == second.size;
        });
    const std::string secondName = Utf8(second.name);
    if (atOwn == ladderAnswers[1].end() || atOwn->hr != S_OK || !atOwn->answer.bitmap) {
      Say(Text("stored at its file's size: not known (%s at %u: %s)", secondName.c_str(),
               second.size, atOwn == ladderAnswers[1].end() ? "not asked" : Hr(atOwn->hr).c_str()));
    } else {
      const LONG edge = edgeOf(*atOwn);
      const char* answer = edge == static_cast<LONG>(pdft::CacheSizeOf(second.size)) ? "yes"
                           : edge == static_cast<LONG>(second.size) ? "no, at the size asked"
                                                                    : "not known";
      Say(Text("stored at its file's size: %s (%s: %s)", answer, secondName.c_str(),
               BitmapText(atOwn->answer).c_str()));
    }

    // One thumbnail ID per file at every size, and another for each file: the probe and the
    // ladder files have the same bytes.
    std::vector<std::pair<std::string, std::set<std::array<BYTE, 16>>>> ids;
    ids.emplace_back(Utf8(std::wstring(kProbeLabel) + L"/" + probeFile_.filename().native()),
                     std::set<std::array<BYTE, 16>>());
    for (const Answered& a : probe) {
      if (a.flags == pdft::kRefreshProbeFlags && a.hr == S_OK && a.answer.bitmap) {
        ids.back().second.insert(a.answer.id);
      }
    }
    for (size_t i = 0; i < std::size(kLadderFiles); ++i) {
      ids.emplace_back(Utf8(std::wstring(kLadderLabel) + L"/" + kLadderFiles[i].name),
                       std::set<std::array<BYTE, 16>>());
      for (const Answered& a : ladderAnswers[i]) {
        if (a.flags == pdft::kRefreshProbeFlags && a.hr == S_OK && a.answer.bitmap) {
          ids.back().second.insert(a.answer.id);
        }
      }
    }
    bool known = true, onePerFile = true;
    std::set<std::array<BYTE, 16>> every;
    std::string perFile;
    for (const auto& [name, fileIds] : ids) {
      known = known && !fileIds.empty();
      onePerFile = onePerFile && fileIds.size() == 1;
      every.insert(fileIds.begin(), fileIds.end());
      perFile += (perFile.empty() ? "" : ", ") + name + Text(": %zu", fileIds.size());
    }
    const bool different = every.size() == ids.size();
    const std::string question =
        "thumbnail IDs: one per file at every size, and different for files with the same bytes: ";
    Say(!known                    ? question + "not known (IDs per file: " + perFile + ")"
        : onePerFile && different ? question + "yes"
                                  : question + Text("no (IDs per file: %s; %zu different in all)",
                                                    perFile.c_str(), every.size()));
    return true;
  }

  // The judge's rules at sizes_ with |filesPerPage|, for a control or a folder refreshed in
  // another process (no extraction answers); |clean| for that refresh.
  pdft::test::JudgeRules Rules(size_t filesPerPage, bool clean = true) const {
    pdft::test::JudgeRules rules;
    rules.sizes = sizes_;
    rules.filesPerPage = filesPerPage;
    rules.refreshClean = clean;
    return rules;
  }

  // |label|'s verdict on |before| and |after| by |rules|, as a control when |control|.
  FolderVerdict JudgeFolder(const std::wstring& label, std::span<const CacheRecord> before,
                            std::span<const CacheRecord> after, bool control,
                            const pdft::test::JudgeRules& rules) const {
    const std::span<const std::wstring> one(&label, 1);
    const std::vector<FolderVerdict> verdicts =
        pdft::test::Judge(Only(before, one), Only(after, one), kBaselineZoom, kFlippedZoom,
                          control ? one : std::span<const std::wstring>(), rules);
    const FolderVerdict* verdict = VerdictOf(verdicts, label);
    if (verdict) return *verdict;
    FolderVerdict none;
    none.label = label;
    none.control = control;
    none.ruled = true;
    return none;
  }

  // At least |need| files of each page judged at every size: the folder can show a pass.
  static bool Enough(const FolderVerdict& verdict, size_t need) {
    return verdict.portraitBefore >= need && verdict.landscapeBefore >= need;
  }

  // Every judged folder's baseline must hold a portrait and a landscape page at the old zoom at
  // every size (the control two of each), and nothing new or neither; a folder that does not is
  // left out. The files the cache does not hold are named, and not judged.
  bool CheckBaseline() {
    Say("");
    Phase("the check");
    Say(
        Text("Baseline check: a portrait and a landscape page held at %d %% at every size (the "
             "control: %zu of each)",
             kBaselineZoom, kControlFilesPerPage));
    // A size at which the cache refused every question cannot be asked about: it is not judged.
    std::vector<UINT> asked;
    for (const UINT size : sizes_) {
      const auto at = [size](const CacheRecord& record) { return record.size == size; };
      const bool refused =
          std::any_of(baseline_.begin(), baseline_.end(), at) &&
          std::all_of(baseline_.begin(), baseline_.end(), [&at](const CacheRecord& record) {
            return !at(record) || HitOf(record) == pdft::CacheHit::kRefused;
          });
      if (refused) {
        Say(Text("size %4u: the cache refuses the question (E_INVALIDARG): not judged", size));
      } else {
        asked.push_back(size);
      }
    }
    if (asked.empty()) {
      valid_ = false;
      return Stop(
          "the cache refuses the question WTS_INCACHEONLY at every size (E_INVALIDARG): nothing "
          "can be asked");
    }
    if (asked.size() < sizes_.size()) {
      sizes_ = std::move(asked);
      const auto notJudged = [this](const CacheRecord& record) {
        return std::find(sizes_.begin(), sizes_.end(), record.size) == sizes_.end();
      };
      std::erase_if(baseline_, notJudged);
      std::erase_if(slowBefore_, notJudged);
    }

    // The sizes the cache holds at all.
    std::set<UINT> held;
    for (const CacheRecord& record : baseline_) {
      if (record.cached) held.insert(record.size);
    }
    FolderVerdict control;
    size_t controlNotHeld = 0, controlEntries = 0, judgedFiles = 0;
    for (const std::wstring& label : judged_) {
      const size_t need = label == kControlLabel ? kControlFilesPerPage : kFilesPerPage;
      const FolderVerdict verdict = JudgeFolder(label, baseline_, baseline_, false, Rules(need));
      const std::vector<CacheRecord> own =
          Only(baseline_, std::span<const std::wstring>(&label, 1));
      const size_t notHeld = static_cast<size_t>(std::count_if(
          own.begin(), own.end(), [](const CacheRecord& record) { return !record.cached; }));
      judgedFiles += FilesOf(label).size();
      const bool valid = verdict.baselineValid;
      if (!valid) excluded_.insert(label);
      std::string line = Text("%-8s %s: %zu held, %zu not at %d %%, %zu not held",
                              valid ? "VALID" : "EXCLUDED", Utf8(label).c_str(), verdict.baseline,
                              verdict.baselineNotOld, kBaselineZoom, notHeld);
      if (notHeld > 0) line += " (" + NotHeldNames(own) + ")";
      if (valid) {
        line += Text("; judged on %zu portrait and %zu landscape pages at every size",
                     verdict.portraitBefore, verdict.landscapeBefore);
      } else if (!Enough(verdict, need)) {
        line += Text(
            ": %zu portrait and %zu landscape pages held at every size, of the %zu of "
            "each it needs",
            verdict.portraitBefore, verdict.landscapeBefore, need);
      } else {
        line += Text(": thumbnails not at %d %%", kBaselineZoom);
      }
      Say(line);
      if (label == kControlLabel) {
        control = verdict;
        controlNotHeld = notHeld;
        controlEntries = own.size();
      }
    }
    size_t everySize = 0;
    const std::vector<NotHeldFile> notHeld = NotHeldFiles(baseline_);
    for (const NotHeldFile& file : notHeld) {
      if (file.sizes == "every size") ++everySize;
    }
    Say(
        Text("files not held at the baseline: %zu of %zu (%zu at every size, %zu at some sizes "
             "only)",
             notHeld.size(), judgedFiles, everySize, notHeld.size() - everySize));
    std::string sizes;
    for (const UINT size : held) sizes += Text(" %u", size);
    Say("sizes held:" + (sizes.empty() ? std::string(" none") : sizes));
    if (held.empty()) {
      valid_ = false;
      return Stop("the cache holds none of the thumbnails the fill made (see the baseline lines)");
    }
    if (Excluded(kControlLabel)) {
      valid_ = false;
      if (!Enough(control, kControlFilesPerPage)) {
        return Stop(Text(
            "the control folder holds too few thumbnails to tell a pass from thumbnails made "
            "again by something else: it needs %zu portrait and %zu landscape pages held at "
            "every size, and holds %zu portrait and %zu landscape pages at every size (%zu of its "
            "%zu entries not held)",
            kControlFilesPerPage, kControlFilesPerPage, control.portraitBefore,
            control.landscapeBefore, controlNotHeld, controlEntries));
      }
      return Stop(
          Text("the control folder has thumbnails not at %d %% (%zu), so a pass could not be told "
               "from thumbnails made again by something else",
               kBaselineZoom, control.baselineNotOld));
    }
    if (std::all_of(variants_.begin(), variants_.end(),
                    [this](const Variant* variant) { return Excluded(variant->label); })) {
      valid_ = false;
      return Stop(
          "no extraction variant's folder holds a portrait and a landscape page at every size");
    }
    return true;
  }

  // The flip, and the check that the thumbnail process renders at the new zoom.
  bool Flip() {
    Phase("the flip");
    if (!SetZoom(kFlippedZoom)) return false;
    pdft::CacheAnswer answer;
    HRESULT hr = cache_->Select(probeFile_);
    if (SUCCEEDED(hr)) hr = cache_->Get(kSanitySize, kNoCacheFlags, &answer);
    const std::wstring key = std::wstring(kProbeLabel) + L"/" + probeFile_.filename().native();
    const Crop crop = answer.bitmap ? pdft::test::ClassifyCrop(key, answer.dims.cx, answer.dims.cy,
                                                               kBaselineZoom, kFlippedZoom)
                                    : Crop::kOther;
    Say(Text("sanity check at %u, made and not stored: %s, %ldx%ld (%s)", kSanitySize,
             Hr(hr).c_str(), answer.dims.cx, answer.dims.cy,
             crop == Crop::kNew ? "the new zoom" : "NOT the new zoom"));
    if (hr != S_OK || crop != Crop::kNew) {
      valid_ = false;
      return Stop("the thumbnail process does not see the new zoom");
    }
    return true;
  }

  // ---- The extraction variants (Pass A) ----

  // What a refresh's extraction answers |made| show, each entry by its last call (a variant that
  // stores after the extraction makes two): "made: 36, 36 at 142 %, 36 with WTS_CACHED; out 0x2
  // x36".
  static std::string MadeText(std::span<const CacheRecord> made) {
    std::map<std::pair<std::wstring, UINT>, const CacheRecord*> last;
    for (const CacheRecord& record : made) last[{record.key, record.size}] = &record;
    size_t atNew = 0, withCached = 0;
    std::map<DWORD, size_t> outFlags;
    for (const auto& [entry, record] : last) {
      if (record->hr == S_OK &&
          pdft::test::ClassifyCrop(record->key, record->w, record->h, kBaselineZoom,
                                   kFlippedZoom) == Crop::kNew) {
        ++atNew;
      }
      if (record->cacheFlags & WTS_CACHED) ++withCached;
      ++outFlags[record->cacheFlags];
    }
    std::string out;
    for (const auto& [flags, count] : outFlags) {
      out +=
          (out.empty() ? "" : ", ") + Text("0x%lX x%zu", static_cast<unsigned long>(flags), count);
    }
    return Text("made: %zu, %zu at %d %%, %zu with WTS_CACHED; out %s", last.size(), atNew,
                kFlippedZoom, withCached, out.empty() ? "-" : out.c_str());
  }

  // The control's verdict after |variant|'s refresh, as one line.
  static std::string ControlText(const FolderVerdict& v, const std::wstring& variant) {
    return Text(
        "%-8s %s (control) after %s: %zu held before; now %zu old, %zu new, %zu gone, %zu "
        "neither, %zu dropped with their file; on %zu portrait and %zu landscape pages at every "
        "size",
        OutcomeText(v), Utf8(v.label).c_str(), Utf8(variant).c_str(), v.baseline, v.oldAfter,
        v.newAfter, v.missAfter, v.otherAfter, v.droppedAfter, v.portraitAfter, v.landscapeAfter);
  }

  // Each variant is judged right after its own refresh, on its extraction answers and on what
  // the cache holds then, and the control folder with it: a file the cache dropped since cannot
  // pass for a refresh that failed, and the control cannot lose a refresh's effect to the next.
  bool JudgeVariants() {
    Say("");
    Say("Extraction variants (no notification), each judged by the bitmaps' sizes right after "
        "its refresh, with the control folder");
    const std::wstring controls[] = {kControlLabel};
    std::vector<std::wstring> labels{kControlLabel};
    std::vector<CacheRecord> made, rightAfter;
    std::map<std::wstring, std::vector<CacheRecord>> latest;  // each folder's last answers
    std::vector<std::pair<const Variant*, FolderVerdict>> verdicts;
    FolderVerdict control;
    // The records so far, also when the control stops the run.
    const auto save = [&] {
      SaveRecords(L"made-variants.tsv", made);
      SaveRecords(L"right-after-variants.tsv", rightAfter);
    };
    if (const std::string leftOut = LeftOutVariants(); !leftOut.empty()) {
      Say(leftOut + ": left out (" + kSurrogateOnly + ")");
    }
    for (const Variant* variant : variants_) {
      if (Excluded(variant->label)) {
        Say(Text("%s: left out (no valid baseline)", Utf8(variant->label).c_str()));
        continue;
      }
      Phase(Utf8(variant->label));
      pdft::RefreshFilesOptions options = BestOptions(0);
      options.extractFlags = variant->extract;
      options.storeFlags = variant->store;
      pdft::RefreshCounts counts;
      HRESULT hr = S_OK;
      std::vector<CacheRecord> own;
      if (!Refresh(FilesOf(variant->label), options, *cache_, &counts, &hr, &own)) return false;
      made.insert(made.end(), own.begin(), own.end());
      labels.push_back(variant->label);
      if (variant->store) storeVariantJudged_ = true;
      // Its folder and the control, asked at once.
      std::vector<std::filesystem::path> files = FilesOf(variant->label);
      for (const std::filesystem::path& file : FilesOf(kControlLabel)) files.push_back(file);
      bool unopened = false;
      size_t again = 0;
      const std::vector<CacheRecord> answers =
          AskHeld(files, "right after " + Utf8(variant->label), {}, &again, &unopened);
      rightAfter.insert(rightAfter.end(), answers.begin(), answers.end());
      // A refresh that reported a failure fails, as the product's would end with exit code 1.
      pdft::test::JudgeRules rules = Rules(kFilesPerPage, counts.failed == 0);
      rules.made = own;
      rules.inProcess = true;
      const FolderVerdict verdict = JudgeFolder(variant->label, baseline_, answers, false, rules);
      control = JudgeFolder(kControlLabel, baseline_, answers, true, Rules(kControlFilesPerPage));
      const std::wstring label = variant->label;
      latest[label] = Only(answers, std::span<const std::wstring>(&label, 1));
      latest[kControlLabel] = Only(answers, controls);
      Say(Text("%s (%s): %s; %s; of %zu held and new right after, %zu with WTS_CACHED",
               Utf8(variant->label).c_str(), variant->flags, CountsText(hr, counts).c_str(),
               MadeText(own).c_str(), verdict.newAfter, verdict.heldNewCached));
      std::string line = VerdictText(verdict, kBaselineZoom, " right after its refresh");
      if (!verdict.filesDropped.empty()) {
        line += "; dropped: " + NamesText(verdict.filesDropped);
        if (verdict.filesGoneBefore) {
          line += Text(" (%zu already gone when it started)", verdict.filesGoneBefore);
        }
      }
      Say(line);
      Say(ControlText(control, variant->label));
      verdicts.push_back({variant, verdict});
      // The control folder is not refreshed: when it changed, something else made thumbnails
      // again, and a variant's pass would not be its own.
      if (control.outcome == pdft::test::FolderOutcome::kFail) {
        valid_ = false;
        save();
        return Stop(Text(
            "the control folder changed during %s's refresh (%zu new, %zu neither, "
            "%zu gone with other entries of their file left): the variants' results "
            "cannot be told from what else changed the cache",
            Utf8(variant->label).c_str(), control.newAfter, control.otherAfter, control.missAfter));
      }
      if (control.outcome != pdft::test::FolderOutcome::kPass) {
        valid_ = false;
        save();
        return Stop(
            Text("the cache dropped so many of the control folder's thumbnails during "
                 "%s's refresh that a pass could not be told from thumbnails made again "
                 "by something else (%zu portrait and %zu landscape pages left at every "
                 "size)",
                 Utf8(variant->label).c_str(), control.portraitAfter, control.landscapeAfter));
      }
    }
    save();

    // Everything once more, for the record: what the cache dropped since.
    std::vector<std::filesystem::path> files;
    for (const std::wstring& label : labels) {
      for (const std::filesystem::path& file : FilesOf(label)) files.push_back(file);
    }
    bool unopened = false;
    size_t again = 0;
    const std::vector<CacheRecord> after = AskHeld(
        files, "after the variants",
        [this](std::span<const CacheRecord> first) { SayAnswers("after the variants", first); },
        &again, &unopened);
    SaveRecords(L"after-variants.tsv", after);
    std::vector<std::wstring> droppedSince;
    for (const std::wstring& label : labels) {
      const std::vector<std::wstring> dropped =
          DroppedBetween(latest[label], Only(after, std::span<const std::wstring>(&label, 1)));
      droppedSince.insert(droppedSince.end(), dropped.begin(), dropped.end());
    }
    Say(Text("dropped since their own refresh: %zu files (%s)", droppedSince.size(),
             NamesText(droppedSince, true).c_str()));
    controlLast_ = Only(after, controls);
    variantsJudged_ = true;

    // Best: the first that passed. Those not decided before it are named. The entries made wrong
    // count by what the cache holds of them.
    size_t oldAfter = 0, goneAfter = 0, madeWrong = 0;
    bool nothingLeft = false;
    for (const auto& [variant, verdict] : verdicts) {
      if (verdict.passA && !best_) best_ = *variant;
      if (verdict.outcome == pdft::test::FolderOutcome::kUndecided && !best_) {
        notDecided_ += (notDecided_.empty() ? "" : "; ") + Utf8(variant->label) +
                       " (the cache dropped " + NamesText(verdict.filesDropped) + ")";
        notDecidedLabels_ += (notDecidedLabels_.empty() ? "" : ", ") + Utf8(variant->label);
      }
      if (verdict.outcome == pdft::test::FolderOutcome::kFail) {
        oldAfter += verdict.oldAfter + verdict.madeWrongOld;
        goneAfter += verdict.missAfter + verdict.madeWrongGone;
        madeWrong += verdict.madeWrong;
        nothingLeft = nothingLeft || verdict.nothingLeft;
      }
    }
    if (best_) {
      Say(Text("Pass A (extraction): yes; best %s (%s)", Utf8(best_->label).c_str(), best_->flags) +
          (notDecided_.empty() ? "" : "; not decided before it: " + notDecided_));
    } else {
      // Where it failed: the old thumbnails stayed, or they went and nothing took their place
      // (also when nothing a refresh stored was left).
      gone_ = nothingLeft || goneAfter > oldAfter;
      if (!notDecided_.empty()) {
        Say("Pass A (extraction): not decided (no variant passed; not decided: " +
            notDecidedLabels_ + ")");
      } else {
        Say(
            Text("Pass A (extraction): no; the thumbnails %s (%zu still old, %zu gone; %zu made "
                 "wrong)",
                 gone_ ? "went without new ones being stored" : "stayed as they were", oldAfter,
                 goneAfter, madeWrong));
      }
    }

    // What Pass A rests on, and what the cache dropped of these folders.
    size_t entries = 0, filesHeld = 0, droppedAfter = droppedSince.size();
    for (const auto& [variant, verdict] : verdicts) {
      entries += verdict.heldAfter;
      filesHeld += verdict.filesHeldAfter;
      droppedAfter += verdict.filesDropped.size();
    }
    droppedAfter += control.filesDropped.size();
    size_t droppedBefore = 0;
    for (const std::wstring& label : labels) {
      for (const NotHeldFile& file :
           NotHeldFiles(Only(baseline_, std::span<const std::wstring>(&label, 1)))) {
        if (file.sizes == "every size") ++droppedBefore;
      }
    }
    restsOn_ = Text(
        "Pass A rests on %zu entries in %zu files of %zu variant%s, and the control on %zu "
        "entries in %zu files; the cache dropped %zu files whole: %zu before the baseline, %zu "
        "since",
        entries, filesHeld, verdicts.size(), verdicts.size() == 1 ? "" : "s", control.heldAfter,
        control.filesHeldAfter, droppedBefore + droppedAfter, droppedBefore, droppedAfter);
    Say(restsOn_);
    (void)CheckUntouched("after the variants");
    return true;
  }

  // ---- By eye ----

  static Crop CropOf(const CacheRecord& record) {
    const std::wstring_view key =
        record.label == kDesktopLabel ? std::wstring_view(kDesktopPageKey) : record.key;
    return pdft::test::ClassifyCrop(key, record.w, record.h, kBaselineZoom, kFlippedZoom);
  }

  // |record|'s entry (its key and size) in |records|; nullptr when it is not there.
  static const CacheRecord* EntryIn(std::span<const CacheRecord> records,
                                    const CacheRecord& record) {
    const auto found =
        std::find_if(records.begin(), records.end(), [&record](const CacheRecord& other) {
          return other.key == record.key && other.size == record.size;
        });
    return found == records.end() ? nullptr : &*found;
  }

  // Just before a round on |label| (or the dialog), said as |name|: its folder asked and judged
  // against the baseline as a control. The files whose judged entries are all held and old are
  // the ones to look at (round->look, with those entries); false when the round is left out.
  bool BeforeRound(const std::wstring& label, const std::string& name, Round* round) {
    bool unopened = false;
    size_t again = 0;
    const std::vector<CacheRecord> now =
        AskHeld(FilesOf(label), name + " before", {}, &again, &unopened);
    const FolderVerdict verdict = JudgeFolder(label, baseline_, now, true, Rules(kFilesPerPage));
    if (verdict.outcome == pdft::test::FolderOutcome::kFail) {
      Say(name + (verdict.newAfter || verdict.otherAfter
                      ? ": not valid (the cache already holds new thumbnails of it)"
                      : ": not valid (the cache dropped some of a file's thumbnails and kept "
                        "others)"));
      return false;
    }
    round->look.clear();
    round->entries.clear();
    for (const std::filesystem::path& file : FilesOf(label)) {
      const std::wstring key = label + L"/" + file.filename().native();
      std::vector<CacheRecord> entries;
      bool all = true;
      for (const CacheRecord& old : baseline_) {
        if (old.key != key || !old.cached || CropOf(old) != Crop::kOld) continue;
        const CacheRecord* held = EntryIn(now, old);
        if (!held || !held->cached || CropOf(*held) != Crop::kOld) {
          all = false;
          break;
        }
        entries.push_back(*held);
      }
      if (!all || entries.empty()) continue;
      round->look.push_back(file);
      round->entries.insert(round->entries.end(), entries.begin(), entries.end());
    }
    if (round->look.empty()) {
      Say(name + ": left out (the cache dropped all its thumbnails)");
      return false;
    }
    return true;
  }

  // The names of the files a round looks at, "portrait-001.pdf and landscape-002.pdf"; empty
  // when they are all of |label|'s.
  std::string LookNames(const std::wstring& label, const Round& round) {
    if (round.look.size() == FilesOf(label).size()) return {};
    std::vector<std::wstring> names;
    for (const std::filesystem::path& file : round.look) names.push_back(file.filename().native());
    return NamesText(names, true, true);
  }

  // The question whether the files show the old crop, naming the ones to look at; |names| is
  // LookNames, and the line before it says why.
  static std::string OldCropQuestion(const std::string& names) {
    if (!names.empty()) {
      std::printf(
          "Look only at %s: the cache dropped the others' thumbnails, so File Explorer draws "
          "those at the new zoom at once.\n",
          names.c_str());
    }
    return "Do " + (names.empty() ? std::string("the files") : names) +
           " show whole pages, with white at the sides of the tall ones and above and below the "
           "wide ones?";
  }

  // After a change that is a refresh: each file looked at must be held and new at every size it
  // was judged at, or the answers count for nothing (said as |name|): the refresh did not make
  // it again. False then.
  bool MadeAgain(const std::string& name, const Round& round) {
    if (round.look.empty()) return true;
    bool unopened = false;
    size_t again = 0;
    const std::vector<CacheRecord> now =
        AskHeld(round.look, name + " after", {}, &again, &unopened);
    std::vector<std::wstring> dropped, old;
    for (const CacheRecord& entry : round.entries) {
      const CacheRecord* held = EntryIn(now, entry);
      std::vector<std::wstring>& into = !held || !held->cached ? dropped : old;
      if (held && held->cached && CropOf(*held) == Crop::kNew) continue;
      if (std::find(into.begin(), into.end(), entry.key) == into.end()) into.push_back(entry.key);
    }
    if (!dropped.empty()) {
      Say(name + ": not valid (the cache dropped " + NamesText(dropped, false, true) +
          " during the round, so the refresh did not make it again)");
    } else if (!old.empty()) {
      Say(name + ": not valid (the refresh did not make " + NamesText(old, false, true) +
          " again)");
    }
    return dropped.empty() && old.empty();
  }

  // After the round that makes nothing: what the cache answers for the files looked at, recorded
  // only.
  void RecordAfterTouch(const std::wstring& label, const Round& round) {
    if (round.look.empty()) return;
    bool unopened = false;
    size_t again = 0;
    const std::vector<CacheRecord> now =
        AskHeld(round.look, "round " + Utf8(label) + " after", {}, &again, &unopened);
    size_t notHeld = 0, newId = 0, old = 0, fresh = 0;
    for (const CacheRecord& entry : round.entries) {
      const CacheRecord* held = EntryIn(now, entry);
      if (!held || !held->cached) {
        ++notHeld;
        continue;
      }
      if (held->id != entry.id) ++newId;
      const Crop crop = CropOf(*held);
      if (crop == Crop::kOld) ++old;
      if (crop == Crop::kNew) ++fresh;
    }
    Say(Text("round %s after: %zu not held, %zu under a new ID, %zu old, %zu new",
             Utf8(label).c_str(), notHeld, newId, old, fresh));
  }

  // The user shows |label|'s folder in their test window at Large icons and says whether it shows
  // the old crop; then |change| runs, and they say when the thumbnails turned square. Only the
  // files the cache still holds at the old zoom are looked at.
  bool RunRound(const std::wstring& label, const char* what, const std::function<bool()>& change,
                Round* round) {
    if (Excluded(label)) {
      Say(Text("round %s: left out (no valid baseline)", Utf8(label).c_str()));
      return true;
    }
    if (!BeforeRound(label, "round " + Utf8(label), round)) return true;
    round->run = true;
    windowUsed_ = true;
    std::printf(
        "\nRound %s (%s).\nPaste this path into the address bar of ONE File Explorer window "
        "(Win+E opens one), then press Ctrl+Shift+2 (Large icons):\n  %s\n",
        Utf8(label).c_str(), what, Utf8(Folder(label).native()).c_str());
    const Answer shown = AskYesNo(OldCropQuestion(LookNames(label, *round)));
    if (shown == Answer::kStop) return StopByUser();
    if (shown == Answer::kNo) {
      Say(Text("round %s (%s): not valid (File Explorer made new thumbnails by itself)",
               Utf8(label).c_str(), what));
      return true;
    }
    round->valid = true;
    std::printf("Watch the window now.\n");
    std::fflush(stdout);
    if (!change()) return false;
    const ULONGLONG start = PreciseMs();
    double seconds = 0;
    const Answer square = TimeSquare("in the window", start, &seconds);
    if (square == Answer::kStop) return StopByUser();
    if (square == Answer::kYes) round->seconds = seconds;
    round->square = square == Answer::kYes && seconds <= kSquareWithinSeconds;
    Say(Text(
        "round %s (%s): %s", Utf8(label).c_str(), what,
        square == Answer::kYes
            ? Text("square after %.1f s%s", seconds, round->square ? "" : " (too late)").c_str()
            : "not square within 10 s"));
    return true;
  }

  // Refreshes |label| with the best extraction and |notify|.
  bool RefreshLabel(const std::wstring& label, unsigned notify) {
    pdft::RefreshCounts counts;
    HRESULT hr = S_OK;
    if (!Refresh(FilesOf(label), BestOptions(notify), *cache_, &counts, &hr)) return false;
    Note(Text("  %s: %s", Utf8(label).c_str(), CountsText(hr, counts).c_str()));
    return true;
  }

  // The last-write time of the experiment's own files in |label| moved on 2 s (through their own
  // handles), then SHCNE_UPDATEITEM per file; no thumbnail is made.
  bool Touch(const std::wstring& label) {
    size_t touched = 0;
    for (const OwnFile& own : ownFiles_) {
      if (own.path.parent_path() != Folder(label)) continue;
      wil::unique_hfile file;
      if (OpenOwnFile(own, FILE_WRITE_ATTRIBUTES, std::addressof(file)) != S_OK) continue;
      FILETIME written = {};
      if (!GetFileTime(file.get(), nullptr, nullptr, &written)) continue;
      const ULONGLONG later =
          ((static_cast<ULONGLONG>(written.dwHighDateTime) << 32) | written.dwLowDateTime) +
          kTouch100ns;
      const FILETIME moved = {static_cast<DWORD>(later), static_cast<DWORD>(later >> 32)};
      if (!SetFileTime(file.get(), nullptr, nullptr, &moved)) continue;
      file.reset();
      ++touched;
      pdft::RealShellNotifier().ItemUpdated(own.path);
    }
    Note(Text("  %s: %zu files touched", Utf8(label).c_str(), touched));
    return true;
  }

  bool ShowDialog(const std::filesystem::path& folder, wil::unique_process_information* dialog) {
    const HRESULT hr = StartOpenDialog(options_.self, folder, dialog);
    if (FAILED(hr)) {
      Say("  the File Open dialog could not be started: " + Hr(hr));
      return false;
    }
    if (WaitForSingleObject(dialog->hProcess, kDialogStartMs) == WAIT_OBJECT_0) {
      DWORD exitCode = 0;
      (void)GetExitCodeProcess(dialog->hProcess, &exitCode);
      Say(Text("  the File Open dialog's process ended at once (exit %lu)", exitCode));
      return false;
    }
    return true;
  }

  // Asks the user to close the dialog and waits a while for its process.
  bool CloseDialog(wil::unique_process_information* dialog) {
    const bool go = WaitForEnter("Close the Open dialog, then press Enter.");
    if (WaitForSingleObject(dialog->hProcess, kDialogCloseMs) != WAIT_OBJECT_0) {
      Say("  the Open dialog is still open; it ends when it is closed");
    }
    return go || StopByUser();
  }

  // A ladder step that acts on the window itself turned it square: whether the product may do
  // that to every open window, by the user's answer.
  bool AcceptSideEffects() {
    const Answer accept = AskYesNo(
        "Would it be all right if every refresh did what you just saw to the open File Explorer "
        "windows?");
    if (accept == Answer::kStop) return StopByUser();
    sideEffectsAccepted_ = accept == Answer::kYes;
    Say(Text("  its side effects %s", *sideEffectsAccepted_ ? "accepted" : "NOT accepted"));
    return true;
  }

  // The ladder, on a notified round whose window stayed old: one step at a time until one turns
  // it square.
  bool Ladder(const std::wstring& label) {
    Say(Text("ladder on %s", Utf8(label).c_str()));
    const std::filesystem::path folder = Folder(label);
    size_t acted = 0;
    HRESULT hr = RefreshViewsOf(folder, &acted);
    Say(Text("  views refreshed (IShellView::Refresh): %zu (%s)", acted, Hr(hr).c_str()));
    Answer answer = AskYesNo("Square now?");
    if (answer == Answer::kStop) return StopByUser();
    const Answer kept = AskYesNo("Did the window keep its selection and its scroll position?");
    if (kept == Answer::kStop) return StopByUser();
    viewsKept_ = kept == Answer::kYes;
    Say(Text("  %s after the views were refreshed; selection and scroll %s",
             answer == Answer::kYes ? "square" : "still old", *viewsKept_ ? "kept" : "NOT kept"));
    if (answer == Answer::kYes) {
      ladderStep_ = Mechanism::kViews;
      return AcceptSideEffects();
    }
    hr = BrowseAgain(folder, &acted);
    Say(Text("  views browsed to their folder again: %zu (%s)", acted, Hr(hr).c_str()));
    answer = AskYesNo("Square now?");
    if (answer == Answer::kStop) return StopByUser();
    if (answer == Answer::kYes) {
      ladderStep_ = Mechanism::kBrowse;
      Say("  square after browsing again");
      return AcceptSideEffects();
    }
    wil::unique_process_information dialog;
    if (ShowDialog(folder, &dialog)) {
      answer = AskYesNo(
          "In the Open dialog press Ctrl+Shift+2 (Large icons). Are the thumbnails square there?");
      if (answer == Answer::kStop) return StopByUser();
      dialogFresh_ = answer == Answer::kYes;
      Say(Text("  another program's dialog: %s", *dialogFresh_ ? "square" : "old"));
      if (!CloseDialog(&dialog)) return false;
    }
    answer = AskYesNo("Press F5 in the File Explorer window. Square now?");
    if (answer == Answer::kStop) return StopByUser();
    f5Works_ = answer == Answer::kYes;
    Say(Text("  F5 by hand: %s", *f5Works_ ? "square" : "still old"));
    return true;
  }

  // |mechanism| turned a window square: it is the one kept unless a cheaper one did too (the
  // order of Mechanism: the cheapest and least invasive first).
  void Found(Mechanism mechanism) {
    if (!mechanism_ || mechanism < *mechanism_) mechanism_ = mechanism;
  }

  bool Rounds() {
    Say("");
    Say(Text("Rounds judged by eye, with the best extraction (%s)", best_->flags));
    const std::pair<const wchar_t*, Mechanism> notified[] = {{kNoneLabel, Mechanism::kNone},
                                                             {kItemLabel, Mechanism::kItem},
                                                             {kDirLabel, Mechanism::kDir}};
    bool ladderRun = false;
    for (const auto& [label, mechanism] : notified) {
      Round& round = rounds_[label];
      const Mechanism m = mechanism;
      if (!RunRound(
              label, MechanismText(m),
              [this, label = std::wstring(label), m] { return RefreshLabel(label, NotifyOf(m)); },
              &round)) {
        return false;
      }
      bool ladderHere = false;
      if (m != Mechanism::kNone && round.valid && !round.square && !ladderRun) {
        ladderRun = true;
        ladderHere = true;
        if (!Ladder(label)) return false;
      }
      // The answers, the ladder's too, count only when the refresh made the files again.
      if (round.valid && !MadeAgain("round " + Utf8(label), round)) {
        round.valid = false;
        round.square = false;
        if (ladderHere) ladderStep_.reset();
      }
      if (round.square) Found(m);
      if (ladderHere && ladderStep_) Found(*ladderStep_);
    }
    // Neither notification worked: whether the file's identity in the cache matters (diagnostic:
    // the experiment's own files only). It makes no thumbnail, so what the cache answers
    // afterwards is recorded only.
    if (!rounds_[kItemLabel].square && !rounds_[kDirLabel].square) {
      Round& touch = rounds_[kTouchLabel];
      if (!RunRound(
              kTouchLabel, "last write moved on 2 s, then SHCNE_UPDATEITEM, no extraction",
              [this] { return Touch(kTouchLabel); }, &touch)) {
        return false;
      }
      if (touch.valid) RecordAfterTouch(kTouchLabel, touch);
    }
    // Nothing turned a window square so far: associations changed after the update (global, so
    // last).
    if (!mechanism_) {
      Round& assoc = rounds_[kAssocLabel];
      if (!RunRound(
              kAssocLabel, MechanismText(Mechanism::kAssoc),
              [this] { return RefreshLabel(kAssocLabel, pdft::kNotifyAssociations); }, &assoc)) {
        return false;
      }
      if (assoc.valid && !MadeAgain("round " + Utf8(kAssocLabel), assoc)) {
        assoc.valid = false;
        assoc.square = false;
      }
      if (assoc.square) Found(Mechanism::kAssoc);
    }
    Say(mechanism_ ? Text("mechanism that turned a window square: %s", MechanismText(*mechanism_))
                   : std::string("no mechanism turned a window square"));
    return true;
  }

  // The best extraction with |mechanism| on |files| of |folder|.
  bool Apply(Mechanism mechanism, std::span<const std::filesystem::path> files,
             const std::filesystem::path& folder, bool desktop) {
    pdft::RefreshCounts counts;
    HRESULT hr = S_OK;
    if (!Refresh(files, BestOptions(NotifyOf(mechanism)), *cache_, &counts, &hr)) return false;
    Note("  " + CountsText(hr, counts));
    if (mechanism == Mechanism::kBrowse && !desktop) {
      size_t browsed = 0;
      (void)BrowseAgain(folder, &browsed);
    }
    return true;
  }

  // With the mechanism found: the Desktop (when the test PDF is there) and another program's File
  // Open dialog.
  bool DesktopAndDialog() {
    if (!mechanism_) {
      Say("Desktop and dialog: not checked (no mechanism turned a window square)");
      return true;
    }
    Say("");
    Say(Text("Desktop and another program's dialog, with %s", MechanismText(*mechanism_)));
    if (desktopFile_) {
      // Asked just before its question: it counts only while the cache holds it at the old zoom.
      Round desktop;
      desktop.look = {*desktopFile_};
      bool unopened = false;
      size_t again = 0;
      const std::vector<CacheRecord> now =
          AskHeld(desktop.look, "the Desktop file", {}, &again, &unopened);
      bool held = false, notOld = false;
      for (const CacheRecord& record : now) {
        if (!record.cached) continue;
        held = true;
        if (CropOf(record) == Crop::kOld) {
          desktop.entries.push_back(record);
        } else {
          notOld = true;
        }
      }
      if (!held) {
        Say("Desktop: left out (the cache dropped its thumbnail)");
      } else if (notOld) {
        Say("Desktop: not valid (the cache already holds a new thumbnail of it)");
      } else {
        Answer answer = AskYesNo(
            "Look at the Desktop (Win+D, then Win+D again to come back). Is PdfThumbnails-"
            "refresh-test a whole page, narrow with white at the sides?");
        if (answer == Answer::kStop) return StopByUser();
        if (answer == Answer::kNo) {
          Say("Desktop: not valid (it did not show the old crop)");
        } else {
          desktopChecked_ = true;
          std::printf("Look at the Desktop now (Win+D).\n");
          const std::filesystem::path file = *desktopFile_;
          if (!Apply(*mechanism_, std::span<const std::filesystem::path>(&file, 1),
                     file.parent_path(), true)) {
            return false;
          }
          double seconds = 0;
          answer = TimeSquare("on the Desktop", PreciseMs(), &seconds);
          if (answer == Answer::kStop) return StopByUser();
          desktopSquare_ = answer == Answer::kYes && seconds <= kSquareWithinSeconds;
          Say(answer == Answer::kYes ? Text("Desktop: square after %.1f s", seconds)
                                     : std::string("Desktop: not square within 10 s"));
          if (!MadeAgain("Desktop", desktop)) {
            desktopChecked_ = false;
            desktopSquare_ = false;
          }
        }
      }
    }
    if (Excluded(kDialogLabel)) {
      Say("dialog: left out (no valid baseline)");
      return true;
    }
    Round round;
    if (!BeforeRound(kDialogLabel, "dialog", &round)) return true;
    wil::unique_process_information dialog;
    if (!ShowDialog(Folder(kDialogLabel), &dialog)) return true;
    std::printf("In the Open dialog press Ctrl+Shift+2 (Large icons).\n");
    Answer answer = AskYesNo(OldCropQuestion(LookNames(kDialogLabel, round)));
    if (answer == Answer::kStop) return StopByUser();
    if (answer == Answer::kNo) {
      Say("dialog: not valid (it did not show the old crop)");
    } else {
      dialogChecked_ = true;
      std::printf("Watch the Open dialog now.\n");
      if (!Apply(*mechanism_, FilesOf(kDialogLabel), Folder(kDialogLabel), false)) return false;
      double seconds = 0;
      answer = TimeSquare("in the Open dialog", PreciseMs(), &seconds);
      if (answer == Answer::kStop) return StopByUser();
      dialogSquare_ = answer == Answer::kYes && seconds <= kSquareWithinSeconds;
      Say(answer == Answer::kYes ? Text("dialog: square after %.1f s", seconds)
                                 : std::string("dialog: not square within 10 s"));
      if (!MadeAgain("dialog", round)) {
        dialogChecked_ = false;
        dialogSquare_ = false;
      }
    }
    return CloseDialog(&dialog);
  }

  // ---- Timing ----

  // The timing part. Without the user the timing fill runs also without a best variant; the
  // refreshes and the stop test need one. With the user, the untouched files decide first: once
  // the cache has dropped one of them, many more stores would push out more of the user's own,
  // and with none of them held after its store nothing would show it.
  bool Timing() {
    Say("");
    const size_t lost = CheckUntouched("before the timing fill");
    if (options_.timingFiles == 0) {
      Say("Timing: left out (--timing-files 0)");
    } else if (options_.interactive && Watched() == 0) {
      Say(
          Text("Timing: left out (none of the %zu thumbnails stored once was held right after its "
               "store, so what the cache drops of yours could not be watched)",
               untouched_.size()));
    } else if (options_.interactive && lost > 0) {
      // The stores the timing part would make: the fill at kTimingSize, then each file made
      // again once, at its largest size held (the smaller ones come with that store), the first
      // ones once more in a thumbnail process of their own where that pass runs.
      const size_t files = static_cast<size_t>(options_.timingFiles);
      const size_t instance =
          InstancePassRuns() ? std::min(files, static_cast<size_t>(options_.instanceFiles)) : 0;
      Say(Text(
          "Timing: left out (the cache dropped %zu of the %zu thumbnails stored once and never "
          "touched, so %zu more stores would push out more of yours)",
          lost, Watched(), files + files + instance));
    } else {
      Phase("the timing fill");
      if (!FillTiming() || (best_ && !TimeRefresh())) return false;
    }
    (void)CheckUntouched("after the timing part");
    return !best_ || StopTest();
  }

  // Whether the own-process pass runs: the product's refresh takes its extraction.
  static bool InstancePassRuns() { return RefreshAllows(kInstanceExtractFlags, std::nullopt); }

  // The untouched files held right after their store.
  size_t Watched() const {
    return static_cast<size_t>(std::count_if(untouched_.begin(), untouched_.end(),
                                             [](const Untouched& file) { return file.watched; }));
  }

  // The timing folder filled at one size only, once the variants were judged, so that its
  // entries go into the cache after theirs; then how much of the control folder is still held
  // (what it lost here, the timing fill pushed out), and of the timing files.
  bool FillTiming() {
    size_t made = 0, failed = 0;
    const std::vector<std::filesystem::path>& timing = FilesOf(kTimingLabel);
    for (size_t i = 0; i < timing.size(); ++i) {
      pdft::CacheAnswer answer;
      HRESULT hr = cache_->Select(timing[i]);
      if (SUCCEEDED(hr)) hr = cache_->Get(kTimingSize, kPopulateFlags, &answer);
      if (Blocked(hr)) return StopIfBlocked(hr);
      if (SUCCEEDED(hr) && answer.bitmap) {
        ++made;
      } else {
        ++failed;
      }
      if ((i + 1) % 500 == 0) {
        std::printf("  timing: %zu of %zu\n", i + 1, timing.size());
        std::fflush(stdout);
      }
    }
    Say(Text("timing fill at %u: %zu made, %zu failed", kTimingSize, made, failed));
    Say("cache files after the timing fill: " + CacheFilesText());
    bool unopened = false;
    size_t again = 0;
    const std::vector<CacheRecord> control =
        AskHeld(FilesOf(kControlLabel), "the control folder", {}, &again, &unopened);
    SaveRecords(L"control-after-timing-fill.tsv", control);
    // Against what it held after the variants.
    size_t heldBefore = 0, stillHeld = 0;
    for (const CacheRecord& record : controlLast_) {
      if (!record.cached) continue;
      ++heldBefore;
      const CacheRecord* now = EntryIn(control, record);
      if (now && now->cached) ++stillHeld;
    }
    const std::vector<std::wstring> dropped = DroppedBetween(controlLast_, control);
    Say(Text(
        "control after the timing fill: %zu of the %zu entries held after the variants still "
        "held%s; dropped whole since: %s",
        stillHeld, heldBefore, again ? Text(" (%zu only when asked again)", again).c_str() : "",
        NamesText(dropped).c_str()));
    SayNotHeld(control);
    controlLast_ = control;
    // A record dropped loses every size at once: the timing files are asked at their one size.
    const UINT sizes[] = {kTimingSize};
    const std::vector<CacheRecord> timingHeld =
        AskCache(*cache_, timing, sizes, pdft::kRefreshProbeFlags, std::nullopt, false, &unopened);
    timingHeld_ = HeldFiles(timingHeld);
    Say(Text("timing files held at %u after the timing fill: %zu of %zu", kTimingSize,
             timingHeld_.size(), timing.size()));
    return true;
  }

  // The timing folder refreshed while the contention probe runs, then its first files each in a
  // thumbnail process of their own.
  bool TimeRefresh() {
    Phase("the timing refresh");
    Say(Text("Timing: %ld files cached at %u, with the best extraction", options_.timingFiles,
             kTimingSize));
    size_t failed = 0;
    ContentionProbe idle(probeFile_);
    idle.Start();
    Sleep(kIdleMs);
    const std::vector<DWORD> idleTimes = idle.Stop(&failed);
    Say("contention probe idle (one thumbnail at 96 every 500 ms, not stored): " +
        TimesText(idleTimes) + (failed ? Text(", %zu failed", failed) : ""));

    // The product's notification without the user (or none found), else the mechanism found.
    const unsigned notify =
        options_.interactive && mechanism_ ? NotifyOf(*mechanism_) : pdft::kRefreshNotify;
    TimedCache timed(*cache_);
    std::vector<DWORD> extractions;
    pdft::RefreshFilesOptions options = BestOptions(notify);
    options.extractionMs = &extractions;
    ContentionProbe during(probeFile_);
    during.Start();
    const ULONGLONG start = PreciseMs();
    pdft::RefreshCounts counts;
    HRESULT hr = S_OK;
    const bool go = Refresh(FilesOf(kTimingLabel), options, timed, &counts, &hr);
    const ULONGLONG total = PreciseMs() - start;
    const std::vector<DWORD> duringTimes = during.Stop(&failed);
    if (!go) return false;
    Say(Text("timing: %s; %llu ms in all", CountsText(hr, counts).c_str(), total));
    Say("  questions: " + TimesText(timed.questions));
    Say("  extractions: " + TimesText(extractions));
    SaveText(L"timing.tsv", TimingText(extractions));
    contention_ = Mean(idleTimes) > 0 ? Mean(duringTimes) / Mean(idleTimes) : 0;
    Say("contention probe during it: " + TimesText(duringTimes) +
        (failed ? Text(", %zu failed", failed) : "") +
        Text("; mean %.1f times the idle one", contention_));

    // The first files again, each in a thumbnail process of its own, where the refresh allows it.
    const std::vector<std::filesystem::path>& timing = FilesOf(kTimingLabel);
    if (!InstancePassRuns()) {
      Say(Text("own thumbnail process (INSTANCESURROGATE): left out (%s)", kSurrogateOnly));
    } else {
      Phase("the own-process pass");
      const std::span<const std::filesystem::path> first(
          timing.data(), std::min(static_cast<size_t>(options_.instanceFiles), timing.size()));
      std::vector<DWORD> instanceExtractions;
      pdft::RefreshFilesOptions instance = BestOptions(0);
      instance.extractFlags = kInstanceExtractFlags;
      instance.storeFlags.reset();
      instance.extractionMs = &instanceExtractions;
      ContentionProbe duringInstance(probeFile_);
      duringInstance.Start();
      const ULONGLONG instanceStart = PreciseMs();
      const bool instanceGo = Refresh(first, instance, *cache_, &counts, &hr);
      const ULONGLONG instanceTotal = PreciseMs() - instanceStart;
      const std::vector<DWORD> instanceTimes = duringInstance.Stop(&failed);
      if (!instanceGo) return false;
      Say(Text("own thumbnail process (INSTANCESURROGATE), first %zu files: %s; %llu ms in all",
               first.size(), CountsText(hr, counts).c_str(), instanceTotal));
      Say("  extractions: " + TimesText(instanceExtractions));
      Say("  contention probe during it: " + TimesText(instanceTimes));
    }

    // What the cache dropped whole across them: the timing files at their one size, and the
    // control.
    bool unopened = false;
    size_t again = 0;
    const UINT sizes[] = {kTimingSize};
    const std::vector<std::wstring> timingNow = HeldFiles(
        AskCache(*cache_, timing, sizes, pdft::kRefreshProbeFlags, std::nullopt, false, &unopened));
    size_t timingDropped = 0;
    for (const std::wstring& key : timingHeld_) {
      if (std::find(timingNow.begin(), timingNow.end(), key) == timingNow.end()) ++timingDropped;
    }
    const std::vector<CacheRecord> control =
        AskHeld(FilesOf(kControlLabel), "the control folder", {}, &again, &unopened);
    const std::vector<std::wstring> controlDropped = DroppedBetween(controlLast_, control);
    controlLast_ = control;
    Say(
        Text("dropped whole across the timing refresh: %zu of %zu timing files, %zu control files "
             "(%s)",
             timingDropped, timingHeld_.size(), controlDropped.size(),
             NamesText(controlDropped).c_str()));
    return true;
  }

  // How fast a refresh of the slow folder stops, and whether the cache answers consistently after
  // the stop: each thumbnail of a heavy page still cached, old or new. The largest pages are square
  // at both zooms (the judge does not know them by their names), so theirs only have to be still
  // cached. A file the stopped refresh never reached (it takes them in order) that the cache
  // dropped whole is counted, and is no inconsistency.
  bool StopTest() {
    Phase("the stop test");
    // The slow folder as the cache holds it just before the stop: what the timing fill and the
    // refreshes since the baseline pushed out is not the stop's doing.
    bool unopened = false;
    size_t again = 0;
    const std::vector<CacheRecord> slowBefore =
        AskHeld(FilesOf(kSlowLabel), "the slow folder before the stop", {}, &again, &unopened);
    SaveRecords(L"slow-before-stop.tsv", slowBefore);
    const auto held = [](std::span<const CacheRecord> records) {
      return static_cast<size_t>(std::count_if(
          records.begin(), records.end(), [](const CacheRecord& record) { return record.cached; }));
    };
    Say(Text("the slow folder before the stop: %zu of %zu held (%zu at the baseline)",
             held(slowBefore), slowBefore.size(), held(slowBefore_)));

    ULONGLONG asked = 0, returned = 0;
    pdft::RefreshCounts counts;
    HRESULT blocked = S_OK;
    const HRESULT hr = RefreshStoppedAfter(FilesOf(kSlowLabel), BestOptions(0), *cache_,
                                           kCancelAfterMs, &counts, &asked, &returned, &blocked);
    if (!StopIfBlocked(blocked)) return false;
    if (asked) {
      Say(
          Text("stop asked %lu ms into the slow folder's refresh: it returned %llu ms later (%s, "
               "%zu made again)",
               kCancelAfterMs, returned > asked ? returned - asked : 0,
               counts.stopped ? "stopped" : "had finished", counts.refreshed));
    } else {
      Say(Text("the slow folder's refresh ended before the stop was asked (%s)",
               CountsText(hr, counts).c_str()));
    }
    const std::vector<CacheRecord> slowAfter =
        AskHeld(FilesOf(kSlowLabel), "the slow folder after the stop", {}, &again, &unopened);
    SaveRecords(L"slow-after-stop.tsv", slowAfter);
    // The files the refresh reached: the first counts.files of the folder.
    const std::vector<std::filesystem::path>& slow = FilesOf(kSlowLabel);
    std::set<std::wstring> reached;
    for (size_t i = 0; i < counts.files && i < slow.size(); ++i) {
      reached.insert(std::wstring(kSlowLabel) + L"/" + slow[i].filename().native());
    }
    std::vector<std::wstring> droppedWhole;
    for (const std::wstring& key : DroppedBetween(slowBefore, slowAfter)) {
      if (!reached.count(key)) droppedWhole.push_back(key);
    }
    size_t heavyBefore = 0, heavyNew = 0, heavyOld = 0, heavyGone = 0, heavyOther = 0;
    size_t heavyDropped = 0, largestBefore = 0, largestGone = 0;
    for (const CacheRecord& record : slowBefore) {
      if (!record.cached) continue;
      const CacheRecord* now = EntryIn(slowAfter, record);
      const bool stillHeld = now && now->cached;
      const bool dropped =
          std::find(droppedWhole.begin(), droppedWhole.end(), record.key) != droppedWhole.end();
      if (IsLargest(record)) {
        ++largestBefore;
        if (!stillHeld && !dropped) ++largestGone;
        continue;
      }
      ++heavyBefore;
      if (!stillHeld) {
        ++(dropped ? heavyDropped : heavyGone);
        continue;
      }
      switch (CropOf(*now)) {
        case Crop::kNew:
          ++heavyNew;
          break;
        case Crop::kOld:
          ++heavyOld;
          break;
        case Crop::kOther:
          ++heavyOther;
          break;
        case Crop::kIndistinguishable:
          break;
      }
    }
    const bool consistent = heavyGone == 0 && heavyOther == 0 && largestGone == 0;
    Say(
        Text("after the stop: heavy pages %zu new, %zu old, %zu gone, %zu neither, %zu dropped "
             "with their file, of %zu held before; largest pages %zu gone of %zu held before; %zu "
             "files dropped whole, which the refresh had not reached (%s)",
             heavyNew, heavyOld, heavyGone, heavyOther, heavyDropped, heavyBefore, largestGone,
             largestBefore, droppedWhole.size(), consistent ? "consistent" : "NOT consistent"));
    return true;
  }

  static std::string TimingText(const std::vector<DWORD>& times) {
    std::string text = "n\tms\n";
    for (size_t n = 0; n < times.size(); ++n) {
      text += std::to_string(n + 1) + "\t" + std::to_string(times[n]) + "\n";
    }
    return text;
  }

  // ---- The product, from a Windows Terminal tab ----

  bool ProductCheck() {
    Say("");
    Phase("the product check");
    Say("Product check: --refresh --folder from a Windows Terminal tab");
    bool unopened = false;
    size_t again = 0;
    // The control folder must still be old now, or a pass after the command would not be its own.
    // From here on it is the folder refreshed: one page of each kind held at every size will do.
    const std::vector<CacheRecord> before =
        AskHeld(FilesOf(kControlLabel), "the control folder", {}, &again, &unopened);
    SaveRecords(L"control-before-cli.tsv", before);
    const FolderVerdict still =
        JudgeFolder(kControlLabel, baseline_, before, true, Rules(kFilesPerPage));
    Say(VerdictText(still, kBaselineZoom, " before the command"));
    if (still.outcome != pdft::test::FolderOutcome::kPass) {
      productDropped_ = still.outcome != pdft::test::FolderOutcome::kFail;
      productLeftOut_ =
          productDropped_ ? "the cache dropped too many of its thumbnails" : "it changed";
      Say("left out (" + *productLeftOut_ + "): Pass A is not complete");
      return true;
    }
    const std::string cli = Utf8(options_.cli.native());
    const std::string control = Utf8(Folder(kControlLabel).native());
    std::printf(
        "Open a Windows Terminal tab with + and run, in PowerShell (one line):\n"
        "  & '%s' --refresh --folder '%s'; \"exit=$LASTEXITCODE\"\n"
        "or in a Command Prompt (two commands, the second once the first has ended):\n"
        "  \"%s\" --refresh --folder \"%s\"\n"
        "  echo exit=%%ERRORLEVEL%%\n"
        "Then come back here.\n",
        cli.c_str(), control.c_str(), cli.c_str(), control.c_str());
    std::optional<long> code;
    if (!AskExitCode(&code)) return StopByUser();
    Say(code ? Text("the command's exit code, as typed: %ld", *code)
             : std::string("the command's exit code: not known"));
    const std::vector<CacheRecord> after = AskHeld(
        FilesOf(kControlLabel), "the control folder after the command", {}, &again, &unopened);
    SaveRecords(L"control-after-cli.tsv", after);
    // The entries held and old just before, judged as the folder refreshed: only exit code 0
    // (every extraction succeeded) lets a file the cache dropped whole count neither way.
    const FolderVerdict verdict =
        JudgeFolder(kControlLabel, before, after, false, Rules(kFilesPerPage, code == 0L));
    // The line names the exit code instead of the refresh's failures: cancelled, busy or not
    // known says nothing of the extractions, so the folder is not decided then.
    const bool rerun = !code || *code == static_cast<long>(pdft::ExitCode::kCancelled) ||
                       *code == static_cast<long>(pdft::ExitCode::kBusy);
    FolderVerdict shown = verdict;
    shown.refreshFailed = false;
    if (rerun && shown.outcome == pdft::test::FolderOutcome::kFail) {
      shown.outcome = pdft::test::FolderOutcome::kUndecided;
    }
    std::string line = VerdictText(shown, kBaselineZoom, " after the command");
    if (code != 0L) line += code ? Text("; exit %ld", *code) : std::string("; exit code not known");
    Say(line);
    if (code == 0L) {
      product_ = verdict.outcome == pdft::test::FolderOutcome::kUndecided
                     ? pdft::test::FolderOutcome::kUndecided
                 : verdict.passA ? pdft::test::FolderOutcome::kPass
                                 : pdft::test::FolderOutcome::kFail;
      productDropped_ = *product_ == pdft::test::FolderOutcome::kUndecided;
      productText_ = *product_ == pdft::test::FolderOutcome::kPass ? std::string("yes")
                     : productDropped_
                         ? "not decided (the cache dropped " + NamesText(verdict.filesDropped) + ")"
                         : std::string("no");
    } else if (rerun) {
      // The command has to run again.
      product_ = pdft::test::FolderOutcome::kUndecided;
      productText_ = !code ? "not decided (the exit code is not known: run the command again)"
                     : *code == static_cast<long>(pdft::ExitCode::kCancelled)
                         ? "not decided (exit 3, cancelled: run the command again)"
                         : "not decided (exit 4, busy: run the command again)";
    } else {
      product_ = pdft::test::FolderOutcome::kFail;
      productText_ = Text("no (exit %ld)", *code);
    }
    Say("Pass A through the command-line executable: " + productText_);
    size_t count = 0;
    const HRESULT hr = CountTerminalCacheFiles(&count);
    terminalCacheFiles_ = count;
    Say(Text("thumbcache files in Windows Terminal's private folders: %zu%s", count,
             FAILED(hr) ? (" (not all readable: " + Hr(hr) + ")").c_str() : ""));
    return true;
  }

  // ---- The copy progress window ----

  bool CopyCheck() {
    Say("");
    Say("Copy check: the class of File Explorer's copy progress window");
    ULARGE_INTEGER free = {};
    if (!GetDiskFreeSpaceExW(root_.c_str(), &free, nullptr, nullptr) ||
        free.QuadPart < kCopyCheckFreeBytes) {
      Say("left out (not enough free space for the file and its copy)");
      return true;
    }
    const std::filesystem::path file = root_ / kCopyCheckName;
    const std::string chunk(kCopyCheckChunk, '\0');
    OwnFile own;
    std::printf("Writing a 1 GiB file...\n");
    std::fflush(stdout);
    HRESULT hr =
        WriteOwnFile(file, chunk, static_cast<size_t>(kCopyCheckBytes / kCopyCheckChunk), &own);
    if (FAILED(hr)) {
      Say("left out (the file cannot be written: " + Hr(hr) + ")");
      return true;
    }
    ownFiles_.push_back(std::move(own));
    if (!MakeFolder(root_ / kCopyFolderName)) return false;
    copyFolderMade_ = true;
    windowUsed_ = true;
    std::printf("In File Explorer, copy %s (Ctrl+C) and paste it into %s (Ctrl+V).\n",
                Utf8(file.native()).c_str(), Utf8((root_ / kCopyFolderName).native()).c_str());
    if (!WaitForEnter("Press Enter while the copy's progress window shows.")) return StopByUser();
    std::set<DWORD> pids;
    if (const std::optional<std::vector<ExplorerProcess>> now = ExplorersNow()) {
      for (const ExplorerProcess& process : *now) pids.insert(process.pid);
    }
    const std::set<std::string> classes = WindowClassesOf(pids);
    std::string names;
    for (const std::string& name : classes) names += (names.empty() ? "" : ", ") + name;
    operationStatus_ = classes.count(kOperationStatusClass) > 0;
    Say("visible File Explorer window classes: " + (names.empty() ? std::string("none") : names));
    Say(Text("%s among them: %s", kOperationStatusClass, *operationStatus_ ? "yes" : "no"));
    if (!WaitForEnter("Let the copy finish (or cancel it), then press Enter.")) return StopByUser();
    return true;
  }

  // ---- The end ----

  // File Explorer's processes from the start are still running.
  void CheckNoRestart() {
    if (!explorersBefore_) {
      Say("File Explorer restarted: unknown (its processes could not be listed at the start)");
      return;
    }
    const std::optional<std::vector<ExplorerProcess>> now = ExplorersNow();
    if (!now) {
      Say("File Explorer restarted: unknown (its processes cannot be listed)");
      return;
    }
    size_t same = 0;
    for (const ExplorerProcess& before : *explorersBefore_) {
      if (std::any_of(now->begin(), now->end(), [&before](const ExplorerProcess& process) {
            return process.pid == before.pid &&
                   (!before.started || !process.started || process.started == before.started);
          })) {
        ++same;
      }
    }
    noRestart_ = same == explorersBefore_->size();
    Say(
        Text("File Explorer not restarted: %s (%zu of the %zu explorer.exe processes from the "
             "start still run)",
             *noRestart_ ? "yes" : "NO", same, explorersBefore_->size()));
  }

  void CleanUp() {
    if (ownFiles_.empty() && ownDirs_.empty()) return;
    if (options_.keep) {
      Say("files: kept (--keep)");
      std::printf("The experiment's files are in %s\n", Utf8(root_.native()).c_str());
      if (desktopFile_) {
        std::printf("and on the Desktop: %s\n", Utf8(desktopFile_->native()).c_str());
      }
      return;
    }
    size_t deleted = 0, gone = 0, left = 0;
    for (auto own = ownFiles_.rbegin(); own != ownFiles_.rend(); ++own) {
      wil::unique_hfile file;
      HRESULT hr = OpenOwnFile(*own, DELETE, std::addressof(file));
      if (hr == S_OK) hr = DeleteThroughHandle(file.get());
      if (hr == S_OK) {
        ++deleted;
      } else if (hr == S_FALSE) {
        ++gone;
      } else {
        ++left;
      }
    }
    // File Explorer's copy of the copy-check file, in the experiment's own folder.
    if (copyFolderMade_) {
      pdft::ContentDelete result = pdft::ContentDelete::kMissing;
      (void)pdft::DeleteRegularFile(root_ / kCopyFolderName / kCopyCheckName, &result);
    }
    size_t foldersLeft = 0;
    for (auto folder = ownDirs_.rbegin(); folder != ownDirs_.rend(); ++folder) {
      if (!RemoveDirectoryW(folder->c_str())) ++foldersLeft;
    }
    Say(
        Text("files: %zu deleted, %zu gone or replaced (left alone), %zu could not be deleted; "
             "%zu folders left",
             deleted, gone, left, foldersLeft));
    if (left || foldersLeft) {
      std::printf("Delete what is left of %s by hand.\n", Utf8(root_.native()).c_str());
    }
  }

  // What the outcome decides, in the order of the experiment's table, and the next mechanism.
  void Decide() {
    Say("");
    Say("Decision");
    if (!valid_) {
      Say("The run is not valid (" + stop_ + "): nothing is decided.");
      return;
    }
    if (!variantsJudged_) {
      Say("The run stopped before the extraction variants were judged: nothing is decided.");
      return;
    }
    if (!stop_.empty()) {
      Say("The run stopped early (" + stop_ + "); what follows rests on the steps that ran.");
    }
    if (!restsOn_.empty()) Say(restsOn_);
    if (!best_ && !notDecided_.empty()) {
      Say("Pass A: not decided (no variant passed, and the cache dropped too many thumbnails of " +
          notDecidedLabels_ + " to judge them): run the experiment again.");
    } else if (!best_) {
      if (gone_) {
        Say(std::string("No Pass A, the thumbnails went without new ones being stored: ") +
            (storeVariantJudged_ ? "the variant that stores after the extraction normally passes "
                                   "then; if it does not, purge"
                                 : "no variant that stores after the extraction was run; purge") +
            " through the Thumbnail Cache clean-up handler (IEmptyVolumeCache2 "
            "{889900c3-59f3-4c2f-ae21-a409ea01e605}).");
      } else {
        Say("No Pass A, the old thumbnails stayed: the default stays off. Next: purge through the "
            "Thumbnail Cache clean-up handler (IEmptyVolumeCache2 "
            "{889900c3-59f3-4c2f-ae21-a409ea01e605}), on CI first and on this PC only with a "
            "go-ahead (it purges every file type's thumbnails); then a notification or a view "
            "refresh for the open views.");
      }
    } else if (!options_.interactive) {
      Say(Text("Pass A (extraction): yes, best %s%s. Pass B needs the sitting.",
               Utf8(best_->label).c_str(),
               notDecided_.empty() ? "" : ("; not decided before it: " + notDecided_).c_str()));
    } else {
      const bool passA = product_ == pdft::test::FolderOutcome::kPass;
      const std::string product = product_          ? productText_
                                  : productLeftOut_ ? "left out (" + *productLeftOut_ + ")"
                                                    : std::string("not checked");
      // A step that acts on the windows themselves counts only with the user's yes to its side
      // effects.
      const bool onWindows = mechanism_ == Mechanism::kViews || mechanism_ == Mechanism::kBrowse;
      const bool accepted = sideEffectsAccepted_.value_or(false);
      const bool passB = mechanism_ && (!onWindows || accepted) &&
                         (!desktopChecked_ || desktopSquare_) && dialogSquare_ &&
                         noRestart_.value_or(false);
      Say(Text("Pass A: %s (best %s%s; through the command-line executable: %s)",
               passA ? "yes" : "no", Utf8(best_->label).c_str(),
               notDecided_.empty() ? "" : ("; not decided before it: " + notDecided_).c_str(),
               product.c_str()));
      if (productDropped_) {
        Say("Pass A is not complete: the cache dropped too many of the control folder's "
            "thumbnails around the command-line executable's refresh to judge it; run the "
            "experiment again.");
      }
      Say(Text(
          "Pass B: %s (%s%s; Desktop %s; another program's dialog %s; File Explorer not "
          "restarted: %s)",
          passB ? "yes" : "no", mechanism_ ? MechanismText(*mechanism_) : "no mechanism",
          onWindows ? (accepted ? ", side effects accepted" : ", side effects NOT accepted") : "",
          desktopChecked_ ? (desktopSquare_ ? "square" : "old") : "not checked",
          dialogChecked_ ? (dialogSquare_ ? "square" : "old") : "not checked",
          noRestart_ ? (*noRestart_ ? "yes" : "no") : "unknown"));
      if (!mechanism_) {
        if (!dialogFresh_) {
          Say("No window turned square, and another program's dialog was not shown (no notified "
              "round was valid, or the dialog did not start): Pass B is not decided. Next: the "
              "rounds again, in a second sitting.");
        } else if (*dialogFresh_) {
          Say("Only another program's dialog shows the new thumbnails: the default stays off for "
              "now (Save asks \"Rebuild now / Later\"; --refresh stays best effort with its "
              "caveat). Next: associations changed after the update if not reached, then "
              "restarting File Explorer without deleting the cache, in a second sitting with a "
              "go-ahead.");
        } else {
          Say("Nothing showed the new thumbnails, not even another program's dialog: the default "
              "stays off. Next: notifications with SHCNF_IDLIST instead of paths, per file and per "
              "folder together; then restarting File Explorer without deleting the cache.");
        }
      } else if (!passA) {
        Say(product_ == pdft::test::FolderOutcome::kFail
                ? "A window turned square, but the command-line executable's refresh did not "
                  "pass: fix that first."
            : product_ ? "A window turned square, but the command-line executable's refresh was "
                         "not decided: Pass A is not complete."
                       : "A window turned square, but the command-line executable's refresh was "
                         "not checked: Pass A is not complete.");
      } else {
        switch (*mechanism_) {
          case Mechanism::kNone:
            Say("Default on: Pass A, and Pass B with no notification; still notify per file (for "
                "other programs).");
            break;
          case Mechanism::kItem:
          case Mechanism::kDir:
            Say(Text("Default on: notify %s.", MechanismText(*mechanism_)));
            break;
          case Mechanism::kViews:
            Say(accepted ? Text("Default on, its side effects accepted (selection and scroll %s): "
                                "refresh the views and notify per file.",
                                viewsKept_.value_or(false) ? "kept" : "not kept")
                         : std::string("Refreshing the views turned the window square, but its "
                                       "side effects were not accepted: the default stays off. "
                                       "Next: browsing the views again, then associations "
                                       "changed after the update, in a second sitting."));
            break;
          case Mechanism::kBrowse:
            Say(accepted ? "Default on, its side effects accepted: browse the views again (in "
                           "shell_windows) and notify per file."
                         : "Browsing the views again turned the window square, but its side "
                           "effects were not accepted: the default stays off. Next: associations "
                           "changed after the update, in a second sitting.");
            break;
          case Mechanism::kAssoc:
            Say("Default on with the update, then SHCNE_ASSOCCHANGED (every open window "
                "repaints).");
            break;
        }
        if (!passB && (!onWindows || accepted)) {
          Say("Pass B is not complete: see the Desktop and dialog lines above.");
        }
      }
    }
    if (terminalCacheFiles_.value_or(0) > 0) {
      Say("A thumbcache file is in Windows Terminal's private folders: the PDF-only refresh must "
          "refuse below an app package (exit 8, \"run from a Win+R prompt\"), like the rebuild's "
          "start rule.");
    }
    if (!refusedSizes_.empty()) {
      std::string accepted, refused;
      for (const UINT size : sizes_) accepted += Text(" %u", size);
      for (const UINT size : refusedSizes_) refused += Text(" %u", size);
      Say("Sizes refused:" + refused + "; the refresh sizes become the accepted ones:" + accepted +
          ". Extra-large icons at high DPI keep old thumbnails until Windows makes them again.");
    }
    if (contention_ > 2.0) {
      Say(
          Text("Contention is high (%.1f times idle): use INSTANCESURROGATE in the product if "
               "S-instance ran and passed Pass A, otherwise a 20 ms pause between extractions.",
               contention_));
    }
  }

  int Finish() {
    Say("");
    Phase("putting back");
    Say("Putting things back");
    const bool zoomBack = PutZoomBack();
    if (cache_) {
      // What the run added to the user's thumbnail cache, which keeps it after the files go.
      std::map<std::string, CacheFile> files;
      std::map<std::string, DWORD> unreadable;
      std::string grown = "not known";
      if (cacheBytesAtStart_ && SUCCEEDED(ListCacheFiles(&files, &unreadable))) {
        const double mib =
            (static_cast<double>(TotalBytes(files)) - static_cast<double>(*cacheBytesAtStart_)) /
            (1024.0 * 1024.0);
        grown = Text("%.1f MiB", mib);
      }
      Say("cache files at the end: " + CacheFilesText() + "; grown since the start by " + grown);
    }
    if (zoomSet_) {
      // The command names the user's own path, so it goes to the console only.
      std::printf(
          "PDFs shown during the test may keep the %d or %d %% crop until refreshed: \"%s\" "
          "--refresh --folder \"<folder>\"\n",
          kBaselineZoom, kFlippedZoom, Utf8(options_.cli.native()).c_str());
      Note(
          Text("PDFs shown during the test may keep the %d or %d %% crop until refreshed: "
               "PdfThumbnailsCli --refresh --folder <folder>",
               kBaselineZoom, kFlippedZoom));
    }
    CheckNoRestart();
    if (options_.interactive && windowUsed_ && !stoppedByUser_) {
      (void)WaitForEnter(
          "Close the File Explorer window that shows the test folders (and any Open dialog), then "
          "press Enter.");
    }
    (void)CheckUntouched("at the end");
    CleanUp();
    Decide();
    Note(Text("ran for %llu s", (PreciseMs() - started_) / 1000));
    if (!stop_.empty()) Note("stopped: " + stop_);
    std::string text;
    for (const std::string& line : lines_) text += line + "\n";
    const std::filesystem::path report = reportDir_ / L"report.txt";
    if (WriteText(report.native(), text)) {
      std::printf("\nThe report: %s\n", Utf8(report.native()).c_str());
    } else {
      std::printf("\ncannot write the report %s\n", Utf8(report.native()).c_str());
    }
    // A zoom not put back matters most, even after a q.
    if (!zoomBack) return 1;
    if (stoppedByUser_) return 3;
    const bool passA =
        valid_ && best_ && (!options_.interactive || product_ == pdft::test::FolderOutcome::kPass);
    return stop_.empty() && passA ? 0 : 1;
  }

  // ---- State ----

  const Options options_;
  ULONGLONG started_ = 0;
  std::wstring stamp_;
  std::wstring build_;
  int savedZoom_ = 100;
  bool otherShellUser_ = false;
  bool sacOn_ = false;
  bool cliBlocked_ = false;  // Application Control blocked the command-line executable
  bool thumbnailsOff_ = false;
  std::optional<std::vector<ExplorerProcess>> explorersBefore_;
  std::filesystem::path reportDir_;
  std::vector<std::string> lines_;

  std::filesystem::path root_;
  std::vector<std::wstring> judged_;
  // The labels in the order the fill takes them (kDesktopLabel for the Desktop file).
  std::vector<std::wstring> fillOrder_;
  std::map<std::wstring, std::vector<std::filesystem::path>> files_;
  std::vector<OwnFile> ownFiles_;
  std::vector<std::filesystem::path> ownDirs_;
  std::filesystem::path probeFile_;
  std::optional<std::filesystem::path> desktopFile_;
  bool copyFolderMade_ = false;

  std::unique_ptr<pdft::ThumbnailCacheOps> cache_;
  // The sizes judged and refreshed: those of kRefreshSizes the fill made thumbnails at, less those
  // at which the cache refuses the question.
  std::vector<UINT> sizes_;
  std::vector<UINT> refusedSizes_;
  std::vector<CacheRecord> baseline_;
  std::vector<CacheRecord> slowBefore_;
  std::set<std::wstring> excluded_;
  // Asking again found an entry at the baseline, so later questions ask again too (AskHeld).
  bool askAgain_ = false;
  // An untouched file: watched when it was held right after its store; when the cache was first
  // seen without it.
  struct Untouched {
    std::wstring key;
    bool watched = false;
    std::string lost;
  };
  std::vector<Untouched> untouched_;
  std::optional<ULONGLONG> cacheBytesAtStart_;  // the cache files' bytes before the fill
  std::vector<CacheRecord> controlLast_;        // the control's last answers since the variants
  std::vector<std::wstring> timingHeld_;        // the timing files held after the timing fill
  bool zoomSet_ = false;                        // the zoom was set at least once

  std::string stop_;
  bool stoppedByUser_ = false;
  bool variantsJudged_ = false;
  bool valid_ = true;
  // The variants run (AllowedVariants): only they get a folder and are judged.
  const std::vector<const Variant*> variants_ = AllowedVariants();
  bool storeVariantJudged_ = false;  // a variant that stores after the extraction was refreshed
  std::optional<Variant> best_;
  // The variants not decided before the best, or at all without one: with the files the cache
  // dropped, and their labels only.
  std::string notDecided_;
  std::string notDecidedLabels_;
  std::string restsOn_;  // what Pass A rests on, and what the cache dropped
  bool gone_ = false;

  std::map<std::wstring, Round> rounds_;
  std::optional<Mechanism> ladderStep_;
  std::optional<Mechanism> mechanism_;
  std::optional<bool> viewsKept_;
  std::optional<bool> sideEffectsAccepted_;  // of the ladder step that turned the window square
  std::optional<bool> dialogFresh_;
  std::optional<bool> f5Works_;
  bool windowUsed_ = false;
  bool desktopChecked_ = false;
  bool desktopSquare_ = false;
  bool dialogChecked_ = false;
  bool dialogSquare_ = false;

  double contention_ = 0;
  // The product check: its outcome and the words for it, why it was left out, and whether the
  // cache dropped too much to decide it.
  std::optional<pdft::test::FolderOutcome> product_;
  std::string productText_;
  std::optional<std::string> productLeftOut_;
  bool productDropped_ = false;
  std::optional<size_t> terminalCacheFiles_;
  std::optional<bool> operationStatus_;
  std::optional<bool> noRestart_;
};

int ExperimentUsage() {
  std::printf("usage:\n");
  PrintExperimentUsage();
  return 2;
}

}  // namespace

int RunExperiment(const std::vector<std::wstring>& args) {
  Options options;
  std::optional<std::wstring> cli;
  for (size_t i = 0; i < args.size(); ++i) {
    const std::wstring& name = args[i];
    const bool value = i + 1 < args.size();
    if (name == L"--cli" && value) {
      cli = args[++i];
    } else if (name == L"--root" && value) {
      options.root = args[++i];
    } else if (name == L"--non-interactive") {
      options.interactive = false;
    } else if (name == L"--keep") {
      options.keep = true;
    } else if (name == L"--timing-files" && value) {
      if (!ParseNumber(args[++i], 0, kMaxTimingFiles, &options.timingFiles)) {
        return ExperimentUsage();
      }
    } else if (name == L"--instance-files" && value) {
      if (!ParseNumber(args[++i], 0, kMaxTimingFiles, &options.instanceFiles)) {
        return ExperimentUsage();
      }
    } else if (name == L"--desktop") {
      options.desktop = true;
    } else if (name == L"--copy-check") {
      options.copyCheck = true;
    } else {
      return ExperimentUsage();
    }
  }
  if (!options.interactive && (options.desktop || options.copyCheck)) {
    std::printf("--desktop and --copy-check need answers, so not with --non-interactive\n");
    return 2;
  }
  HRESULT hr = pdft::GetOwnExe(&options.self);
  if (FAILED(hr)) {
    std::printf("cannot find thumbtest's own path: 0x%08lX\n", static_cast<unsigned long>(hr));
    return 2;
  }
  // The command-line executable: by default the one next to thumbtest.
  options.cli = cli ? std::filesystem::path(*cli) : options.self.parent_path() / pdft::kCliExe;
  std::error_code error;
  options.cli = std::filesystem::absolute(options.cli, error);
  const DWORD attributes =
      error ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(options.cli.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
    std::printf("no command-line executable at %s: pass --cli <PdfThumbnailsCli.exe>\n",
                Utf8(options.cli.native()).c_str());
    return 2;
  }
  if (options.root) {
    options.root = std::filesystem::absolute(*options.root, error);
    if (error || GetFileAttributesW(options.root->c_str()) != INVALID_FILE_ATTRIBUTES) {
      std::printf("--root must name a folder that does not exist yet\n");
      return 2;
    }
  }
  try {
    Experiment experiment(std::move(options));
    return experiment.Run();
  } catch (...) {
    // Out of memory, say: the zoom goes back all the same.
    std::lock_guard<std::mutex> held(g_zoomGuard.lock);
    if (g_zoomGuard.changed && FAILED(PutZoomBackHere())) {
      std::printf("The zoom could not be put back: run the command printed at the start.\n");
    }
    std::printf("the experiment failed\n");
    return 1;
  }
}

void PrintExperimentUsage() {
  std::printf(
      "  thumbtest experiment [--cli <PdfThumbnailsCli.exe>] [--root <dir>] [--non-interactive]\n"
      "                       [--keep] [--timing-files <n>] [--instance-files <n>] [--desktop]\n"
      "                       [--copy-check]\n"
      "      The one-sitting refresh experiment (docs/TESTING.md): it sets the zoom to 100 and\n"
      "      142 and puts it back, and fills and refreshes the real thumbnail cache with\n"
      "      synthetic PDFs. --instance-files: how many timing files are made again each in a\n"
      "      thumbnail process of its own (200), where the refresh allows that extraction.\n"
      "      Extraction variants the refresh refuses are left out.\n");
}

}  // namespace thumbtest
