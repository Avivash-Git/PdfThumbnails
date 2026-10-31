// Everything the check looks at, gathered once, read-only: the registration, Windows' own view of
// .pdf, Explorer's settings, the user's settings, the check task, PdfCraft, Smart App Control,
// the Explorer test, what 1.x left behind and the build folders. Judging it (evaluate.h) and
// planning repairs (repair.h) then need no I/O, so both are tested with made-up snapshots.
//
// Everything in the registry is read through RegistryRoots and every question only Windows can
// answer goes through SystemProbes, so a test runs the whole gathering against a private
// registry key, folders under %TEMP% and fakes.

#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "build_folder.h"
#include "context.h"
#include "exit_code.h"
#include "explorer_settings.h"
#include "legacy.h"
#include "live_test.h"
#include "pdfcraft.h"
#include "registration.h"
#include "registry_roots.h"
#include "sac.h"
#include "settings.h"
#include "system_probes.h"
#include "tasks.h"

namespace pdft {

// The folder below an install root that holds the build folders.
inline constexpr wchar_t kBuildsFolderName[] = L"app";

// Where the check looks for files. The real paths come from GetEnginePaths; tests point every
// one of them into a folder under %TEMP%.
struct EnginePaths {
  std::filesystem::path userRoot;           // a per-user install's root (GetUserRoot)
  std::filesystem::path machineRoot;        // a machine-wide install's root (GetMachineRoot)
  std::filesystem::path legacyUserRoot;     // 1.x's per-user folder (GetLegacyUserRoot)
  std::filesystem::path legacyMachineRoot;  // 1.x's machine-wide folder
  std::filesystem::path legacyIni;          // the 1.x settings file the handler reads
  std::filesystem::path ownExe;             // this executable
};

// The real paths.
[[nodiscard]] HRESULT GetEnginePaths(EnginePaths* paths) noexcept;

// Why the checks that go through Windows' association code were skipped: they describe the
// registry as this process sees it, which is only Explorer's when nothing redirects it.
enum class WindowsChecksSkip {
  kNone,          // they ran
  kViewsDiffer,   // the read-only comparison found a value that differs
  kViewsUnknown,  // the comparison could not decide
  kVirtualized,   // the canary found (or could not rule out) that writes are swallowed
  kNotChecked,    // a packaged app started this process, and no registry check ran
};

// The decision (pure): a context that ran the canary is judged by it, one that ran the read-only
// comparison by that; without either, only a packaged ancestor is reason to doubt.
WindowsChecksSkip DecideWindowsChecksSkip(const ProcessContext& context) noexcept;

// One build folder with what the check needs to know about its DLL.
struct BuildInfo {
  BuildFolder folder;
  // The handler DLL's PE machine; read only for complete 2.x folders, when it could be read.
  std::optional<USHORT> dllMachine;
};

// A part of the gathering that failed without stopping the rest (the task could not be read,
// Windows Installer failed, ...): its name and why.
struct GatherIssue {
  const wchar_t* part = L"";
  HRESULT hr = S_OK;
};

// The --test-file run.
struct TestFileRun {
  std::filesystem::path requested;  // as given
  std::filesystem::path resolved;   // the file it ends at, when it is usable
  TestFileProblem problem = TestFileProblem::kNone;
  LiveTestSkip skip = LiveTestSkip::kNone;
  std::optional<LiveTest> result;  // when it ran
  HRESULT error = S_OK;            // why it could not start (no probe thread, ...)
};

struct Snapshot {
  ProcessContext context;
  EnginePaths paths;
  std::wstring osBuild;  // "26300.9818"

  // Machine-wide if a machine-wide class exists, else per-user if a per-user class exists, else
  // none (1.x's rule). |installRoot| is that scope's root (empty for none).
  Scope scope = Scope::kNone;
  std::filesystem::path installRoot;
  std::optional<ClassReg> machineClass;
  std::optional<ClassReg> userClass;
  SlotValues slots;
  SlotValues machineSlots;  // for information only
  // What the registry says about the CLSIDs in the slots that are not ours (empty otherwise).
  std::array<ClsidDescription, kSlotCount> slotOwners;

