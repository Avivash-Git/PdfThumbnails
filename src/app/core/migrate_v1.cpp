#include "migrate_v1.h"

#include <wil/result_macros.h>

#include <algorithm>
#include <utility>

#include "file_ops.h"
#include "paths.h"
#include "project_config.h"
#include "registration.h"
#include "settings.h"
#include "status_format.h"
#include "strings.h"
#include "tasks.h"
#include "zoom.h"

namespace pdft {
namespace {

struct V1ShippedFile {
  const wchar_t* path;
  const char* sha256;
};

constexpr V1ShippedFile kV1ShippedFiles[] = {
#include "v1_files.inc"
};

constexpr wchar_t kLicensesFolder[] = L"licenses";
constexpr wchar_t kBinFolder[] = L"bin";
constexpr wchar_t kSettingsIni[] = L"settings.ini";

bool IsV1Version(std::wstring_view version) noexcept {
  return version == L"1" || version.starts_with(L"1.");
}

HRESULT LastErrorOr(HRESULT fallback) noexcept {
  const DWORD error = GetLastError();
  return error == ERROR_SUCCESS ? fallback : HRESULT_FROM_WIN32(error);
}

bool IsMissing(HRESULT hr) noexcept {
  return hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
         hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}

// The folder |path| exists as a real folder: not a file, not a link or junction.
HRESULT CheckRealFolder(const std::filesystem::path& path) noexcept {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) return LastErrorOr(E_FAIL);
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
    return HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  }
  if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
  return S_OK;
}

// The regular files directly in |dir|.
HRESULT TopLevelFiles(const std::filesystem::path& dir, std::vector<std::filesystem::path>* files) {
  files->clear();
  std::vector<FolderEntry> entries;
  RETURN_IF_FAILED(ListFolderEntries(dir, &entries));
  for (const FolderEntry& entry : entries) {
    if (!entry.directory && !entry.reparsePoint) files->push_back(dir / entry.name);
  }
  return S_OK;
}

bool Contains(const std::vector<std::wstring>& names, std::wstring_view name) {
  return std::any_of(names.begin(), names.end(),
                     [&](const std::wstring& candidate) { return IEquals(candidate, name); });
}

// The name of the folder below <root>\app that |dll| lies directly in, if it does.
std::optional<std::wstring> BuildFolderOf(const std::wstring& dll,
                                          const std::filesystem::path& root) {
  if (dll.empty()) return std::nullopt;
  const std::filesystem::path file = std::filesystem::path(dll).lexically_normal();
  const std::filesystem::path app = (root / kBuildsFolderName).lexically_normal();
  if (!file.has_parent_path() ||
      !IEquals(file.parent_path().parent_path().native(), app.native())) {
    return std::nullopt;
  }
  return file.parent_path().filename().native();
}

// The folder name and the rest of |path| ("licenses\\zlib.txt" -> "licenses", "zlib.txt"); the
// folder is empty for a top-level name.
std::pair<std::wstring_view, std::wstring_view> SplitTop(std::wstring_view path) noexcept {
  const size_t slash = path.find(L'\\');
  if (slash == std::wstring_view::npos) return {std::wstring_view(), path};
  return {path.substr(0, slash), path.substr(slash + 1)};
}

