// The clean-up after version 1.x: what --migrate-v1 does, and what the installer steps and the
// check reuse of it.
//
// User part (unelevated, as the user, in this order):
//   1. the sign-in clean-up values (RunOnce) go first: 1.x's would delete the per-user folder,
//      which a per-user install of this version shares;
//   2. the zoom: 1.x's settings.ini value becomes the registry value, only when there is none;
//   3. 1.x's task: this version's check task is registered first (both triggers, unless the user
//      already has one), then 1.x's is deleted, so its repairs never fight this version's;
//   4. a per-user class in a 1.x build folder: pointed at the newest per-user 2.x build, or deleted
//      when a machine-wide class takes over; before any file goes, so it never names a deleted DLL;
//   5. the files 1.x left in its per-user folder (the file part below);
//   6. the .pdf slots stay (the class id is the same); State\MigratedFromV1 = 1.
//
// The file part deletes only:
//   - files 1.x shipped (its manager and launcher, README.md, LICENSE, licenses\*, bin\*) whose
//     contents are in the committed table of 1.x files (v1_files.inc, made by
//     cmake/scripts/V1FileTable.cmake), and only while the 1.x manager is still there and nothing
//     of a 2.x install is in the folder. The manager goes last, once the rest of that group went,
//     so the next run finishes an interrupted one. licenses\ and bin\ go only once empty: a
//     folder whose name a 2.x install also uses is never deleted with what is in it;
//   - files only 1.x writes (settings.ini, once the registry holds a zoom, and its logs), by name,
//     as regular files;
//   - app\1.* build folders, by name, when no process holds a file in them and no class points
//     into them.
// Links and junctions are never followed, and never deleted either.
//
// Machine part (elevated: --elevated machine-migrate-v1 and --setup-machine): the app\1.*
// folders under 1.x's machine-wide folder, except the one the machine-wide class still points at
// (installing this version for all users repoints it). Files in use go to a trash folder and are
// deleted at the next restart.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "build_folder.h"
#include "legacy.h"
#include "registry_roots.h"
#include "snapshot.h"
#include "system_probes.h"

