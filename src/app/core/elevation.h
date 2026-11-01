// The elevation boundary: how an unelevated command has an administrator step done, and what the
// elevated process checks before it does anything.
//
// The only way in is the settings executable started through UAC ("runas") with exactly
//   --elevated <step>
// and nothing else on its command line. The steps are a closed set (repair.h ElevatedVerb;
// machine-install only in developer builds). The elevated process trusts nothing it is given:
// - it must really be elevated (TokenElevation), and must not run inside an app package or below
//   a packaged process (whose file and registry views could be private copies);
// - its command line must be exactly the step;
// - it must itself be <FOLDERID_ProgramFiles>\<Product>\<settings exe>, where only administrators
//   can put it (developer builds exempt machine-install, which copies a build from the folder it
//   runs in; that folder's permissions are checked instead, file_ops.h);
// - no reparse point may lie on the way to the install folder;
// - whatever it writes machine-wide it computes itself from the files under the install folder.
// It never writes or deletes in the user's profile (%LOCALAPPDATA%, %TEMP%, the thumbnail cache),
// never loads the thumbnail handler, shows no window and writes no log. Its exit code is its
// result: 0 done, 1 done but a restart is pending, else the HRESULT that failed.
//
// Progress lines go back to the process that asked, through a named pipe that process creates
// before starting the elevated one (\\.\pipe\<Product>.Elevated.<its process id>, first
// instance only, open to the user and Administrators). The elevated process writes there only
// after making sure that the pipe's server is its own parent process; it connects anonymously,
// so the server cannot act with its rights. Nothing it reads ever comes from the pipe.

#pragma once

#include <windows.h>

#include <wil/resource.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cli_args.h"
#include "repair.h"

namespace pdft {

// Whether this process's token is elevated (TokenElevation): a full administrator token, or
// SYSTEM. Not the same as ProcessContext::elevated, which also counts a raised integrity level.
[[nodiscard]] HRESULT GetTokenElevation(bool* elevated) noexcept;

// ---- Steps and their command line (pure) ----------------------------------------------------

// "machine-install", "machine-repair-class", "machine-uninstall", "machine-migrate-v1".
std::wstring_view ElevatedVerbName(ElevatedVerb verb) noexcept;

// The step named |name| (exact, case-sensitive), if it exists in this build: machine-install
// only with |devInstall|.
std::optional<ElevatedVerb> ElevatedVerbFromName(std::wstring_view name, bool devInstall) noexcept;

// The command line parser's step as an ElevatedVerb.
ElevatedVerb ElevatedVerbFromStep(ElevatedStep step) noexcept;

// What the elevated process is started with: "--elevated <name>". May throw std::bad_alloc.
std::wstring ElevatedParameters(ElevatedVerb verb);

// The elevated process's own arguments after the program name, held to the one accepted form:
// exactly "--elevated" and a step name, as two words. Anything else (another word, the
// "--elevated=<step>" spelling, another case) gives std::nullopt.
std::optional<ElevatedVerb> ParseElevatedArguments(std::span<const std::wstring_view> args,
                                                   bool devInstall) noexcept;

// ---- What the elevated side checks first ----------------------------------------------------

struct ElevatedEntryFacts {
  bool tokenElevated = false;
  bool packaged = false;             // this process or a parent runs in an app package
  std::optional<ElevatedVerb> verb;  // ParseElevatedArguments' answer
  bool devInstall = false;           // kDevInstall
  // This executable is <machine root>\<settings exe> (both canonicalized).
  bool runsFromMachineRoot = false;
  HRESULT reparsePoints = S_OK;  // CheckNoReparsePoints on the machine root
};

// The refusal for |facts| (pure), checked in this order:
//   not elevated                       HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED)
//   in or below an app package         HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED_IN_APPCONTAINER)
//   not exactly one known step         E_INVALIDARG
//   not run from the machine root      E_ACCESSDENIED (machine-install in developer builds is
//                                      exempt)
//   a reparse point on the way         facts.reparsePoints
// S_OK when it may go ahead.
HRESULT CheckElevatedEntry(const ElevatedEntryFacts& facts) noexcept;

// The elevated process (--elevated <step>): reads its own command line and context, checks them
// (CheckElevatedEntry), reports through the parent's pipe if there is one, and runs the step.
// Returns the process exit code: 0, 1 (done, restart pending) or the failing HRESULT.
int ElevatedMain() noexcept;

// The step itself, after the entry checks passed (install.h, uninstall.h, migrate_v1.h).
// |restartPending| is set when something is left for the next restart. A step not in this build
// fails with E_NOTIMPL.
class ElevatedReporter;
[[nodiscard]] HRESULT RunElevatedStep(ElevatedVerb verb, ElevatedReporter& report,
                                      bool* restartPending) noexcept;

// ---- The reporting channel ------------------------------------------------------------------

// Lines longer than this are cut; the parent keeps at most kMaxReportLines lines.
inline constexpr size_t kMaxReportLineChars = 1024;
inline constexpr size_t kMaxReportLines = 512;

// The pipe of the process |parentProcessId|: \\.\pipe\<Product>.Elevated.<id>. May throw
// std::bad_alloc.
std::wstring ElevationPipeName(DWORD parentProcessId);

// The elevated side's end: connects to its parent's pipe and writes lines to it.
class ElevatedReporter {
 public:
  ElevatedReporter() = default;
  ElevatedReporter(ElevatedReporter&&) noexcept = default;
  ElevatedReporter& operator=(ElevatedReporter&&) noexcept = default;