// The file part, with what it needs read again now.
HRESULT RunFilePart(const RegistryRoots& roots, const SystemProbes& probes,
                    const Snapshot& snapshot, bool endHolders, V1FilePlan* plan,
                    std::vector<V1FileOutcome>* outcomes, size_t* holdersEnded) {
  *plan = {};
  outcomes->clear();
  if (holdersEnded) *holdersEnded = 0;
  const std::filesystem::path& root = snapshot.paths.legacyUserRoot;
  if (root.empty()) return S_OK;
  V1Tree tree;
  RETURN_IF_FAILED(ReadV1Tree(root, &tree));
  if (tree.entries.empty()) return S_OK;

  V1PlanInput input;
  std::optional<ClassReg> userClass;
  std::optional<ClassReg> machineClass;
  RETURN_IF_FAILED(ReadClass(roots, Scope::kUser, &userClass));
  RETURN_IF_FAILED(ReadClass(roots, Scope::kMachine, &machineClass));
  for (const std::optional<ClassReg>* reg : {&userClass, &machineClass}) {
    if (!*reg) continue;
    if (std::optional<std::wstring> folder = BuildFolderOf((*reg)->dll, root)) {
      input.classBuilds.push_back(std::move(*folder));
    }
  }
  const std::wstring appPrefix = std::wstring(kBuildsFolderName) + L"\\";
  for (const V1TreeEntry& entry : tree.entries) {
    if (!entry.directory || entry.reparsePoint || !IStartsWith(entry.path, appPrefix)) continue;
    const std::wstring name = entry.path.substr(appPrefix.size());
    if (!IsV1BuildFolderName(name) || Contains(input.classBuilds, name)) continue;
    std::vector<std::filesystem::path> files;
    if (FAILED(TopLevelFiles(root / entry.path, &files)) || files.empty()) continue;
    std::vector<Holder> holders;
    if (FAILED(probes.FindHolders(files, &holders)) || holders.empty()) continue;
    if (endHolders) {
      for (const Holder& holder : holders) {
        if (probes.EndHolder(holder) == S_OK && holdersEnded) ++*holdersEnded;
      }
      holders.clear();
      if (FAILED(probes.FindHolders(files, &holders))) holders.clear();
    }
    if (!holders.empty()) input.heldBuilds.push_back(name);
  }
  std::optional<RegistryValue> zoom;
  RETURN_IF_FAILED(ReadRegistryValue(roots.user, kRegKey, kZoomValueName, &zoom));
  input.zoomSaved = zoom.has_value();

  *plan = PlanV1UserFiles(tree, input);
  return ApplyV1UserFiles(root, *plan, probes, outcomes);
}

}  // namespace

// ---- The table ----------------------------------------------------------------------------

std::optional<Sha256Digest> DigestFromHex(std::string_view hex) noexcept {
  if (hex.size() != 2 * kSha256Bytes) return std::nullopt;
  const auto value = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  Sha256Digest digest = {};
  for (size_t i = 0; i < kSha256Bytes; ++i) {
    const int high = value(hex[2 * i]);
    const int low = value(hex[2 * i + 1]);
    if (high < 0 || low < 0) return std::nullopt;
    digest[i] = static_cast<BYTE>(high * 16 + low);
  }
  return digest;
}

std::vector<Sha256Digest> V1ShippedDigests(std::wstring_view relative) {
  std::vector<Sha256Digest> digests;
  for (const V1ShippedFile& file : kV1ShippedFiles) {
    if (!IEquals(file.path, relative)) continue;
    if (std::optional<Sha256Digest> digest = DigestFromHex(file.sha256)) digests.push_back(*digest);
  }
  return digests;
}

bool IsV1ShippedFile(std::wstring_view relative) noexcept {
  return std::any_of(std::begin(kV1ShippedFiles), std::end(kV1ShippedFiles),
                     [&](const V1ShippedFile& file) { return IEquals(file.path, relative); });
}

// ---- Planning -------------------------------------------------------------------------------