  // Windows' view; not looked at when windowsChecksSkipped.
  bool windowsChecksSkipped = false;
  WindowsChecksSkip windowsChecksSkip = WindowsChecksSkip::kNone;
  std::optional<std::wstring> effective;  // the handler Windows resolves for .pdf
  ClsidDescription effectiveOwner;        // when it is not ours
  DefaultApp defaultApp;
  ClsidDescription defaultAppHandlerOwner;  // when the default app brings a handler not ours

  ExplorerSettings explorer;
  Settings settings;
  ZoomDetails zoom;

  TaskLocation taskLocation;
  TaskState task;
  PdfCraftInfo pdfcraft;
  std::optional<CheckRecord> last;  // the previous recorded check
  bool migratedFromV1 = false;

  SacReading sac;
  // The Explorer test: its result when it ran, else why not (liveSkip).
  std::optional<LiveTest> live;
  LiveTestSkip liveSkip = LiveTestSkip::kNone;
  HRESULT liveError = S_OK;  // the test was due but could not start (no probe file, no thread)
  std::optional<BlockCount> blocks;  // counted only when DecideSac needs them
  SacVerdict sacVerdict;
  std::optional<TestFileRun> testFile;

  LegacyState legacy;
  std::vector<BuildInfo> userBuilds;     // <userRoot>\app\*, every real folder
  std::vector<BuildInfo> machineBuilds;  // <machineRoot>\app\*

  std::vector<GatherIssue> issues;

  // The class of |scope|; nullptr for none.
  const ClassReg* ActiveClass() const noexcept;
  // The effective handler is ours (false when the Windows checks were skipped).
  bool EffectiveIsOurs() const noexcept;
  // DisableThumbnails is set for the user or for the machine.
  bool ThumbnailsDisabledByPolicy() const noexcept {
    return explorer.thumbnailsOffByUserPolicy || explorer.thumbnailsOffByMachinePolicy;
  }
};

struct GatherOptions {
  bool liveTest = true;
  std::optional<std::filesystem::path> testFile;
  // The process context the command already read; when absent, probes.GetProcessContext with
  // ContextProbe::kReadOnly.
  std::optional<ProcessContext> context;
  // When absent, GetEnginePaths.
  std::optional<EnginePaths> paths;
};

// Gathers in the check's order: own exe, install roots and build folders; both classes; the
// slots; the scope; Windows' view (skipped when the registry view may not be Explorer's); Explorer
// settings and policies; settings and the zoom's origin; the task; PdfCraft and the last record;
// Smart App Control; 1.x leftovers; the Explorer test (only when Windows resolves our handler and
// this process is neither elevated nor virtualized); Code Integrity blocks (ShouldCountBlocks,
// once more after kSacRecountDelayMs when ShouldRecountBlocks). Read-only, but the Explorer test
// writes and deletes a probe PDF in %TEMP%. Registry reads that fail fail the gathering; the
// Windows-wide parts that fail are listed in |snapshot->issues| and read as "nothing found".
[[nodiscard]] HRESULT GatherSnapshot(const RegistryRoots& roots, const GatherOptions& options,
                                     const SystemProbes& probes, Snapshot* snapshot) noexcept;

// The scope by 1.x's rule (pure).
Scope DecideScope(const std::optional<ClassReg>& machineClass,
                  const std::optional<ClassReg>& userClass) noexcept;

// The per-user build a per-user class should point at (pure): among the complete 2.x folders of
// |snapshot.userBuilds| whose DLL is made for |snapshot.context.osArch|, the newest by FileVersion
// (from the folder name), then by build id. std::nullopt when there is none.
std::optional<std::filesystem::path> ChooseUserBuildDll(const Snapshot& snapshot);

// |dll| lies directly in |folder| (pure, lexical, ignoring case).
bool DllIsInFolder(const std::filesystem::path& dll, const std::filesystem::path& folder);

}  // namespace pdft
