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
// - its command line must be exactly the step;
// - it must itself be <FOLDERID_ProgramFiles>\<Product>\<settings exe>, where only administrators
//   can put it (developer builds exempt machine-install, which copies a build from the folder it
//   runs in; that folder's permissions are checked instead, file_ops.h);
// - no reparse point may lie on the way to the install folder;
// - below an app package (it or a parent runs in one, or the walk up its parents fails) it goes
//   on only when test values it writes under HKEY_LOCAL_MACHINE\Software\Classes and in its own
//   user's registry come back through WMI (context.h), since a package alone says nothing about
//   where writes land; this check runs last, so only a step that passed everything else writes
//   its two volatile test keys;
// - whatever it writes machine-wide it computes itself from the files under the install folder.
// It never writes or deletes in the user's profile (%LOCALAPPDATA%, %TEMP%, the thumbnail cache)
// except that test key in its own registry, which it deletes again through the handle it made it
// with, never by its name; it never loads the thumbnail handler, shows no window and writes no
// log. Its exit code is its result: 0 done, 1 done but a restart is pending, else the HRESULT
// that failed.
//
// A step that goes on after a part of it failed (the class for all users written, then the task
// folder not made or an old build folder not removed) reports each such part as "not done:
// <part>: <HRESULT>", and its last line counts them: "elevated <step> end hr=<HRESULT>[
// restart-pending] not-done=<n>". Reporting the parts changes no exit code: machine-install still
// exits 0 or 1 once its class for all users is written, since a parent of any build reads 0 and
// 1 as done, and machine-uninstall and machine-migrate-v1 still exit with the HRESULT of their
// first failed part. The asking side adds each part to its command's failures (ReadReportParts,
// NotDoneFailures); the report can only add failures, never take away the one a failed exit code
// gives.
//
// Progress lines go back to the process that asked, through a named pipe that process creates
// before starting the elevated one (\\.\pipe\<Product>.Elevated.<its process id>, first
// instance only, owned by its user and open to the user and Administrators). The name is
// predictable, so the elevated process writes there only after making sure that the pipe's
// server is its own parent process (held open while it checks, so the id stays the parent's),
// in the parent's session, and that the pipe's owner is the parent's user (no other account can
// make it that). It connects anonymously, so the server cannot act with its rights, and gives
// each line a few seconds at most, so a server that does not read cannot hold up the step.
// Nothing it reads ever comes from the pipe. When someone else made a pipe of that name first,
// the step runs without a report, and the asking side records why (ReportChannelTaken).

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
#include "context.h"
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

// Where this executable runs from, as the elevated side checks it, and as the settings window
// checks it before File Explorer starts it again for another account.
struct MachineRootCheck {
  // This executable is <machine root>\<settings exe> (both canonicalized).
  bool runsFromMachineRoot = false;
  HRESULT reparsePoints = S_OK;  // CheckNoReparsePoints on the machine root
};

// Reads MachineRootCheck for this executable. Where the machine root cannot be found,
// reparsePoints is E_UNEXPECTED (and runsFromMachineRoot false).
MachineRootCheck CheckMachineRoot() noexcept;

// Whether |check| shows the settings executable of the install for all users (pure): it runs from
// the machine root, and no link or junction is on the way there. Only administrators can change
// what lies there, while a copy anywhere else may lie in a folder its account can change.
bool IsMachineInstall(const MachineRootCheck& check) noexcept;

struct ElevatedEntryFacts {
  bool tokenElevated = false;  // TokenElevation
  // The Administrators group is enabled in the token (CheckTokenMembership): present but
  // deny-only or disabled does not count. Program Files and HKLM let only it write.
  bool administratorsEnabled = false;
  DWORD integrityRid = 0;            // the token's mandatory level, SECURITY_MANDATORY_*_RID
  std::optional<ElevatedVerb> verb;  // ParseElevatedArguments' answer
  bool devInstall = false;           // kDevInstall
  // This executable is <machine root>\<settings exe> (both canonicalized).
  bool runsFromMachineRoot = false;
  HRESULT reparsePoints = S_OK;  // CheckNoReparsePoints on the machine root
  // The walk from GatherEntryFacts; the canaries only from DecideElevatedEntry.
  MachineWriteCheck machineWrites;
};