V1FilePlan PlanV1UserFiles(const V1Tree& tree, const V1PlanInput& input) {
  std::vector<FolderEntry> top;
  for (const V1TreeEntry& entry : tree.entries) {
    if (entry.path.find(L'\\') == std::wstring::npos) {
      top.push_back({entry.path, entry.directory, entry.reparsePoint});
    }
  }
  const auto findTop = [&](std::wstring_view name) -> const V1TreeEntry* {
    for (const V1TreeEntry& entry : tree.entries) {
      if (IEquals(entry.path, name)) return &entry;
    }
    return nullptr;
  };
  const auto children = [&](std::wstring_view folder) {
    std::vector<const V1TreeEntry*> found;
    for (const V1TreeEntry& entry : tree.entries) {
      const auto [parent, rest] = SplitTop(entry.path);
      if (!rest.empty() && IEquals(parent, folder) && rest.find(L'\\') == std::wstring_view::npos) {
        found.push_back(&entry);
      }
    }
    return found;
  };

  V1FilePlan plan;
  const auto step = [&](V1FileStep::Kind kind, const std::wstring& path) -> V1FileStep& {
    plan.steps.push_back(V1FileStep{kind, path});
    return plan.steps.back();
  };
  const auto keep = [&](const std::wstring& path, V1KeepReason reason) {
    plan.kept.push_back({path, reason});
  };

  // 1. What only 1.x writes: its logs, then settings.ini (once the zoom is safe in the registry).
  for (const bool settings : {false, true}) {
    for (const FolderEntry& entry : top) {
      if (!IsV1GeneratedFileName(entry.name) || IEquals(entry.name, kSettingsIni) != settings) {
        continue;
      }
      if (entry.reparsePoint) {
        keep(entry.name, V1KeepReason::kLink);
      } else if (entry.directory) {
        continue;  // never 1.x's
      } else if (settings && !input.zoomSaved) {
        keep(entry.name, V1KeepReason::kZoomNotSaved);
      } else {
        step(V1FileStep::Kind::kDeleteFile, entry.name);
      }
    }
  }

  // 2. 1.x build folders, by name.
  if (const V1TreeEntry* app = findTop(kBuildsFolderName);
      app && app->directory && !app->reparsePoint) {
    for (const V1TreeEntry* entry : children(kBuildsFolderName)) {
      const std::wstring name(SplitTop(entry->path).second);
      if (!IsV1BuildFolderName(name)) continue;
      if (entry->reparsePoint) {
        keep(entry->path, V1KeepReason::kLink);
      } else if (!entry->directory) {
        continue;
      } else if (Contains(input.classBuilds, name)) {
        keep(entry->path, V1KeepReason::kClassUsesIt);
      } else if (Contains(input.heldBuilds, name)) {
        keep(entry->path, V1KeepReason::kHeld);
      } else {
        step(V1FileStep::Kind::kRemoveBuildFolder, entry->path);
      }
    }
  }

  // 3. What 1.x shipped, by contents, while the folder is still 1.x's alone. When it is not, only
  // files whose contents are 1.x's are reported: anything else there is not 1.x's to report.
  const V1KeepReason gate = !HasV1Manager(top)       ? V1KeepReason::kNoManager
                            : Has2xInstallFiles(top) ? V1KeepReason::k2xInstall
                                                     : V1KeepReason::kNone;
  const auto contentIs1x = [&](const V1TreeEntry& entry) {
    if (!entry.sha) return false;
    const std::vector<Sha256Digest> digests = V1ShippedDigests(entry.path);
    return std::find(digests.begin(), digests.end(), *entry.sha) != digests.end();
  };
  // Plans the deletion of one shipped file; false when it stays.
  const auto shipped = [&](const V1TreeEntry& entry, bool marker) {
    if (entry.directory || !IsV1ShippedFile(entry.path)) return false;
    if (entry.reparsePoint) {
      keep(entry.path, V1KeepReason::kLink);
      return false;
    }
    if (gate != V1KeepReason::kNone) {
      if (contentIs1x(entry)) keep(entry.path, gate);
      return false;
    }
    if (!entry.sha) {
      keep(entry.path, V1KeepReason::kNotHashed);
      return false;
    }
    if (!contentIs1x(entry)) {
      keep(entry.path, V1KeepReason::kContentDiffers);
      return false;
    }
    V1FileStep& planned = step(V1FileStep::Kind::kDeleteFile, entry.path);
    planned.byContent = true;
    planned.marker = marker;
    return true;
  };

  for (const wchar_t* folder : {kLicensesFolder, kBinFolder}) {
    const V1TreeEntry* entry = findTop(folder);
    if (!entry || (!entry->directory && !entry->reparsePoint)) continue;
    if (entry->reparsePoint) {
      if (gate == V1KeepReason::kNone) keep(entry->path, V1KeepReason::kLink);
      continue;
    }
    bool all = true;
    for (const V1TreeEntry* child : children(folder)) {
      if (!shipped(*child, false)) all = false;
    }
    if (gate != V1KeepReason::kNone) continue;
    if (all) {
      step(V1FileStep::Kind::kRemoveEmptyFolder, entry->path);
    } else {
      keep(entry->path, V1KeepReason::kNotEmpty);
    }
  }

  // 4. The documents next to the manager, then 5. the manager itself, last.
  const std::wstring legacyName = kLegacyRootName;
  const std::wstring markers[] = {legacyName + L".cmd", legacyName + L".ps1"};
  for (const FolderEntry& name : top) {
    if (IEquals(name.name, markers[0]) || IEquals(name.name, markers[1])) continue;
    if (const V1TreeEntry* entry = findTop(name.name)) (void)shipped(*entry, false);
  }
  for (const std::wstring& marker : markers) {
    if (const V1TreeEntry* entry = findTop(marker)) (void)shipped(*entry, true);
  }
  return plan;
}