  // Connects to the pipe named after this process's parent, but only when the parent (still the
  // process that created this one: not created after it) is the pipe's server. Without a pipe,
  // or with someone else's, the reporter stays unconnected and Line() does nothing.
  static ElevatedReporter Connect() noexcept;

  bool connected() const noexcept { return pipe_.is_valid(); }
  // Writes one line (UTF-8, control characters as spaces, at most kMaxReportLineChars). A write
  // that fails disconnects the reporter.
  void Line(std::wstring_view text) noexcept;

 private:
  wil::unique_hfile pipe_;
};

// The asking side's end: the pipe, and a thread that collects what arrives.
class ElevationReportServer {
 public:
  ElevationReportServer(const ElevationReportServer&) = delete;
  ElevationReportServer& operator=(const ElevationReportServer&) = delete;
  ~ElevationReportServer();

  // Creates this process's pipe (first instance only: a pipe of that name that someone else made
  // first fails with HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) or ERROR_PIPE_BUSY) with a DACL for
  // |userSid| and Administrators, and starts collecting.
  [[nodiscard]] static HRESULT Create(std::wstring_view userSid,
                                      std::unique_ptr<ElevationReportServer>* server) noexcept;

  // Call once the elevated process has ended: stops collecting (waiting briefly for the rest of
  // its lines) and returns the lines, but only if the client was |processId|; otherwise none.
  // |fromClient| (optional) tells whether |processId| was the client.
  std::vector<std::wstring> Finish(DWORD processId, bool* fromClient = nullptr) noexcept;

  // What the collecting thread shares with this object (elevation.cpp only).
  struct State;

 private:
  explicit ElevationReportServer(std::shared_ptr<State> state) noexcept;
  std::shared_ptr<State> state_;
  wil::unique_handle thread_;
};

// The pipe bytes as lines (pure): split at "\n" (a "\r" before it dropped), UTF-8 decoded with
// U+FFFD for malformed bytes, control characters as spaces, each cut at kMaxReportLineChars, at
// most kMaxReportLines lines; a last line without "\n" counts. May throw std::bad_alloc.
std::vector<std::wstring> SplitReportLines(std::string_view bytes);

// ---- Starting the elevated process ----------------------------------------------------------

struct ElevationRequest {
  std::filesystem::path exe;  // the settings executable
  std::wstring parameters;    // "--elevated <step>"
  HWND owner = nullptr;       // the window that asks; nullptr for the command line
};

// Starts |request| elevated, waits for it to end and returns its process id and exit code.
// HRESULT_FROM_WIN32(ERROR_CANCELLED) when the user declined the prompt. Injectable, so tests
// never start a real elevation.
using ElevationLauncher =
    std::function<HRESULT(const ElevationRequest& request, DWORD* processId, DWORD* exitCode)>;

// The real launcher: ShellExecuteExW "runas", hidden, no error UI; waits pumping messages when
// there is an owner window, plainly otherwise.
[[nodiscard]] HRESULT LaunchElevatedWithShell(const ElevationRequest& request, DWORD* processId,
                                              DWORD* exitCode) noexcept;

enum class ElevatedOutcome {
  kDone,
  kRestartPending,  // done; something waits for the next restart
  kCancelled,       // the user declined the prompt: exit code 3
  kFailed,
};

// How an elevated run ended (pure): the launcher's result first (cancelled, or failed to start),
// then the exit code: 0, 1, a failure HRESULT; any other code counts as failed with E_UNEXPECTED.
// |failure| receives the HRESULT of a failure, else S_OK.
ElevatedOutcome ClassifyElevatedExit(HRESULT launched, DWORD exitCode, HRESULT* failure) noexcept;

struct ElevatedRun {
  ElevatedOutcome outcome = ElevatedOutcome::kFailed;
  HRESULT failure = S_OK;  // with kFailed
  DWORD exitCode = 0;
  bool reportChannel = false;        // the elevated process wrote to this process's pipe
  std::vector<std::wstring> report;  // what it wrote, as untrusted text
};

// Has |verb| done by the settings executable |exe| (which must be named like it and be
// absolute: E_INVALIDARG otherwise, before anything starts) through |launcher|, collecting its
// report. Fails only for invalid arguments or when out of memory; everything else is in |run|.
[[nodiscard]] HRESULT RunElevated(ElevatedVerb verb, const std::filesystem::path& exe, HWND owner,
                                  const ElevationLauncher& launcher, ElevatedRun* run) noexcept;

}  // namespace pdft