// Reads the token part of |facts| (tokenElevated, administratorsEnabled, integrityRid) from
// |token|: nullptr for this thread's current token (what the elevated side reads), else a token
// opened with TOKEN_QUERY | TOKEN_DUPLICATE. The other members are left as they are.
[[nodiscard]] HRESULT ReadEntryTokenFacts(HANDLE token, ElevatedEntryFacts* facts) noexcept;

// Whether the token part of |facts| has administrator rights as the elevated side judges its own
// token (pure): elevated, the Administrators group enabled and an integrity level of High or
// more, the first three checks of FindElevatedRefusal.
bool HasAdministratorRights(const ElevatedEntryFacts& facts) noexcept;

// HasAdministratorRights of this thread's current token (ReadEntryTokenFacts). The steps for all
// users that run in the installer's own process (--setup-machine, --uninstall-cleanup) check it
// before anything else, since an elevated token alone (GetTokenElevation) does not prove these
// rights.
[[nodiscard]] HRESULT ReadAdministratorRights(bool* rights) noexcept;

// Why the elevated side refuses a step, in the order it checks.
enum class ElevatedRefusal {
  kNone,
  kNotElevated,               // TokenElevation says no
  kAdministratorsNotEnabled,  // the Administrators group is absent, deny-only or disabled
  kIntegrityBelowHigh,        // a mandatory integrity level below High
  kNotOneStep,                // not exactly one known step
  kNotFromMachineRoot,        // not the settings executable of the install for all users
  kReparsePoint,              // a link or junction on the way to the install folder
  // below an app package (or with the processes above unreadable) where its test writes did not
  // come back
  kPackaged,
};

// What the elevated side refuses with when its token cannot write for all users (not elevated,
// the Administrators group not enabled, or an integrity level below High): UAC did not give the
// step administrator rights. The commands that asked for the step say so and exit 7
// (exit_code.h): neither a declined prompt (3) nor a build folder others could change (8). A
// code of this app's own (FACILITY_ITF), not a Windows error, so that neither a failed launch nor
// a call inside a step's work can return it.
inline constexpr HRESULT kNoAdministratorRightsHr =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_ITF, 0x2E4);

// The elevated side's refusal below an app package (or where it cannot read the processes above
// it) when its test writes did not come back: HRESULT_FROM_WIN32(
// ERROR_NOT_SUPPORTED_IN_APPCONTAINER), the code earlier builds refused every step below a package
// with, so that parents of either build tell it apart. The commands exit 8.
inline constexpr HRESULT kBelowPackageHr =
    MAKE_HRESULT(SEVERITY_ERROR, FACILITY_WIN32, ERROR_NOT_SUPPORTED_IN_APPCONTAINER);

// The refusal for |facts| (pure), checked in this order:
//   not elevated                       kNoAdministratorRightsHr
//   Administrators not enabled         kNoAdministratorRightsHr
//   integrity below High               kNoAdministratorRightsHr
//   not exactly one known step         E_INVALIDARG
//   not run from the machine root      E_ACCESSDENIED (machine-install in developer builds is
//                                      exempt)
//   a reparse point on the way         facts.reparsePoints
//   below an app package whose test    kBelowPackageHr (RefusesMachineWrites: below a package,
//   writes did not come back           unless both canaries ran and came back)
// FindElevatedRefusal names it (kNone when it may go ahead); CheckElevatedEntry gives its
// HRESULT (S_OK when it may go ahead).
ElevatedRefusal FindElevatedRefusal(const ElevatedEntryFacts& facts) noexcept;
HRESULT CheckElevatedEntry(const ElevatedEntryFacts& facts) noexcept;

// FindElevatedRefusal; where the only refusal left is kPackaged not yet probed, runs |probe| on
// facts->machineWrites once and decides again. Pure but for |probe| (ElevatedMain passes
// ProbeMachineWrites).
ElevatedRefusal DecideElevatedEntry(ElevatedEntryFacts* facts,
                                    const std::function<void(MachineWriteCheck*)>& probe) noexcept;