// ---- Reading and applying -----------------------------------------------------------------------

HRESULT ReadV1Tree(const std::filesystem::path& root, V1Tree* tree) noexcept try {
  tree->entries.clear();
  if (FAILED(CheckRealFolder(root))) return S_OK;  // nothing there, or a link not ours to follow
  const auto add = [&](const std::wstring& relative, const FolderEntry& entry) {
    V1TreeEntry item{relative, entry.directory, entry.reparsePoint};
    if (!entry.directory && !entry.reparsePoint && IsV1ShippedFile(relative)) {
      Sha256Digest digest = {};
      if (SUCCEEDED(Sha256File(root / relative, &digest))) item.sha = digest;
    }
    tree->entries.push_back(std::move(item));
  };
  std::vector<FolderEntry> top;
  RETURN_IF_FAILED(ListFolderEntries(root, &top));
  for (const FolderEntry& entry : top) add(entry.name, entry);
  for (const FolderEntry& entry : top) {
    if (!entry.directory || entry.reparsePoint) continue;
    if (!IEquals(entry.name, kLicensesFolder) && !IEquals(entry.name, kBinFolder) &&
        !IEquals(entry.name, kBuildsFolderName)) {
      continue;
    }
    std::vector<FolderEntry> inner;
    RETURN_IF_FAILED(ListFolderEntries(root / entry.name, &inner));
    for (const FolderEntry& child : inner) add(entry.name + L"\\" + child.name, child);
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT ApplyV1UserFiles(const std::filesystem::path& root, const V1FilePlan& plan,
                         const SystemProbes& probes, std::vector<V1FileOutcome>* outcomes) noexcept
    try {
  outcomes->clear();
  using Result = V1FileOutcome::Result;
  bool groupIncomplete = false;  // a by-content step (or its folder) did not go
  for (const V1FileStep& step : plan.steps) {
    V1FileOutcome outcome;
    outcome.step = step;
    const auto finish = [&](Result result, V1KeepReason reason = V1KeepReason::kNone,
                            HRESULT hr = S_OK) {
      outcome.result = result;
      outcome.reason = result == Result::kKept ? reason : V1KeepReason::kNone;
      outcome.hr = result == Result::kFailed ? hr : S_OK;
    };
    const std::filesystem::path full = root / step.path;
    // The folder and every folder on the way to the entry must be real folders now.
    HRESULT checked = CheckRealFolder(root);
    if (SUCCEEDED(checked) && full.parent_path() != root) {
      checked = CheckRealFolder(full.parent_path());
    }
    if (step.marker && groupIncomplete) {
      finish(Result::kKept, V1KeepReason::kWaiting);
    } else if (IsMissing(checked)) {
      finish(Result::kUnchanged);
    } else if (checked == HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK)) {
      finish(Result::kKept, V1KeepReason::kLink);
    } else if (FAILED(checked)) {
      finish(Result::kFailed, V1KeepReason::kNone, checked);
    } else {
      switch (step.kind) {
        case V1FileStep::Kind::kDeleteFile: {
          const std::vector<Sha256Digest> digests =
              step.byContent ? V1ShippedDigests(step.path) : std::vector<Sha256Digest>();
          if (step.byContent && digests.empty()) {
            // Never by contents without contents to compare with.
            finish(Result::kKept, V1KeepReason::kContentDiffers);
            break;
          }
          ContentDelete result = ContentDelete::kMissing;
          const HRESULT hr = DeleteFileIfContent(full, digests, &result);
          if (FAILED(hr)) {
            finish(Result::kFailed, V1KeepReason::kNone, hr);
            break;
          }
          switch (result) {
            case ContentDelete::kDeleted:
              finish(Result::kDone);
              break;
            case ContentDelete::kMissing:
              finish(Result::kUnchanged);
              break;
            case ContentDelete::kMismatch:
              finish(Result::kKept, V1KeepReason::kContentDiffers);
              break;
            case ContentDelete::kNotAFile:
              finish(Result::kKept, V1KeepReason::kLink);
              break;
            case ContentDelete::kInUse:
              finish(Result::kKept, V1KeepReason::kInUse);
              break;
          }
          break;
        }
        case V1FileStep::Kind::kRemoveEmptyFolder: {
          const HRESULT real = CheckRealFolder(full);
          if (IsMissing(real)) {
            finish(Result::kUnchanged);
          } else if (real == HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK)) {
            finish(Result::kKept, V1KeepReason::kLink);
          } else if (FAILED(real)) {
            finish(Result::kFailed, V1KeepReason::kNone, real);
          } else if (RemoveDirectoryW(full.c_str())) {
            finish(Result::kDone);
          } else if (GetLastError() == ERROR_DIR_NOT_EMPTY) {
            finish(Result::kKept, V1KeepReason::kNotEmpty);
          } else {
            finish(Result::kFailed, V1KeepReason::kNone, LastErrorOr(E_FAIL));
          }
          break;
        }
        case V1FileStep::Kind::kRemoveBuildFolder: {
          const HRESULT real = CheckRealFolder(full);
          if (IsMissing(real)) {
            finish(Result::kUnchanged);
            break;
          }
          if (real == HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK)) {
            finish(Result::kKept, V1KeepReason::kLink);
            break;
          }
          if (FAILED(real)) {
            finish(Result::kFailed, V1KeepReason::kNone, real);
            break;
          }
          std::vector<std::filesystem::path> files;
          std::vector<Holder> holders;
          if (SUCCEEDED(TopLevelFiles(full, &files)) && !files.empty() &&
              SUCCEEDED(probes.FindHolders(files, &holders)) && !holders.empty()) {
            finish(Result::kKept, V1KeepReason::kHeld);
            break;
          }
          // No trash folder: this is the user's profile, and the user part never runs elevated.
          RemoveOutcome removal;
          const HRESULT hr = RemoveTreeOrTrash(full, {}, 0, &removal);
          if (FAILED(hr)) {
            finish(Result::kFailed, V1KeepReason::kNone, hr);
          } else if (!removal.removed) {
            finish(Result::kKept, V1KeepReason::kInUse);
          } else {
            finish(Result::kDone);
          }
          break;
        }
      }
    }
    const bool gone = outcome.result == Result::kDone || outcome.result == Result::kUnchanged;
    if (!gone && !step.marker &&
        (step.byContent || step.kind == V1FileStep::Kind::kRemoveEmptyFolder)) {
      groupIncomplete = true;
    }
    outcomes->push_back(std::move(outcome));
  }
  return S_OK;
}
CATCH_RETURN();

