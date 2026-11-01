// The elevation boundary: how an unelevated command has an administrator step done, and what the
// elevated process checks before it does anything.
//
// The only way in is the settings executable started through UAC ("runas") with exactly
//   --elevated <step>
// and nothing else on its command line. The steps are a closed set (repair.h ElevatedVerb;
// machine-install only in developer builds). The elevated process trusts nothing it is given:
// - it must really have administrator rights: an elevated token (TokenElevation) with the
//   Administrators group enabled and an integrity level of at least High (a filtered copy of an
//   administrator's token can count as elevated without being able to write for all users);
// - it must not run inside an app package or below a packaged process (whose file and registry
//   views could be private copies);
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

// What a token is, as far as writing for all users goes: the first line of an administrator
// step's report says it, because an elevated token alone does not prove that the step can write
// to Program Files and HKLM (that takes the Administrators group, enabled).
struct TokenFacts {
  bool elevated = false;  // TokenElevation
  TOKEN_ELEVATION_TYPE elevationType = TokenElevationTypeDefault;
  DWORD integrityRid = 0;  // SECURITY_MANDATORY_*_RID
  enum class Group {
    kEnabled,
    kDenyOnly,  // as in a filtered token: it can deny access, never grant it
    kDisabled,
    kAbsent,
  };
  Group administrators = Group::kAbsent;  // the Administrators group among the token's groups
};

// Reads the facts of |token| (opened with TOKEN_QUERY, or a pseudo handle such as
// GetCurrentProcessToken()).
[[nodiscard]] HRESULT ReadTokenFacts(HANDLE token, TokenFacts* facts) noexcept;

// "token: elevated yes, elevation type 2 (full), integrity 0x3000, Administrators enabled"
// (pure). May throw std::bad_alloc.
std::wstring TokenFactsText(const TokenFacts& facts);

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
  bool tokenElevated = false;  // TokenElevation
  // The Administrators group is enabled in the token (CheckTokenMembership): present but
  // deny-only or disabled does not count. Program Files and HKLM let only it write.
  bool administratorsEnabled = false;
  DWORD integrityRid = 0;            // the token's mandatory level, SECURITY_MANDATORY_*_RID
  bool packaged = false;             // this process or a parent runs in an app package
  std::optional<ElevatedVerb> verb;  // ParseElevatedArguments' answer
  bool devInstall = false;           // kDevInstall
  // This executable is <machine root>\<settings exe> (both canonicalized).
  bool runsFromMachineRoot = false;
  HRESULT reparsePoints = S_OK;  // CheckNoReparsePoints on the machine root
};

// Reads the token part of |facts| (tokenElevated, administratorsEnabled, integrityRid) from
// |token|: nullptr for this thread's current token (what the elevated side reads), else a token
// opened with TOKEN_QUERY | TOKEN_DUPLICATE. The other members are left as they are.
[[nodiscard]] HRESULT ReadEntryTokenFacts(HANDLE token, ElevatedEntryFacts* facts) noexcept;

// Why the elevated side refuses a step, in the order it checks.
enum class ElevatedRefusal {
  kNone,
  kNotElevated,               // TokenElevation says no
  kAdministratorsNotEnabled,  // the Administrators group is absent, deny-only or disabled
  kIntegrityBelowHigh,        // a mandatory integrity level below High
  kPackaged,                  // in or below an app package
  kNotOneStep,                // not exactly one known step
  kNotFromMachineRoot,        // not the settings executable of the install for all users
  kReparsePoint,              // a link or junction on the way to the install folder
};

// What the elevated side refuses with when its token cannot write for all users (not elevated,
// the Administrators group not enabled, or an integrity level below High): UAC did not give the
// step administrator rights. The commands that asked for the step say so and exit 7
// (exit_code.h): neither a declined prompt (3) nor a build folder others could change (8). A
// code of this app's own (FACILITY_ITF), not a Windows error, so that neither a failed launch nor
// a call inside a step's work can return it.
inline constexpr HRESULT kNoAdministratorRightsHr =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x2E4);

// The refusal for |facts| (pure), checked in this order:
//   not elevated                       kNoAdministratorRightsHr
//   Administrators not enabled         kNoAdministratorRightsHr
//   integrity below High               kNoAdministratorRightsHr
//   in or below an app package         HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED_IN_APPCONTAINER)
//   not exactly one known step         E_INVALIDARG
//   not run from the machine root      E_ACCESSDENIED (machine-install in developer builds is
//                                      exempt)
//   a reparse point on the way         facts.reparsePoints
// FindElevatedRefusal names it (kNone when it may go ahead); CheckElevatedEntry gives its
// HRESULT (S_OK when it may go ahead).
ElevatedRefusal FindElevatedRefusal(const ElevatedEntryFacts& facts) noexcept;
HRESULT CheckElevatedEntry(const ElevatedEntryFacts& facts) noexcept;

// What the report's refusal line says about |refusal| (pure; never a path): "no administrator
// rights (not elevated)", "no administrator rights (the Administrators group is not enabled)",
// ..., "inside or below an app package", ...; empty for kNone.
std::wstring_view ElevatedRefusalText(ElevatedRefusal refusal) noexcept;

// The elevated process (--elevated <step>): reads its own command line and context, checks them
// (CheckElevatedEntry), reports through the parent's pipe if there is one, and runs the step.
// Returns the process exit code: 0, 1 (done, restart pending) or the failing HRESULT. The
// report's first line ("elevated <step> start", or "... refused hr=<HRESULT>: <why>" with
// ElevatedRefusalText) ends with its token's facts (TokenFactsText); a step that fails says
// where its failure started before its last line ("elevated <step> failed in <part> at
// <file>:<line> <function> hr=<HRESULT>", failure_trace.h, with " (end hr differs)" when the
// step returned another HRESULT).
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
  kFailed,          // LackedAdministratorRights tells a step without administrator rights
};

// How an elevated run ended (pure): the launcher's result first (cancelled, or failed to start),
// then the exit code: 0, 1, a failure HRESULT; any other code counts as failed with E_UNEXPECTED.
// |failure| receives the HRESULT of a failure, else S_OK.
ElevatedOutcome ClassifyElevatedExit(HRESULT launched, DWORD exitCode, HRESULT* failure) noexcept;

struct ElevatedRun {
  ElevatedOutcome outcome = ElevatedOutcome::kFailed;
  HRESULT failure = S_OK;   // with kFailed
  HRESULT launched = S_OK;  // what the launcher returned
  DWORD exitCode = 0;
  bool reportChannel = false;        // the elevated process wrote to this process's pipe
  std::vector<std::wstring> report;  // what it wrote, as untrusted text
};

// Whether |run| failed because the step did not get administrator rights: it was launched and
// the elevated process itself exited with kNoAdministratorRightsHr, its entry check's refusal
// (pure).
bool LackedAdministratorRights(const ElevatedRun& run) noexcept;

// Has |verb| done by the settings executable |exe| (which must be named like it and be
// absolute: E_INVALIDARG otherwise, before anything starts) through |launcher|, collecting its
// report. Fails only for invalid arguments or when out of memory; everything else is in |run|.
[[nodiscard]] HRESULT RunElevated(ElevatedVerb verb, const std::filesystem::path& exe, HWND owner,
                                  const ElevationLauncher& launcher, ElevatedRun* run) noexcept;

}  // namespace pdft