// What the report's refusal line says about |refusal| (pure; never a path): "no administrator
// rights (not elevated)", "no administrator rights (the Administrators group is not enabled)",
// ..., "inside or below an app package, or with the processes above it unreadable, and its test
// writes did not come back", ...; empty for kNone.
std::wstring_view ElevatedRefusalText(ElevatedRefusal refusal) noexcept;

// ElevatedRefusalText, with MachineWriteCheckText of |facts| in brackets for kPackaged (pure).
// May throw std::bad_alloc.
std::wstring ElevatedRefusalReason(ElevatedRefusal refusal, const ElevatedEntryFacts& facts);

// "elevated <step> writes checked: <MachineWriteCheckText>" (pure): the report line of a step
// that ran its canaries and went on. May throw std::bad_alloc.
std::wstring ElevatedWritesCheckedLine(std::wstring_view step, const MachineWriteCheck& check);

// The elevated process (--elevated <step>): reads its own command line and context, checks them
// (DecideElevatedEntry, CheckElevatedEntry), reports through the parent's pipe if there is one,
// and runs the step. Returns the process exit code: 0, 1 (done, restart pending) or the failing
// HRESULT. The report's first line ("elevated <step> start", or "... refused hr=<HRESULT>: <why>"
// with ElevatedRefusalReason) ends with its token's facts (TokenFactsText); a step that tested
// its writes and went on says so next (ElevatedWritesCheckedLine). After the step's own lines
// come one NotDoneLine per part it did not complete, then, for a step that fails, where its
// failure started ("elevated <step> failed in <part> at <file>:<line> <function> hr=<HRESULT>",
// failure_trace.h, with " (end hr differs)" when the step returned another HRESULT), and last
// ElevatedEndLine with their count (EndElevatedStep).
int ElevatedMain() noexcept;

// The step itself, after the entry checks passed (install.h, uninstall.h, migrate_v1.h).
// |restartPending| is set when something is left for the next restart; |notDone| receives the
// parts it went on after, "<part>: <HRESULT>" (never paths). A step not in this build fails with
// E_NOTIMPL.
class ElevatedReporter;
[[nodiscard]] HRESULT RunElevatedStep(ElevatedVerb verb, ElevatedReporter& report,
                                      bool* restartPending,
                                      std::vector<std::wstring>* notDone) noexcept;

// ---- The reporting channel ------------------------------------------------------------------

// Lines longer than this are cut; the parent keeps at most kMaxReportLines lines, from the first
// kMaxReportBytes bytes that arrive. It reads the rest too, and drops it: left in the pipe, it
// would make the elevated process wait for room.
inline constexpr size_t kMaxReportLineChars = 1024;
inline constexpr size_t kMaxReportLines = 512;
inline constexpr size_t kMaxReportBytes = 256 * 1024;

// How long the elevated process waits for the pipe to take one line. A parent that does not read
// must not hold up the step: past this, the step goes on without a report.
inline constexpr DWORD kReportWriteTimeoutMs = 5000;

// The pipe of the process |parentProcessId|: \\.\pipe\<Product>.Elevated.<id>. May throw
// std::bad_alloc.
std::wstring ElevationPipeName(DWORD parentProcessId);

// What the elevated side knows about the pipe named after its parent when it decides whether to
// write there.
struct ReportServerFacts {
  DWORD parentId = 0;       // its parent (OpenParentProcess, kept open while it decides)
  DWORD parentSession = 0;  // the parent's session
  DWORD serverId = 0;       // the process that made the pipe (GetNamedPipeServerProcessId)
  DWORD serverSession = 0;  // that process's session (GetNamedPipeServerSessionId)
  // The pipe's owner is the user of the parent's token.
  bool ownerIsParentsUser = false;
};

// Whether the elevated side writes to the pipe (pure): only when its parent made it, in the
// parent's session, and it is owned by the parent's user. The process id alone is not enough:
// another account's pipe could carry an id that has since passed to the parent, but no other
// account can make the parent's user its owner.
bool TrustReportServer(const ReportServerFacts& facts) noexcept;

// The elevated side's end: connects to its parent's pipe and writes lines to it.
class ElevatedReporter {
 public:
  ElevatedReporter() = default;
  ElevatedReporter(ElevatedReporter&&) noexcept = default;
  ElevatedReporter& operator=(ElevatedReporter&&) noexcept = default;