// ---- The user part ------------------------------------------------------------------------------

bool V1UserResult::Incomplete() const noexcept {
  return std::any_of(files.begin(), files.end(),
                     [](const V1FileOutcome& outcome) {
                       return outcome.result == V1FileOutcome::Result::kKept &&
                              (outcome.reason == V1KeepReason::kInUse ||
                               outcome.reason == V1KeepReason::kHeld ||
                               outcome.reason == V1KeepReason::kWaiting ||
                               outcome.reason == V1KeepReason::kNotEmpty);
                     }) ||
         std::any_of(plan.kept.begin(), plan.kept.end(),
                     [](const V1Kept& kept) { return kept.reason == V1KeepReason::kHeld; });
}

HRESULT MigrateV1User(const RegistryRoots& roots, const SystemProbes& probes,
                      const Snapshot& snapshot, const V1UserOptions& options,
                      V1UserResult* result) noexcept try {
  *result = {};
  RETURN_HR_IF(E_ACCESSDENIED, snapshot.context.elevated);
  const EnginePaths& paths = snapshot.paths;
  const auto failed = [&](const wchar_t* what, HRESULT hr) {
    result->failures.push_back(std::wstring(what) + L": " + HResultText(hr));
  };

  // 1. The sign-in clean-up first: 1.x's would delete the per-user folder.
  if (const HRESULT hr = DeleteRunOnceRemovals(roots, &result->runOnceDeleted); FAILED(hr)) {
    failed(L"runonce", hr);
  }

  // 2. The zoom, only when the registry holds none.
  if (!options.keepZoom) {
    std::optional<RegistryValue> zoom;
    HRESULT hr = ReadRegistryValue(roots.user, kRegKey, kZoomValueName, &zoom);
    std::optional<int> iniZoom;
    if (SUCCEEDED(hr) && !zoom && !paths.legacyIni.empty()) {
      hr = ReadLegacyIniZoom(paths.legacyIni, &iniZoom);
    }
    if (SUCCEEDED(hr) && !zoom && iniZoom) {
      hr = SaveZoom(roots, *iniZoom);
      if (SUCCEEDED(hr)) result->zoomMigrated = ClampZoom(*iniZoom);
    }
    if (FAILED(hr)) failed(L"zoom", hr);
  }

  // 3. This version's task, then 1.x's goes.
  bool legacyTask = false;
  if (const HRESULT hr = probes.LegacyTaskExists(&legacyTask); FAILED(hr)) {
    failed(L"legacy-task", hr);
    legacyTask = false;
  }
  if (legacyTask) {
    bool mayDelete = !options.registerTask;
    if (options.registerTask) {
      const std::optional<std::filesystem::path> program = InstalledSettingsExe(snapshot);
      if (program && !snapshot.taskLocation.name.empty()) {
        TaskState now;
        HRESULT hr = probes.ReadTask(snapshot.taskLocation, &now);
        if (SUCCEEDED(hr) && now.exists && !HasNoTriggers(now.options)) {
          mayDelete = true;  // the user's own task is there already, as they set it
        } else {
          hr = probes.ApplyTaskOptions(snapshot.taskLocation, TaskOptions{}, *program,
                                       snapshot.context);
          result->taskRegistered = SUCCEEDED(hr);
          if (hr == kTaskTakenHr) {
            // Only an administrator can free the names: the line says so, and which tasks.
            result->failures.push_back(L"task: " + TaskTakenText() + L" (" + HResultText(hr) +
                                       L"). " + TaskTakenHint(snapshot.taskLocation));
          } else if (FAILED(hr)) {
            failed(L"task", hr);
          }
          // 1.x's task goes either way: its script is about to go too, and it must not repair
          // against this version.
          mayDelete = true;
        }
      }
    }
    if (mayDelete) {
      const HRESULT hr = probes.DeleteLegacyTask();
      result->legacyTaskDeleted = hr == S_OK;
      if (FAILED(hr)) failed(L"delete-legacy-task", hr);
    }
  }

  // 4. A per-user class in a 1.x build folder, before any file goes.
  std::optional<ClassReg> userClass;
  std::optional<ClassReg> machineClass;
  HRESULT hr = ReadClass(roots, Scope::kUser, &userClass);
  if (SUCCEEDED(hr)) hr = ReadClass(roots, Scope::kMachine, &machineClass);
  if (FAILED(hr)) {
    failed(L"class", hr);
  } else if (userClass && (IsInV1BuildFolder(userClass->dll, paths.legacyUserRoot) ||
                           IsInV1BuildFolder(userClass->dll, paths.legacyMachineRoot))) {
    if (machineClass) {
      hr = DeleteClass(roots, Scope::kUser);
      result->userClass = V1UserResult::ClassChange::kDeleted;
    } else if (const std::optional<std::filesystem::path> target = ChooseUserBuildDll(snapshot)) {
      hr = WriteClass(roots, Scope::kUser, *target);
      result->userClass = V1UserResult::ClassChange::kRepointed;
    } else {
      // Nothing else would load: the 1.x class stays until this version is installed.
      result->userClass = V1UserResult::ClassChange::kKept;
    }
    if (FAILED(hr)) {
      failed(L"class", hr);
      result->userClass = V1UserResult::ClassChange::kKept;
    }
  }

  // 5. The files.
  if (options.files) {
    if (const HRESULT files = RunFilePart(roots, probes, snapshot, options.endHolders,
                                          &result->plan, &result->files, &result->holdersEnded);
        FAILED(files)) {
      failed(L"files", files);
    }
    for (const V1FileOutcome& outcome : result->files) {
      if (outcome.result == V1FileOutcome::Result::kFailed) failed(L"file", outcome.hr);
    }
  }

  // 6. Done for this user (an incomplete file part is finished by later checks).
  if (result->failures.empty()) {
    if (const HRESULT marked = WriteMigratedFromV1(roots); FAILED(marked)) {
      failed(L"mark", marked);
    } else {
      result->marked = true;
    }
  }
  if (result->userClass == V1UserResult::ClassChange::kDeleted ||
      result->userClass == V1UserResult::ClassChange::kRepointed) {
    NotifyAssocChanged(roots);
  }
  return S_OK;
}
CATCH_RETURN();

