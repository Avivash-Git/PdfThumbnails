// Installing from a build's folder (--install, developer builds), and the administrator steps
// that write for all users: machine-install (developer builds) and machine-repair-class.
//
// A build's folder holds the four binaries (the two executables, the handler and PDFium) and,
// when it is a staged runtime, the notices (LICENSE.txt, THIRD-PARTY-NOTICES.md, licenses\). An
// install root gets the executables and the notices at its top and the two DLLs in
// app\<FileVersion>-<build id>, the id computed from the DLLs' bytes (build_folder.h), so a new
// build never overwrites one that Explorer's thumbnail process still has loaded.
//
// --install [--machine], unelevated, in this order:
//   1. refused where the registry may be virtualized (8) and in an elevated process (8: the
//      per-user half must run as the signed-in user; the step for all users asks for an
//      administrator itself); refused while the installer (Inno Setup) has an install here (8),
//      for a folder on a network drive (8, before anything reads it), for a folder that is not a
//      build of this version for this Windows (8), and for a folder that other accounts could
//      change (8: an administrator would copy and register what they put there; for --user, this
//      user's thumbnail process and sign-in task would run it): the folder, anything that is
//      copied from it or a folder above it that another account owns or may change, or a link,
//      junction or mount point on its path or among what is copied (file_ops.h HoldBuildFolder);
//   2. the operation lock (4; 5 when Windows does not let this process make it);
//   3. this session's thumbnail processes that hold an old build's DLLs are ended (only Windows'
//      own dllhost.exe of this user), so the old builds can go and the new one loads;
//   4. the administrator step machine-install through UAC (declined: 3; started without
//      administrator rights, it refuses: 7; below an app package where its test writes do not
//      come back, it refuses: 8): it checks the folder again and holds it open, so
//      that nothing in it or above it can be renamed, replaced or written until the copy is done,
//      copies the build from what it holds into <Program Files>\<Product>, writes the class for
//      all users, makes the shared task folder, then removes 1.x's and the old 2.x build folders
//      for all users, last, after a short retry for files still in use (those go to a trash
//      folder and are deleted at the next restart). A part after the class that does not
//      complete does not undo the step: it is reported as not done, --install lists it as
//      "machine-install <part>: <HRESULT>" and exits 1;
//   5. the installer's per-user step (setup.h SetupUser);
//   6. the status, with the Explorer test, whose answer the result needs: exit 0 when healthy and
//      the test passed, 1 when installed but not healthy or when the test did not run (could not
//      start), which leaves the install unconfirmed; files waiting for a restart are reported,
//      not an exit code of their own.
// --install --user: the same checks, and all of it unelevated: while a class for all users
//   exists, 1.x's rule applies and the install is for all users instead; a 1.x install for all
//   users refuses it unless --force (Windows' thumbnail process would go on loading 1.x). Pending
//   sign-in clean-ups of the per-user folder go first, then the files 1.x left in it, which this
//   install shares (migrate_v1.h's file part); then the build is copied there from the folder
//   held since its check, the per-user class written, and step 5 and 6 follow.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "build_folder.h"
#include "cli_args.h"
#include "context.h"
#include "elevation.h"
#include "evaluate.h"
#include "exit_code.h"
#include "file_ops.h"
#include "inno.h"
#include "migrate_v1.h"
#include "registry_roots.h"
#include "setup.h"
#include "snapshot.h"
#include "system_probes.h"

