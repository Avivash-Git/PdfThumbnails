// The exit codes of both executables: part of the command-line contract that scripts, scheduled
// tasks and the installer rely on, so the values never change. Also two small enums that the
// command line and the checks share.

#pragma once

namespace pdft {

enum class ExitCode : int {
  kOk = 0,            // done; healthy
  kProblems = 1,      // problems remain, or a step is incomplete (e.g. a restart is pending)
  kUsage = 2,         // the command line is wrong
  kCancelled = 3,     // the user cancelled (declined UAC, answered "no", Ctrl+C before a change)
  kBusy = 4,          // another operation held the lock for too long
  kFailed = 5,        // failed; the log has the HRESULT
  kNotInstalled = 6,  // the product is not installed
  kNeedsAdmin = 7,    // needs administrator rights, and nobody can be asked (or UAC gave none)
  // cannot run here (a virtualized container, a step for all users whose test writes did not
  // reach Windows, or elevated where refused)
  kWrongContext = 8,
};

// One row of the exit-code table that --help prints.
struct ExitCodeInfo {
  ExitCode code;
  const wchar_t* meaning;
};

// Every exit code, in order: --help prints this table, so the help cannot miss a code.
inline constexpr ExitCodeInfo kExitCodes[] = {
    {ExitCode::kOk, L"OK"},
    {ExitCode::kProblems, L"problems remain, or a step is incomplete"},
    {ExitCode::kUsage, L"usage error"},
    {ExitCode::kCancelled,
     L"cancelled (the administrator prompt was declined, the answer was no, or Ctrl+C stopped it "
     L"before anything changed)"},
    {ExitCode::kBusy, L"busy: another check or change did not finish within 30 seconds"},
    {ExitCode::kFailed, L"failed (the log has the error code)"},
    {ExitCode::kNotInstalled, L"not installed"},
    {ExitCode::kNeedsAdmin,
     L"needs administrator rights, and nobody can be asked (or the step for all users did not "
     L"get them)"},
    {ExitCode::kWrongContext,
     L"cannot run here (an app container that virtualizes the registry, a step for all users "
     L"below an app package whose test writes did not reach Windows, or elevated where that is "
     L"refused)"},
};

// The table is in value order without gaps.
consteval bool ExitCodesInOrder() {
  int expected = 0;
  for (const ExitCodeInfo& row : kExitCodes) {
    if (static_cast<int>(row.code) != expected++) return false;
  }
  return true;
}
static_assert(ExitCodesInOrder());

// Where the product is installed: for all users, for the signed-in user, or not at all.
enum class Scope { kNone, kUser, kMachine };

// A processor architecture: of a process, of Windows itself, or of a DLL.
enum class Arch { kX64, kArm64, kOther };

}  // namespace pdft