HRESULT RemoveV1UserFiles(const RegistryRoots& roots, const SystemProbes& probes,
                          const Snapshot& snapshot, V1FilePlan* plan,
                          std::vector<V1FileOutcome>* outcomes) noexcept try {
  *plan = {};
  outcomes->clear();
  RETURN_HR_IF(E_ACCESSDENIED, snapshot.context.elevated);
  return RunFilePart(roots, probes, snapshot, false, plan, outcomes, nullptr);
}
CATCH_RETURN();

// ---- Build folders for all users ----------------------------------------------------------------

MachineBuildPlan PlanMachineV1Removal(std::span<const FolderEntry> app,
                                      const std::filesystem::path& root,
                                      const std::optional<std::wstring>& classDll) {
  MachineBuildPlan plan;
  if (classDll) plan.keep = BuildFolderOf(*classDll, root);
  for (const FolderEntry& entry : app) {
    if (!IsV1BuildFolderName(entry.name)) continue;
    if (entry.reparsePoint) {
      plan.links.push_back(entry.name);
    } else if (entry.directory && !(plan.keep && IEquals(entry.name, *plan.keep))) {
      plan.remove.push_back(entry.name);
    }
  }
  return plan;
}

MachineBuildPlan PlanOldMachineBuilds(std::span<const FolderEntry> app,
                                      const std::filesystem::path& root,
                                      const std::optional<std::wstring>& classDll,
                                      bool classBuildComplete) {
  MachineBuildPlan plan;
  if (classDll) plan.keep = BuildFolderOf(*classDll, root);
  const auto is2x = [](std::wstring_view name) {
    const std::optional<BuildFolderNameParts> parts = ParseBuildFolderName(name);
    return parts && !IsV1Version(parts->version);
  };
  for (const FolderEntry& entry : app) {
    if (entry.reparsePoint && is2x(entry.name)) plan.links.push_back(entry.name);
  }
  if (!plan.keep || !is2x(*plan.keep) || !classBuildComplete) return plan;
  for (const FolderEntry& entry : app) {
    if (entry.reparsePoint || !entry.directory || !is2x(entry.name)) continue;
    if (IEquals(entry.name, *plan.keep)) continue;
    plan.remove.push_back(entry.name);
  }
  return plan;
}