namespace pdft {

// ---- The table of 1.x files (pure) --------------------------------------------------------

// The digests the table holds for |relative|, a path below the per-user folder ("LICENSE",
// "licenses\\zlib.txt"), compared ignoring case; none when the table does not name it. May throw
// std::bad_alloc.
std::vector<Sha256Digest> V1ShippedDigests(std::wstring_view relative);

// The table names |relative|.
bool IsV1ShippedFile(std::wstring_view relative) noexcept;

// 64 hexadecimal digits as a digest (pure); std::nullopt for anything else.
std::optional<Sha256Digest> DigestFromHex(std::string_view hex) noexcept;

// ---- The file part: planning (pure) -----------------------------------------------------------

// One entry of 1.x's per-user folder, by its path below the folder.
struct V1TreeEntry {
  std::wstring path;  // "settings.ini", "licenses\\zlib.txt", "app\\1.1.0.0-67a1323e"
  bool directory = false;
  bool reparsePoint = false;
  std::optional<Sha256Digest> sha;  // regular files the table names, when they could be hashed
};

// What ReadV1Tree lists: the folder's entries, and those of its licenses\, bin\ and app\ folders
// when they are real folders. Nothing deeper.
struct V1Tree {
  std::vector<V1TreeEntry> entries;
};

struct V1FileStep {
  enum class Kind {
    kDeleteFile,         // a file (byContent: only with contents the table holds)
    kRemoveEmptyFolder,  // licenses\ or bin\, only if it is empty by then
    kRemoveBuildFolder,  // an app\1.* folder with everything in it
  };
  Kind kind = Kind::kDeleteFile;
  std::wstring path;
  bool byContent = false;
  // The 1.x manager: deleted only when every by-content step before it went.
  bool marker = false;
};

// Why something 1.x may have left is not deleted.
enum class V1KeepReason {
  kNone,
  kContentDiffers,  // not the contents 1.x shipped (changed, or a later file of the same name)
  kNotHashed,       // its contents could not be read
  kNoManager,       // the 1.x manager is gone, so a shared name is not proof enough
  k2xInstall,       // files of a 2.x install are in the folder, so a shared name may be theirs
  kHeld,            // a process holds a file in the build folder
  kClassUsesIt,     // a class points into the build folder
  kLink,            // a link or junction
  kZoomNotSaved,    // settings.ini, while the registry holds no zoom
  kNotEmpty,        // a folder that still holds something
  kInUse,           // a file in use, at the time of deleting
  kWaiting,         // the manager, while something before it is still there
};

struct V1Kept {
  std::wstring path;
  V1KeepReason reason = V1KeepReason::kNone;
};

struct V1FilePlan {
  std::vector<V1FileStep> steps;  // in the order they are applied
  std::vector<V1Kept> kept;       // what the folder holds of 1.x's but stays, and why
};

struct V1PlanInput {
  std::vector<std::wstring> heldBuilds;   // names of app\1.* folders a process holds files in
  std::vector<std::wstring> classBuilds;  // ... a class points into
  bool zoomSaved = false;                 // the registry holds a zoom, so settings.ini may go
};

// The steps for |tree| by the rules at the top (pure). The order: 1.x's logs and settings.ini,
// the build folders, licenses\ and bin\ with their files, README.md and LICENSE, and the manager
// last. May throw std::bad_alloc.
V1FilePlan PlanV1UserFiles(const V1Tree& tree, const V1PlanInput& input);

// ---- The file part: reading and applying ------------------------------------------------------

// Lists |root| for PlanV1UserFiles, hashing the regular files the table names. Reads only and
// follows no link. A missing |root|, or one that is a link itself, gives an empty tree.
[[nodiscard]] HRESULT ReadV1Tree(const std::filesystem::path& root, V1Tree* tree) noexcept;

// What became of one step.
struct V1FileOutcome {
  V1FileStep step;
  enum class Result { kDone, kUnchanged, kKept, kFailed } result = Result::kFailed;
  V1KeepReason reason = V1KeepReason::kNone;  // with kKept
  HRESULT hr = S_OK;                          // with kFailed
};

// Applies |plan| under |root|, each step checked again as it runs: the folder and the folders on
// the way must be real folders, a file is hashed again through the handle that deletes it
// (DeleteFileIfContent), a folder is removed only if empty, and a build folder only when
// |probes| finds no holder (then without following links, and unelevated, leaving what is in use
// in place). Never throws; fails only when out of memory.
[[nodiscard]] HRESULT ApplyV1UserFiles(const std::filesystem::path& root, const V1FilePlan& plan,
                                       const SystemProbes& probes,
                                       std::vector<V1FileOutcome>* outcomes) noexcept;

// ---- The user part ----------------------------------------------------------------------------

struct V1UserOptions {
  // Register this version's check task before 1.x's is deleted; false when the caller has just
  // registered it (the installer's user step).
  bool registerTask = true;
  bool files = true;  // the file part
  // End this session's thumbnail processes that hold a file in an app\1.* folder (only Windows'
  // own dllhost.exe of this user), so the folder can go.
  bool endHolders = true;
  // The registry already holds the zoom the caller wants: never write 1.x's.
  bool keepZoom = false;
};

struct V1UserResult {
  size_t runOnceDeleted = 0;
  std::optional<int> zoomMigrated;  // the zoom written from settings.ini
  bool taskRegistered = false;
  bool legacyTaskDeleted = false;
  enum class ClassChange { kNone, kRepointed, kDeleted, kKept } userClass = ClassChange::kNone;
  size_t holdersEnded = 0;
  V1FilePlan plan;
  std::vector<V1FileOutcome> files;
  bool marked = false;  // State\MigratedFromV1 written
  // Every step that failed, as "<step>: <HRESULT>" (no paths).
  std::vector<std::wstring> failures;

