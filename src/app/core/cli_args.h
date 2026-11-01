// The command line of both executables (pure: no I/O; parsing and the help text come from the
// tables in cli_args.cpp, so they cannot drift apart).
//
//   <exe> <command> [options]
//
// Rules:
// - The command comes first; exactly one per call. No command: the settings executable opens
//   its window, the command-line executable starts the settings executable.
// - Options are lowercase and case-sensitive, and belong to the command: an option another
//   command takes is a usage error here.
// - Values: --name value, or --name=value. A separate value may not start with "--".
// - -h, -? and /? are spellings of --help.
// - Numbers are whole decimal numbers, all digits, in range.
// - Two internal options exist in the settings executable only and are not in the help:
//   --elevated <step> (the elevated half of a change; nothing else may follow) and --relaunched
//   (marks a start that went through File Explorer, so it is not relaunched again).

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "exit_code.h"
#include "mode.h"

namespace pdft {

enum class Verb {
  kNone,  // no command
  kStatus,
  kCheck,
  kFix,
  kZoom,
  kChecks,
  kRefresh,
  kInstall,
  kUninstall,
  kDiagnostics,
  kMigrateV1,
  kSetupUser,
  kSetupMachine,
  kUninstallCleanup,
  kVersion,
  kHelp,
  kElevated,
};

// Whether a command changes per-user state. Such a command must reach the real registry, so it
// runs the registry canary first and is refused where the registry is virtualized (context.h).
enum class Writes {
  kNo,
  kYes,
  kWithValue,  // only when the command's own value is given: "--zoom 130" writes, "--zoom" reads
  // The installer steps that run elevated and change machine state. The per-user canary
  // checks the wrong thing for them and must not stop them (their exit codes have no 8): each
  // makes its own per-user decision. For the uninstaller's clean-up that means running the
  // canary itself and skipping only its per-user part when the canary does not say "no",
  // still exiting 0.
  kMachine,
};

enum class ValueKind { kNone, kOptional, kRequired };

// One row of the command table.
struct VerbInfo {
  Verb verb;
  std::wstring_view name;       // "--status"; empty for kNone
  ValueKind value;              // the command's own value ("--zoom 130")
  std::wstring_view valueName;  // how the help shows that value
  Writes writes;
  // Reports on the state of this PC (status, diagnostics): compares the registry as this
  // process sees it with the real one, without writing anything.
  bool reportsState;
  bool guiOnly;               // only the settings executable knows it
  bool devOnly;               // only in developer builds (the PDFT_DEV_INSTALL build option)
  bool hidden;                // internal: not in --help
  std::wstring_view summary;  // for --help; {product}, {pdfcraft}, {gui} are filled in
};

// Every command, in help order.
std::span<const VerbInfo> Verbs() noexcept;
// The row of |verb|.
const VerbInfo& Describe(Verb verb) noexcept;

// --checks <list>: when the scheduled checks run.
struct ChecksSelection {
  bool afterPdfCraft = true;
  bool atSignIn = true;
  friend bool operator==(const ChecksSelection&, const ChecksSelection&) = default;
};

// The steps the elevated half of a change carries out (--elevated <step>).
enum class ElevatedStep {
  kMachineInstall,
  kMachineRepairClass,
  kMachineUninstall,
  kMachineMigrateV1
};

enum class MigrateScope { kBoth, kUserOnly, kMachineOnly };

struct CliArgs {
  Verb verb = Verb::kNone;
  // Changes per-user state, so the canary guards it: Writes::kYes, or kWithValue with the value
  // given. Never for Writes::kMachine.
  bool writes = false;
  bool relaunched = false;  // --relaunched

  // --status
  bool json = false;
  bool noLiveTest = false;  // also --check
  std::optional<std::wstring> testFile;
  // --check
  bool quiet = false;
  bool noRepair = false;
  // --fix
  bool takeBack = false;
  bool yes = false;  // also --refresh, --uninstall
  // --zoom (the command's value), --install, --setup-user
  std::optional<int> zoom;
  bool noRefresh = false;
  // --checks (the command's value), --install, --setup-user
  std::optional<ChecksSelection> checks;
  // --refresh
  std::vector<std::wstring> folders;
  bool full = false;
  // --install
  Scope scope = Scope::kMachine;
  bool force = false;  // a per-user install over a machine-wide 1.x install
  // --uninstall
  bool rebuildThumbnails = false;
  // --diagnostics
  bool copy = false;
  std::optional<std::wstring> out;
  // --migrate-v1
  MigrateScope migrateScope = MigrateScope::kBoth;
  // --elevated
  std::optional<ElevatedStep> elevatedStep;
};

struct ParseError {
  std::wstring message;  // what is wrong, without the "Run --help" hint the caller adds
};

struct ParseOptions {
  Mode mode = Mode::kCli;
  bool devInstall = true;  // pass kDevInstall (project_config.h)
};

// Parses |args|, the arguments after the program name. May throw std::bad_alloc.
std::variant<CliArgs, ParseError> ParseCommandLine(std::span<const std::wstring_view> args,
                                                   const ParseOptions& options);

// The --help text for |options| (lines separated by "\n"): usage, every command that is not
// hidden with its options, and the exit codes. May throw std::bad_alloc.
std::wstring UsageText(const ParseOptions& options);

// The zoom value syntax (pure): all decimal digits, 100 to 200.
std::optional<int> ParseZoomValue(std::wstring_view text) noexcept;

// The --checks value syntax (pure): pdfcraft,signin | pdfcraft | signin | none | default.
std::optional<ChecksSelection> ParseChecksValue(std::wstring_view text) noexcept;

}  // namespace pdft