HRESULT RemoveMachineBuilds(const std::filesystem::path& root, const MachineBuildPlan& plan,
                            const std::filesystem::path& trashParent, DWORD retryMs,
                            MachineRemoval* result) noexcept try {
  if (plan.remove.empty()) return S_OK;
  const std::filesystem::path app = root / kBuildsFolderName;
  RETURN_IF_FAILED(CheckNoReparsePoints(app, trashParent.empty() ? root : trashParent));
  RETURN_IF_FAILED(CheckRealFolder(app));
  for (const std::wstring& name : plan.remove) {
    RemoveOutcome removal;
    const HRESULT hr = RemoveTreeOrTrash(app / name, trashParent, retryMs, &removal);
    if (FAILED(hr)) {
      result->failed.push_back(name);
      if (SUCCEEDED(result->firstError)) result->firstError = hr;
    } else if (!removal.removed) {
      result->left.push_back(name);
    } else {
      result->removed.push_back(name);
    }
    if (removal.restartPending && removal.removed) result->restartPending = true;
  }
  return S_OK;
}
CATCH_RETURN();

namespace {

// The machine-wide class's DLL as roots.machine stores it, and whether that build is complete.
// A path stored to be expanded (REG_EXPAND_SZ) is never expanded here: these steps run elevated,
// with the user's environment, which the user's own programs can change. Such a class is foreign
// (*foreign): which build it loads is not known, so no build folder may go.
HRESULT MachineClassDll(const RegistryRoots& roots, std::optional<std::wstring>* dll,
                        bool* complete, bool* foreign) {
  dll->reset();
  *complete = false;
  *foreign = false;
  std::optional<ClassReg> reg;
  RETURN_IF_FAILED(ReadClass(roots, Scope::kMachine, &reg, ClassPathExpansion::kNever));
  if (reg && reg->dllExpandable) {
    *foreign = true;
  } else if (reg && !reg->dll.empty()) {
    *dll = reg->dll;
    *complete = reg->dllExists && reg->pdfiumExists;
  }
  return S_OK;
}

}  // namespace