  // Connects to the pipe named after this process's parent (still the process that created this
  // one: not created after it, and kept open while the pipe is checked), but only when
  // TrustReportServer trusts it. Without a pipe, with someone else's, or when the pipe's or the
  // parent's facts cannot be read, the reporter stays unconnected and Line() does nothing.
  static ElevatedReporter Connect() noexcept;

  bool connected() const noexcept { return pipe_.is_valid(); }
  // Writes one line (UTF-8, control characters as spaces, at most kMaxReportLineChars). A write
  // that fails, or that the pipe does not take within kReportWriteTimeoutMs (it is cancelled),
  // disconnects the reporter for the rest of the step; the step's exit code still tells how it
  // ended.
  void Line(std::wstring_view text) noexcept;

 private:
  wil::unique_hfile pipe_;             // opened for overlapped I/O
  wil::unique_event_nothrow written_;  // set when a write to |pipe_| has ended
};

// The asking side's end: the pipe, and a thread that collects what arrives.
class ElevationReportServer {
 public:
  ElevationReportServer(const ElevationReportServer&) = delete;
  ElevationReportServer& operator=(const ElevationReportServer&) = delete;
  ~ElevationReportServer();

  // Creates this process's pipe (first instance only: a pipe of that name that someone else made
  // first fails with HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) or ERROR_PIPE_BUSY), owned by
  // |userSid| (this process's user, GetCurrentUserSid: the elevated side writes only to a pipe
  // its parent's user owns) with a DACL for |userSid| and Administrators, and starts collecting.
  [[nodiscard]] static HRESULT Create(std::wstring_view userSid,
                                      std::unique_ptr<ElevationReportServer>* server) noexcept;

  // Call once the elevated process has ended: stops collecting (waiting briefly for the rest of
  // its lines, also when a client keeps writing) and returns the lines of the first
  // kMaxReportBytes bytes (the rest was read and dropped), but only if the client was
  // |processId|; otherwise none. |fromClient| (optional) tells whether |processId| was the client.
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

// ---- Parts a step did not complete ----------------------------------------------------------

class FailureTrace;

// "not done: " + |failure|, where |failure| is "<part>: <HRESULT>" as the step's own list has it
// ("task-folder: 0x80070005", "hive left: 0x800402F0"). May throw std::bad_alloc.
std::wstring NotDoneLine(std::wstring_view failure);

// "elevated <step> end hr=<HRESULT>[ restart-pending] not-done=<n>" (pure). May throw
// std::bad_alloc.
std::wstring ElevatedEndLine(ElevatedVerb verb, HRESULT hr, bool restartPending, size_t notDone);

// ElevatedMain's last part, once the step returned |hr|: a NotDoneLine per |notDone| entry, where
// a failure started (|trace|'s pending failure) when |hr| failed, then ElevatedEndLine. Returns
// the exit code: |hr| when it failed, else 1 when |restartPending|, else 0.
int EndElevatedStep(ElevatedReporter& report, ElevatedVerb verb, HRESULT hr, bool restartPending,
                    std::span<const std::wstring> notDone, const FailureTrace& trace) noexcept;

// A part a report names as not done.
struct NotDonePart {
  std::wstring part;  // "task-folder", "hive left", ...
  HRESULT hr = S_OK;  // a failure
};

// What a step's report says about the parts it did not complete.
struct ReportParts {
  bool ended = false;              // the last line is this step's own end line
  std::optional<size_t> counted;   // its not-done count; absent in earlier builds
  std::vector<NotDonePart> named;  // the well-formed NotDoneLine lines, in order
};

// Reads |lines| (a report of the step |verb|, untrusted text) strictly (pure):
// - the end line is the last line: "elevated <ElevatedVerbName> end hr=", then "0x" and 8 hex
//   digits in either case, then optional words, each after a space. "not-done=<1 to 9 digits>"
//   sets |counted|, at most kMaxReportLines; other words are skipped, so a later build may add
//   some. Any other last line leaves |ended| false and |counted| absent;
// - the named parts come from every line before the end line, or from every line when there is
//   none. Such a line is "not done: " + part + ": " + hr, split at the last ": ". The part is 1 to
//   64 characters of a-z, 0-9, "-" and space, with no space first or last; the hr is "0x" and 8
//   hex digits, and a failure. Any other line names nothing.
// May throw std::bad_alloc.
ReportParts ReadReportParts(ElevatedVerb verb, std::span<const std::wstring> lines);

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
  ReportParts parts;                 // ReadReportParts of |report|
  // Making this process's pipe, before the launch. A failure leaves the step without a report
  // (it runs all the same); ReportChannelTaken tells when another process caused it.
  HRESULT reportServer = S_OK;
};