namespace pdft {

// What the administrator step machine-install fails with when other accounts could change the
// build's folder: --install maps it to exit 8, like its own check.
inline constexpr HRESULT kUnsafeBuildFolderHr = HRESULT_FROM_WIN32(ERROR_INVALID_SECURITY_DESCR);

// The licenses folder of a build's folder and an install root.
inline constexpr wchar_t kLicensesFolder[] = L"licenses";

// The four binaries every build's folder holds: settings executable, command-line executable,
// handler, PDFium (pure).
std::span<const std::wstring_view> BuildBinaries() noexcept;

// The notice files at the top of a build's folder, copied when present (pure).
std::span<const std::wstring_view> BuildTopNotices() noexcept;

// What HoldBuildFolder holds of a build's folder for its copy: the binaries, the notices at the
// top and licenses\ (at most kMaxNoticeDepth folders deep), for this process's user (pure).
BuildFolderHoldOptions BuildHoldOptions() noexcept;

// A handler FileVersion this program installs (pure): a build folder version of 2 or later.
bool IsInstallableVersion(std::wstring_view version) noexcept;

// ---- Reading a build's folder -----------------------------------------------------------------

// Why a folder cannot be installed from.
enum class SourceProblem {
  kNone,
  kMissingFile,  // one of the four binaries is missing or not a file
  kWrongArch,    // a binary is made for another architecture than Windows
  kNot2x,        // the handler is not a build of version 2 or later
  kUnreadable,   // a binary's headers or version cannot be read
};

struct SourceCheck {
  SourceProblem problem = SourceProblem::kNone;
  std::wstring file;         // the binary concerned
  Arch arch = Arch::kOther;  // with kWrongArch: what it is made for
  HRESULT hr = S_OK;         // with kUnreadable
};

struct BuildSource {
  std::filesystem::path dir;  // canonicalized
  std::wstring version;       // the handler's FileVersion
  std::wstring buildName;     // <version>-<build id>
  Sha256Digest dllSha = {};
  Sha256Digest pdfiumSha = {};
  // The notices found, relative to |dir|: the top ones, then every regular file below
  // licenses\ (at most kMaxNoticeDepth folders deep; links and junctions are skipped here, and
  // refused where the folder is held for its copy).
  std::vector<std::filesystem::path> notices;
};

inline constexpr int kMaxNoticeDepth = 4;

// Reads |dir| as a build to install on a Windows of |osArch|. S_OK with check->problem set when
// it cannot be installed from; a failure only for other errors (the folder cannot be read).
[[nodiscard]] HRESULT ReadBuildSource(const std::filesystem::path& dir, Arch osArch,
                                      BuildSource* source, SourceCheck* check) noexcept;

// ---- Copying a build into an install root -----------------------------------------------------

struct CopyBuildOptions {
  // Elevated only: where a file in use at the target goes (a trash folder, deleted at the next
  // restart). Empty: a file still in use after |retryMs| fails the copy.
  std::filesystem::path trashParent;
  DWORD retryMs = 2000;
  // The build's folder as HoldBuildFolder holds it (BuildHoldOptions): every file is read from
  // its held handle, and one that is not held fails with E_UNEXPECTED. nullptr: by its path.
  const HeldBuildFolder* held = nullptr;
};

struct CopyBuildResult {
  std::filesystem::path buildDir;  // <root>\app\<build name>
  std::filesystem::path dll;       // the handler in it
  bool reused = false;             // the build folder held exactly these DLLs already
  size_t copied = 0;               // files written
  bool restartPending = false;     // a file in use went into a trash folder
  std::filesystem::path trash;
  // The first deletion at the restart that could not be queued: that file stays in its trash
  // folder.
  HRESULT restartDelete = S_OK;
};

// Copies |source| into |root| (made when missing): the DLLs into app\<build name> (an existing
// folder is reused only when both DLLs have the same SHA-256; each copy is verified against the
// hash the build id came from), then the executables and the notices at the top (a file already
// there with the same contents is left alone). Every folder on the way must be a real folder,
// never a link or junction. Fails with the first error.
[[nodiscard]] HRESULT CopyBuild(const BuildSource& source, const std::filesystem::path& root,
                                const CopyBuildOptions& options, CopyBuildResult* result) noexcept;

// ---- The administrator steps (elevated) -------------------------------------------------------

struct MachineInstallOptions {
  std::filesystem::path source;  // the build's folder: the elevated executable's own
  std::filesystem::path machineRoot;
  std::filesystem::path legacyMachineRoot;
  std::filesystem::path trashParent;      // empty: the folder that holds |machineRoot|
  std::wstring taskFolder = kTaskFolder;  // empty: not made (tests)
  // false: the owners and permissions are not judged (tests whose folders above %TEMP% are the
  // PC's); network folders and links are refused, and the folder is held, all the same.
  bool checkSourceAccess = true;
  Arch osArch = Arch::kOther;
  DWORD retryMs = kMachineRemoveRetryMs;
};

struct MachineInstallResult {
  std::optional<UnsafeAccess> unsafe;
  SourceCheck sourceCheck;
  BuildSource source;
  CopyBuildResult copy;
  HRESULT taskFolder = S_OK;
  MachineRemoval v1;   // 1.x's build folders for all users
  MachineRemoval old;  // old 2.x builds
  bool restartPending = false;
  // The parts after the class that did not complete, "<part>: <HRESULT>" (task-folder,
  // restart-delete, migrate-v1, old-builds; reported as not done)
  std::vector<std::wstring> failures;
};

// The step for all users of --install, in this order: the folder checked and held
// (HoldBuildFolder with BuildHoldOptions: kUnsafeBuildFolderHr when it refuses), the build
// (HRESULT_FROM_WIN32(ERROR_BAD_FORMAT) when it is not one to install; its DLLs hashed again from
// what is held), no link on the way to |machineRoot|, CopyBuild from what is held, the class
// (WriteMachineClass), the task folder, then 1.x's and the old 2.x build folders. S_OK once the
// class is written (the parts after it that fail are listed, and so is a file in use that the
// copy could not queue for deletion at the restart); the failure otherwise. Never throws.
[[nodiscard]] HRESULT InstallForAllUsers(const MachineInstallOptions& options,
                                         const RegistryRoots& roots,
                                         MachineInstallResult* result) noexcept;

// The step for all users of --fix: points the class at the newest complete 2.x build for
// |osArch| under <machineRoot>\app (ChooseBuildDll), never at a 1.x build.
// HRESULT_FROM_WIN32(ERROR_NOT_FOUND) when there is none. |dll| (optional) receives it.
[[nodiscard]] HRESULT RepairMachineClass(const RegistryRoots& roots,
                                         const std::filesystem::path& machineRoot, Arch osArch,
                                         std::filesystem::path* dll = nullptr) noexcept;

// The elevated process's steps (ElevatedMain, after its entry checks), against the real
// registry and folders: machine-install with its own folder as the build (|notDone| receives
// MachineInstallResult::failures), and machine-repair-class.
[[nodiscard]] HRESULT RunMachineInstall(ElevatedReporter& report, bool* restartPending,
                                        std::vector<std::wstring>* notDone) noexcept;
[[nodiscard]] HRESULT RunMachineRepairClass(ElevatedReporter& report) noexcept;

// ---- --install ------------------------------------------------------------------------------

enum class InstallRefusal {
  kNone,
  kWrongContext,  // 8: the registry may be virtualized
  kElevated,      // 8
  kInnoInstall,   // 8: the installer has an install here
  kNotABuild,     // 8: SourceCheck says why
  kInsideRoot,    // 8: the build's folder is the install root or inside it
  // 8: other accounts could change the build's folder (|unsafe| says what was found: also a link
  // or a network drive)
  kUnsafeFolder,
  kV1Machine,  // 8: a per-user install over a 1.x install for all users, without --force
};

struct InstallOptions {
  Scope scope = Scope::kMachine;
  std::optional<ChecksSelection> checks;
  std::optional<int> zoom;
  bool force = false;
  // The context the command read with the canary; when absent, probes.GetProcessContext with
  // ContextProbe::kWrite.
  std::optional<ProcessContext> context;
  std::optional<EnginePaths> paths;  // when absent, GetEnginePaths
  std::filesystem::path source;      // the build's folder; empty: this executable's folder
  DWORD lockWaitMs = kCheckLockWaitMs;
  HWND owner = nullptr;
  // Starts the administrator step; tests pass a fake, so no test ever starts a real elevation.
  ElevationLauncher launcher;
  bool liveTest = true;  // the Explorer test in the final status, whose answer the result needs
};

struct InstallResult {
  ExitCode code = ExitCode::kFailed;
  InstallRefusal refusal = InstallRefusal::kNone;
  bool busy = false;
  // exit 5: Windows does not let this process make the lock (kNamedObjectsRefusedHr)
  bool namedObjectsRefused = false;
  std::optional<InnoInstall> inno;
  SourceCheck sourceCheck;
  std::optional<UnsafeAccess> unsafe;
  BuildSource source;
  Scope scope = Scope::kNone;      // what was installed (or asked for, when refused)
  bool switchedToMachine = false;  // --user asked, but a class for all users exists
  size_t holdersEnded = 0;
  std::optional<ElevatedRun> run;  // the administrator step
  size_t runOnceDeleted = 0;       // a per-user install's sign-in clean-ups, before the copy
  CopyBuildResult userCopy;        // a per-user install's copy
  V1FilePlan v1Plan;               // 1.x's per-user files, before the copy
  std::vector<V1FileOutcome> v1Files;
  SetupUserResult setup;
  bool restartPending = false;
  Snapshot after;
  std::vector<Finding> findings;
  // "<step>: <HRESULT>", and "machine-install <part>: <HRESULT>" for each part of the step for
  // all users that did not complete
  std::vector<std::wstring> failures;
};

// The exit code of the administrator step for --install (pure): std::nullopt when it is done (a
// restart pending included), 3 when the prompt was declined, 8 when the step found the build's
// folder unsafe or refused to run below an app package, 7 when it did not get administrator
// rights, else 5; the step's own codes count only from a step that was launched.
std::optional<ExitCode> InstallStepExitCode(const ElevatedRun& run) noexcept;

// Runs --install. S_OK when it came to a verdict (*result's code); a failure when the state
// could not be read. Never throws.
[[nodiscard]] HRESULT InstallFromOwnFolder(const InstallOptions& options,
                                           const RegistryRoots& roots, const SystemProbes& probes,
                                           InstallResult* result) noexcept;

}  // namespace pdft