HRESULT MigrateV1Machine(const RegistryRoots& roots, const std::filesystem::path& legacyMachineRoot,
                         const std::filesystem::path& trashParent, MachineRemoval* result) noexcept
    try {
  *result = {};
  std::optional<std::wstring> dll;
  bool complete = false;
  RETURN_IF_FAILED(MachineClassDll(roots, &dll, &complete, &result->classForeign));
  if (result->classForeign) return S_OK;  // every 1.x build stays
  result->classInV1 = dll && IsInV1BuildFolder(*dll, legacyMachineRoot);
  std::vector<FolderEntry> app;
  if (SUCCEEDED(CheckRealFolder(legacyMachineRoot))) {
    RETURN_IF_FAILED(ListFolderEntries(legacyMachineRoot / kBuildsFolderName, &app));
  }
  const MachineBuildPlan plan = PlanMachineV1Removal(app, legacyMachineRoot, dll);
  return RemoveMachineBuilds(legacyMachineRoot, plan, trashParent, kMachineRemoveRetryMs, result);
}
CATCH_RETURN();

HRESULT RemoveOldMachineBuilds(const RegistryRoots& roots, const std::filesystem::path& machineRoot,
                               const std::filesystem::path& trashParent,
                               MachineRemoval* result) noexcept try {
  *result = {};
  std::optional<std::wstring> dll;
  bool complete = false;
  RETURN_IF_FAILED(MachineClassDll(roots, &dll, &complete, &result->classForeign));
  if (result->classForeign) return S_OK;  // every build stays
  std::vector<FolderEntry> app;
  if (SUCCEEDED(CheckRealFolder(machineRoot))) {
    RETURN_IF_FAILED(ListFolderEntries(machineRoot / kBuildsFolderName, &app));
  }
  const MachineBuildPlan plan = PlanOldMachineBuilds(app, machineRoot, dll, complete);
  return RemoveMachineBuilds(machineRoot, plan, trashParent, kMachineRemoveRetryMs, result);
}
CATCH_RETURN();

HRESULT MachineV1RemovalNeeded(const RegistryRoots& roots,
                               const std::filesystem::path& legacyMachineRoot,
                               bool* needed) noexcept try {
  *needed = false;
  std::optional<std::wstring> dll;
  bool complete = false;
  bool foreign = false;
  RETURN_IF_FAILED(MachineClassDll(roots, &dll, &complete, &foreign));
  if (foreign || FAILED(CheckRealFolder(legacyMachineRoot))) return S_OK;
  std::vector<FolderEntry> app;
  RETURN_IF_FAILED(ListFolderEntries(legacyMachineRoot / kBuildsFolderName, &app));
  *needed = !PlanMachineV1Removal(app, legacyMachineRoot, dll).remove.empty();
  return S_OK;
}
CATCH_RETURN();

bool PerUserInstallBlockedByV1Machine(const LegacyState& legacy) noexcept {
  return legacy.machineClassInV1;
}

}  // namespace pdft