// Whether |run| failed because the step did not get administrator rights: it was launched and
// the elevated process itself exited with kNoAdministratorRightsHr, its entry check's refusal
// (pure).
bool LackedAdministratorRights(const ElevatedRun& run) noexcept;

// Whether |run| failed because the step refused to run below an app package: it was launched and
// the elevated process itself exited with kBelowPackageHr (pure). A step of an earlier build
// refuses so below any package, one of this build where its test writes did not come back.
bool RefusedBelowPackage(const ElevatedRun& run) noexcept;

// Whether |run| had no report channel because another process had made a pipe of its name
// first: making it failed with HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) or ERROR_PIPE_BUSY
// (pure). The step ran all the same and its exit code still tells how it ended; only its own
// report could not come back, which is worth telling the user ("the report channel was taken by
// another process").
bool ReportChannelTaken(const ElevatedRun& run) noexcept;

// How many parts |run|'s report says the step did not complete (pure): its count, or the parts
// it names when they are more; 0 for a step that never ran (declined, not launched, no
// administrator rights, refused below an app package).
size_t CountPartsNotDone(const ElevatedRun& run) noexcept;
// CountPartsNotDone(|run|) > 0 (pure).
bool PartsNotDone(const ElevatedRun& run) noexcept;

// The failures of the command for the parts |run|'s step did not complete, whatever its exit code
// says (pure): "<step> <part>: <HRESULT>" per named part, then "<step> unnamed parts: <n> (see the
// step's report)" for parts counted but not named; none for a step that never ran (declined, not
// launched, no administrator rights, refused below an app package). May throw std::bad_alloc.
std::vector<std::wstring> NotDoneFailures(const ElevatedRun& run, std::wstring_view step);

// For the steps that exit with their first part's HRESULT (machine-uninstall,
// machine-migrate-v1): whether the command still needs the failure "<step>: <HRESULT>" (pure).
// True for a failed run (a failed launch included) unless it lacked administrator rights, refused
// below an app package (both have exit codes of their own) or its first named part has the
// HRESULT it exited with. Any other failed step always keeps one entry: this one or its parts.
bool NeedsStepFailure(const ElevatedRun& run) noexcept;

// Done (kDone, kRestartPending) without its end line as the report's last line (pure): no
// report, or one cut short.
bool ReportIncomplete(const ElevatedRun& run) noexcept;

// The line RunElevated logs after the report (pure): "<step> launch=<HRESULT> exit=<HRESULT>",
// then " report-channel=<HRESULT>" (with " (taken by another process)") when the pipe could not
// be made, " no-report" when nothing came through it, " report-incomplete" when the report of a
// step that is done lacks its end line, and " not-done=<n>" (CountPartsNotDone) when parts were
// not done. May throw std::bad_alloc.
std::wstring ElevatedRunSummary(ElevatedVerb verb, const ElevatedRun& run);

// Has |verb| done by the settings executable |exe| (which must be named like it and be
// absolute: E_INVALIDARG otherwise, before anything starts) through |launcher|, collecting its
// report and reading its parts not done (ReadReportParts). Fails only for invalid arguments or
// when out of memory; everything else is in |run|, a report channel it could not make included
// (reportServer; the summary line it logs, ElevatedRunSummary, says so too).
[[nodiscard]] HRESULT RunElevated(ElevatedVerb verb, const std::filesystem::path& exe, HWND owner,
                                  const ElevationLauncher& launcher, ElevatedRun* run) noexcept;

}  // namespace pdft