  // Something stays for a later run: a file or folder in use, or held.
  bool Incomplete() const noexcept;
};

// The user part for the state |snapshot| (gathered before; every step reads what it changes
// again). Needs an installed 2.x program for the task (InstalledSettingsExe); without one the
// task step is left out and 1.x's task stays. Refuses an elevated |snapshot.context| with
// E_ACCESSDENIED: everything here belongs to the signed-in user, in their profile. S_OK when every
// step ran (failures are listed, not returned); fails only when out of memory.
[[nodiscard]] HRESULT MigrateV1User(const RegistryRoots& roots, const SystemProbes& probes,
                                    const Snapshot& snapshot, const V1UserOptions& options,
                                    V1UserResult* result) noexcept;

// The file part alone, for 1.x's per-user folder of |snapshot| (what the check retries once the
// clean-up has run). Never ends a process. Same refusal as MigrateV1User.
[[nodiscard]] HRESULT RemoveV1UserFiles(const RegistryRoots& roots, const SystemProbes& probes,
                                        const Snapshot& snapshot, V1FilePlan* plan,
                                        std::vector<V1FileOutcome>* outcomes) noexcept;

// ---- Build folders for all users (the machine part and old builds) --------------------------

// How long a build folder for all users that is in use is retried before its files go to the
// trash folder.
inline constexpr DWORD kMachineRemoveRetryMs = 2000;

struct MachineBuildPlan {
  std::vector<std::wstring> remove;  // folder names below <root>\app, in order
  std::optional<std::wstring> keep;  // the folder the machine-wide class points into
  std::vector<std::wstring> links;   // links and junctions: never touched
};

// The 1.x build folders among |app| (the entries of <root>\app) that may go: every app\1.* real
// folder except the one |classDll| (the machine-wide class's DLL, if any) lies in (pure). May
// throw std::bad_alloc.
MachineBuildPlan PlanMachineV1Removal(std::span<const FolderEntry> app,
                                      const std::filesystem::path& root,
                                      const std::optional<std::wstring>& classDll);

// The old 2.x build folders among |app|: every real folder named like a 2.x build except the one
// |classDll| lies in, and only when that one is a complete 2.x build (|classBuildComplete|), so a
// class that points nowhere sound never costs the builds that could repair it (pure). May throw
// std::bad_alloc.
MachineBuildPlan PlanOldMachineBuilds(std::span<const FolderEntry> app,
                                      const std::filesystem::path& root,
                                      const std::optional<std::wstring>& classDll,
                                      bool classBuildComplete);

struct MachineRemoval {
  std::vector<std::wstring> removed;  // folder names
  std::vector<std::wstring> left;     // ... some of whose files are still in place, in use
  std::vector<std::wstring> failed;
  bool restartPending = false;  // files wait in a trash folder for the next restart
  bool classInV1 = false;       // the machine-wide class still points into a 1.x build folder
  // The machine-wide class's path is stored to be expanded (REG_EXPAND_SZ), which these steps
  // never do: which build it loads is not known, so every build folder stayed.
  bool classForeign = false;
  HRESULT firstError = S_OK;
};

// Removes |plan.remove| below <root>\app (RemoveTreeOrTrash with |trashParent|, |retryMs|). The
// root and its app folder must be real folders, and no link may lead to them from |trashParent|
// down. Fails only for that, or when out of memory; per-folder failures are in |result|.
[[nodiscard]] HRESULT RemoveMachineBuilds(const std::filesystem::path& root,
                                          const MachineBuildPlan& plan,
                                          const std::filesystem::path& trashParent, DWORD retryMs,
                                          MachineRemoval* result) noexcept;

// The machine part: reads the machine-wide class under roots.machine itself, lists
// <legacyMachineRoot>\app and removes its 1.x folders (PlanMachineV1Removal). Elevated only for
// the trash; unelevated, a folder in use stays. The class's path is read as stored, never
// expanded (ClassPathExpansion::kNever): one stored as REG_EXPAND_SZ is foreign, and then no
// folder goes (result->classForeign). The same holds for the two below.
[[nodiscard]] HRESULT MigrateV1Machine(const RegistryRoots& roots,
                                       const std::filesystem::path& legacyMachineRoot,
                                       const std::filesystem::path& trashParent,
                                       MachineRemoval* result) noexcept;

// The old 2.x builds below <machineRoot>\app (PlanOldMachineBuilds), the class read the same way.
[[nodiscard]] HRESULT RemoveOldMachineBuilds(const RegistryRoots& roots,
                                             const std::filesystem::path& machineRoot,
                                             const std::filesystem::path& trashParent,
                                             MachineRemoval* result) noexcept;

// Whether the machine part has anything to do: |legacyMachineRoot|'s app folder holds a 1.x
// build folder the class does not use. Reads only.
[[nodiscard]] HRESULT MachineV1RemovalNeeded(const RegistryRoots& roots,
                                             const std::filesystem::path& legacyMachineRoot,
                                             bool* needed) noexcept;

// A per-user install of this version over a machine-wide 1.x install (pure): the machine-wide
// class, which Windows prefers, would go on loading 1.x, so such an install is refused unless
// forced.
bool PerUserInstallBlockedByV1Machine(const LegacyState& legacy) noexcept;

}  // namespace pdft
