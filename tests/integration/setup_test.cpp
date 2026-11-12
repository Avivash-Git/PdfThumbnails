// The 1.x clean-up and the installer's steps against the real file system and registry API, but
// only in private places: install roots in a folder under %TEMP%, the registry through a private
// key (ScopedRegistrySandbox, RunOnce included), and every Windows-wide question and write (the
// process context, the tasks, the processes holding files) through the fake probes, which record
// instead of changing anything. The administrator step is started through a fake launcher only,
// never for real. The installed product, the real per-user folder and the real tasks are never
// read or touched.
//
// Files with 1.x's contents come from the source tree (1.x's LICENSE, launcher and manager are
// kept unchanged in tests\system\fixture), found next to the build-id script the build tree
// names; the test bundle has no source tree and leaves those parts out.

#include <windows.h>

#include "setup.h"

#include <gtest/gtest.h>
#include <wil/resource.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "build_folder.h"
#include "child_process.h"
#include "elevation.h"
#include "fake_probes.h"
#include "file_ops.h"
#include "instance.h"
#include "legacy.h"
#include "migrate_v1.h"
#include "project_config.h"
#include "registration.h"
#include "registry_sandbox.h"
#include "settings.h"
#include "status_format.h"
#include "strings.h"
#include "temp_dir.h"
#include "zoom.h"

namespace pdft {
namespace {

using std::filesystem::path;
using test::FakeProbes;

bool WriteFileText(const path& file, const std::string& text) {
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
  DWORD written = 0;
  return handle &&
         WriteFile(handle.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
         written == text.size();
}

bool Exists(const path& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool MakeDirs(const path& p) {
  std::error_code error;
  std::filesystem::create_directories(p, error);
  return !error;
}

bool Elevated() {
  bool elevated = false;
  return SUCCEEDED(GetTokenElevation(&elevated)) && elevated;
}

std::wstring BuiltFile(const wchar_t* name) { return test::SelfFolder() + L"\\" + name; }

// The source tree, from the build-id script's path the build tree passes; empty in the bundle.
path SourceTree() {
  wchar_t* value = nullptr;
  size_t size = 0;
  if (_wdupenv_s(&value, &size, L"PDFT_TEST_BUILDID_SCRIPT") != 0 || !value) return {};
  const path script = value;
  free(value);
  return script.parent_path().parent_path();
}

// A file of the source tree whose contents the table of 1.x files has under |relative|, when
// there is one: at the top, or among the system tests' 1.x files, which are kept as
// "<name>.txt".
std::optional<path> V1FileFromSource(const std::wstring& relative) {
  if (SourceTree().empty()) return std::nullopt;
  // The fixture copy first: a file of the same name in the root may have changed since 1.x.
  for (const path& file : {SourceTree() / L"tests" / L"system" / L"fixture" / (relative + L".txt"),
                           SourceTree() / relative}) {
    Sha256Digest digest = {};
    if (!Exists(file) || FAILED(Sha256File(file, &digest))) continue;
    for (const Sha256Digest& known : V1ShippedDigests(relative)) {
      if (known == digest) return file;
    }
  }
  return std::nullopt;
}

// ---- Deleting by contents ---------------------------------------------------------------------

TEST(ContentDelete, OnlyTheExpectedContentsGo) {
  test::TempDir dir(L"content");
  ASSERT_TRUE(dir.ok());
  const path file = path(dir.path()) / L"LICENSE";
  ASSERT_TRUE(WriteFileText(file, "1.x license"));
  Sha256Digest digest = {};
  ASSERT_HRESULT_SUCCEEDED(Sha256File(file, &digest));
  Sha256Digest other = digest;
  other[0] ^= 0xFF;

  ContentDelete result = ContentDelete::kDeleted;
  ASSERT_EQ(DeleteFileIfContent(file, std::vector<Sha256Digest>{other}, &result), S_OK);
  EXPECT_EQ(result, ContentDelete::kMismatch);
  EXPECT_TRUE(Exists(file));
  ASSERT_EQ(DeleteFileIfContent(file, std::vector<Sha256Digest>{other, digest}, &result), S_OK);
  EXPECT_EQ(result, ContentDelete::kDeleted);
  EXPECT_FALSE(Exists(file));
  ASSERT_EQ(DeleteFileIfContent(file, std::vector<Sha256Digest>{digest}, &result), S_OK);
  EXPECT_EQ(result, ContentDelete::kMissing);

  // No digests: any contents; a read-only file too.
  ASSERT_TRUE(WriteFileText(file, "settings"));
  ASSERT_TRUE(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY));
  ASSERT_EQ(DeleteFileIfContent(file, {}, &result), S_OK);
  EXPECT_EQ(result, ContentDelete::kDeleted);
  EXPECT_FALSE(Exists(file));
}

TEST(ContentDelete, FoldersLinksAndFilesInUseStay) {
  test::TempDir dir(L"content");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(MakeDirs(root / L"folder"));
  ContentDelete result = ContentDelete::kDeleted;
  ASSERT_EQ(DeleteFileIfContent(root / L"folder", {}, &result), S_OK);
  EXPECT_EQ(result, ContentDelete::kNotAFile);
  EXPECT_TRUE(Exists(root / L"folder"));

  // A junction is not followed, and stays itself.
  ASSERT_TRUE(MakeDirs(root / L"target"));
  ASSERT_TRUE(WriteFileText(root / L"target" / L"kept.txt", "kept"));
  ASSERT_TRUE(MakeDirs(root / L"link"));
  ASSERT_TRUE(test::MakeJunction((root / L"link").native(), (root / L"target").native()));
  ASSERT_EQ(DeleteFileIfContent(root / L"link", {}, &result), S_OK);
  EXPECT_EQ(result, ContentDelete::kNotAFile);
  EXPECT_TRUE(Exists(root / L"target" / L"kept.txt"));

  // Someone has it open (and denies deleting): it stays.
  const path held = root / L"held.txt";
  ASSERT_TRUE(WriteFileText(held, "held"));
  {
    wil::unique_hfile user(CreateFileW(held.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(user);
    ASSERT_EQ(DeleteFileIfContent(held, {}, &result), S_OK);
    EXPECT_EQ(result, ContentDelete::kInUse);
    EXPECT_TRUE(Exists(held));
  }
  ASSERT_EQ(DeleteFileIfContent(held, {}, &result), S_OK);
  EXPECT_EQ(result, ContentDelete::kDeleted);
  EXPECT_EQ(DeleteFileIfContent(L"relative.txt", {}, &result), E_INVALIDARG);
}

// ---- The 1.x clean-up and the installer's steps ---------------------------------------------

class Setup : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sandbox_.ok()) << HResultText(sandbox_.error());
    ASSERT_TRUE(temp_.ok());
    roots_ = {sandbox_.user(), sandbox_.machine()};
    const path base = temp_.path();
    paths_.userRoot = base / L"user-root";
    paths_.machineRoot = base / L"machine-root";
    // As on a real PC: 1.x's folders are the same folders.
    paths_.legacyUserRoot = paths_.userRoot;
    paths_.legacyMachineRoot = paths_.machineRoot;
    paths_.legacyIni = paths_.userRoot / L"settings.ini";
    paths_.ownExe = test::SelfPath();
    ASSERT_TRUE(MakeDirs(paths_.userRoot));
    ASSERT_TRUE(MakeDirs(paths_.machineRoot));
  }

  // A build folder <root>\app\<name> with copies of the built DLLs (for 1.x names, stand-ins).
  path MakeBuild(const path& root, const std::wstring& name) {
    const path dir = root / L"app" / name;
    EXPECT_TRUE(MakeDirs(dir));
    if (name.starts_with(L"1.")) {
      EXPECT_TRUE(WriteFileText(dir / kHandlerDll, "1.x"));
      EXPECT_TRUE(WriteFileText(dir / (std::wstring(kLegacyRootName) + L"-pdfium.dll"), "1.x"));
    } else {
      EXPECT_TRUE(CopyFileW(BuiltFile(kHandlerDll).c_str(), (dir / kHandlerDll).c_str(), TRUE));
      EXPECT_TRUE(CopyFileW(BuiltFile(kPdfiumDll).c_str(), (dir / kPdfiumDll).c_str(), TRUE));
    }
    return dir;
  }

  void RegisterMachineClass(const path& dll) {
    const std::wstring inproc = ClassKeyPath() + L"\\InprocServer32";
    ASSERT_HRESULT_SUCCEEDED(
        WriteRegistryString(sandbox_.machine(), ClassKeyPath(), L"", kHandlerClassName));
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.machine(), inproc, L"", dll.native()));
    ASSERT_HRESULT_SUCCEEDED(
        WriteRegistryString(sandbox_.machine(), inproc, L"ThreadingModel", L"Apartment"));
  }

  // A machine-wide install of this version: its build, the class and the settings executable (an
  // empty file, never started).
  path InstallForAllUsers() {
    const path build = MakeBuild(paths_.machineRoot, L"2.0.0.0-1a2b3c4d");
    RegisterMachineClass(build / kHandlerDll);
    EXPECT_TRUE(WriteFileText(paths_.machineRoot / kSettingsExe, ""));
    EXPECT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
    return build;
  }

  // 1.x's per-user folder as 1.1 left it: its settings (Zoom=|zoom|), its log, its build, and
  // what it shipped. Files with 1.x's real contents come from the source tree where it has them;
  // the rest are stand-ins with other contents, as a later product's files of the same names
  // would be.
  void Plant11UserFolder(int zoom = 120) {
    const path root = paths_.userRoot;
    ASSERT_TRUE(WriteFileText(root / L"settings.ini",
                              "[Thumbnails]\r\nZoom=" + std::to_string(zoom) + "\r\n"));
    ASSERT_TRUE(WriteFileText(root / L"repair.log", "repaired\r\n"));
    ASSERT_TRUE(WriteFileText(root / L"README.md", "not 1.x's README\n"));
    ASSERT_TRUE(MakeDirs(root / L"licenses"));
    ASSERT_TRUE(WriteFileText(root / L"licenses" / L"zlib.txt", "not 1.x's zlib notice\n"));
    ASSERT_TRUE(MakeDirs(root / L"bin"));
    ASSERT_TRUE(WriteFileText(root / L"bin" / kHandlerDll, "not 1.x's DLL"));
    MakeBuild(root, L"1.1.0.0-67a1323e");
    shippedFromSource_.clear();
    for (const std::wstring& name :
         {std::wstring(L"LICENSE"), std::wstring(kLegacyRootName) + L".cmd",
          std::wstring(kLegacyRootName) + L".ps1"}) {
      if (const std::optional<path> source = V1FileFromSource(name)) {
        ASSERT_TRUE(CopyFileW(source->c_str(), (root / name).c_str(), FALSE));
        shippedFromSource_.push_back(name);
      }
    }
    if (shippedFromSource_.empty()) {
      // No source tree (the test bundle): the launcher as a stand-in still marks the folder 1.x's.
      ASSERT_TRUE(WriteFileText(root / (std::wstring(kLegacyRootName) + L".cmd"), "@echo off\r\n"));
    }
  }

  void PlantRunOnce() {
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.user(), kRunOnceKey, kLegacyRunOnce,
                                                 L"cmd.exe /d /c rd /s /q \"x\""));
  }

  Snapshot Gather() {
    GatherOptions options;
    options.liveTest = false;
    options.context = probes_.context;
    options.paths = paths_;
    Snapshot snapshot;
    EXPECT_HRESULT_SUCCEEDED(GatherSnapshot(roots_, options, probes_, &snapshot));
    return snapshot;
  }

  std::optional<DWORD> StoredZoom() {
    std::optional<RegistryValue> value;
    if (FAILED(ReadRegistryValue(sandbox_.user(), kRegKey, kZoomValueName, &value)) || !value) {
      return std::nullopt;
    }
    return RegistryDword(*value);
  }

  bool HasValue(const wchar_t* name) {
    std::optional<RegistryValue> value;
    return SUCCEEDED(ReadRegistryValue(sandbox_.user(), kRunOnceKey, name, &value)) &&
           value.has_value();
  }

  test::ScopedRegistrySandbox sandbox_;
  test::TempDir temp_{L"setup"};
  RegistryRoots roots_;
  EnginePaths paths_;
  FakeProbes probes_;
  std::vector<std::wstring> shippedFromSource_;
};

TEST_F(Setup, MigrateV1UserOnThe11Shape) {
  InstallForAllUsers();
  Plant11UserFolder(120);
  PlantRunOnce();
  probes_.legacyTask = true;
  // A per-user class in 1.x's per-user build folder.
  ASSERT_HRESULT_SUCCEEDED(WriteClass(
      roots_, Scope::kUser, paths_.userRoot / L"app" / L"1.1.0.0-67a1323e" / kHandlerDll));

  const Snapshot snapshot = Gather();
  V1UserResult result;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1User(roots_, probes_, snapshot, V1UserOptions{}, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);

  EXPECT_EQ(result.runOnceDeleted, 1u);
  EXPECT_FALSE(HasValue(kLegacyRunOnce));
  EXPECT_EQ(result.zoomMigrated, 120);
  EXPECT_EQ(StoredZoom(), 120u);
  // This version's task first, with both triggers and the installed program, then 1.x's.
  ASSERT_EQ(probes_.applied.size(), 1u);
  EXPECT_EQ(probes_.applied[0].options.afterPdfCraft, TriggerSetting::kEnabled);
  EXPECT_EQ(probes_.applied[0].options.atSignIn, TriggerSetting::kEnabled);
  EXPECT_TRUE(
      IEquals(probes_.applied[0].program.native(), (paths_.machineRoot / kSettingsExe).native()));
  EXPECT_TRUE(result.taskRegistered);
  EXPECT_TRUE(result.legacyTaskDeleted);
  EXPECT_FALSE(probes_.legacyTask);
  // The per-user 1.x class went, so the machine-wide class applies.
  EXPECT_EQ(result.userClass, V1UserResult::ClassChange::kDeleted);
  std::optional<ClassReg> userClass;
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &userClass));
  EXPECT_FALSE(userClass);

  const path root = paths_.userRoot;
  EXPECT_FALSE(Exists(root / L"settings.ini"));
  EXPECT_FALSE(Exists(root / L"repair.log"));
  EXPECT_FALSE(Exists(root / L"app" / L"1.1.0.0-67a1323e"));
  EXPECT_TRUE(Exists(root / L"app"));  // a name 2.x uses too
  for (const std::wstring& name : shippedFromSource_) {
    EXPECT_FALSE(Exists(root / name)) << WideToUtf8Lossy(name);
  }
  // Everything that is not 1.x's own contents stays.
  EXPECT_TRUE(Exists(root / L"README.md"));
  EXPECT_TRUE(Exists(root / L"licenses" / L"zlib.txt"));
  EXPECT_TRUE(Exists(root / L"bin" / kHandlerDll));
  bool migrated = false;
  ASSERT_HRESULT_SUCCEEDED(ReadMigratedFromV1(roots_, &migrated));
  EXPECT_TRUE(migrated);
  EXPECT_TRUE(result.marked);
  EXPECT_FALSE(result.Incomplete());
  std::printf("[ migrate ] 1.x files with real contents from the source tree: %zu\n",
              shippedFromSource_.size());

  // Again: nothing more to do, and the user's task is left as it is.
  const Snapshot again = Gather();
  V1UserResult second;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1User(roots_, probes_, again, V1UserOptions{}, &second));
  EXPECT_TRUE(second.failures.empty());
  EXPECT_EQ(second.runOnceDeleted, 0u);
  EXPECT_FALSE(second.zoomMigrated);
  EXPECT_EQ(probes_.applied.size(), 1u);
  EXPECT_EQ(second.userClass, V1UserResult::ClassChange::kNone);
  for (const V1FileOutcome& outcome : second.files) {
    EXPECT_NE(outcome.result, V1FileOutcome::Result::kDone) << WideToUtf8Lossy(outcome.step.path);
  }
  EXPECT_TRUE(Exists(root / L"README.md"));
}

TEST_F(Setup, AZoomAlreadySetStays) {
  InstallForAllUsers();
  Plant11UserFolder(150);
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 110));
  V1UserResult result;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1User(roots_, probes_, Gather(), V1UserOptions{}, &result));
  EXPECT_FALSE(result.zoomMigrated);
  EXPECT_EQ(StoredZoom(), 110u);
  EXPECT_FALSE(Exists(paths_.userRoot / L"settings.ini"));  // the registry holds a zoom
}

TEST_F(Setup, AHeldBuildFolderStaysUntilItsHolderEnds) {
  InstallForAllUsers();
  Plant11UserFolder();
  const path build = paths_.userRoot / L"app" / L"1.1.0.0-67a1323e";
  Holder holder;
  holder.pid = 4242;
  probes_.holders.push_back({build / kHandlerDll, holder});
  probes_.endWorks = false;

  V1UserResult result;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1User(roots_, probes_, Gather(), V1UserOptions{}, &result));
  EXPECT_EQ(probes_.ended, std::vector<DWORD>{4242});
  EXPECT_TRUE(Exists(build));
  EXPECT_TRUE(result.Incomplete());
  bool kept = false;
  for (const V1Kept& item : result.plan.kept) {
    if (item.path == L"app\\1.1.0.0-67a1323e" && item.reason == V1KeepReason::kHeld) kept = true;
  }
  EXPECT_TRUE(kept);
  // The check never ends a process; once the holder is gone, it finishes the clean-up.
  probes_.holders.clear();
  probes_.ended.clear();
  const Snapshot later = Gather();
  EXPECT_TRUE(later.migratedFromV1);
  V1FilePlan plan;
  std::vector<V1FileOutcome> outcomes;
  ASSERT_HRESULT_SUCCEEDED(RemoveV1UserFiles(roots_, probes_, later, &plan, &outcomes));
  EXPECT_TRUE(probes_.ended.empty());
  EXPECT_FALSE(Exists(build));

  // Ending works: the folder goes in the same run.
  MakeBuild(paths_.userRoot, L"1.1.0.0-67a1323e");
  probes_.holders.push_back({build / kHandlerDll, holder});
  probes_.endWorks = true;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1User(roots_, probes_, Gather(), V1UserOptions{}, &result));
  EXPECT_EQ(result.holdersEnded, 1u);
  EXPECT_FALSE(Exists(build));
}

// A file 1.x shipped that is in use stays, and so does the manager, so the next run (here the
// check's) finishes the job once it is free.
TEST_F(Setup, TheManagerWaitsForAFileInUse) {
  InstallForAllUsers();
  Plant11UserFolder();
  if (shippedFromSource_.size() < 2) {
    GTEST_SKIP() << "needs 1.x's LICENSE and manager from the source tree (not in the bundle)";
  }
  const path license = paths_.userRoot / L"LICENSE";
  {
    wil::unique_hfile user(CreateFileW(license.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(user);
    V1UserResult result;
    ASSERT_HRESULT_SUCCEEDED(MigrateV1User(roots_, probes_, Gather(), V1UserOptions{}, &result));
    EXPECT_TRUE(Exists(license));
    for (const std::wstring& name : shippedFromSource_) {
      if (name != L"LICENSE") EXPECT_TRUE(Exists(paths_.userRoot / name)) << WideToUtf8Lossy(name);
    }
    EXPECT_TRUE(result.Incomplete());
  }
  // The check's repair, through the whole check: the leftovers are fixed by --check now.
  const Snapshot later = Gather();
  const std::vector<Finding> findings = Evaluate(later);
  const RepairPlan plan = PlanRepairs(later, findings, RepairMode::kSafe);
  ASSERT_EQ(plan.user.size(), 1u);
  EXPECT_EQ(plan.user[0].kind, RepairStep::Kind::kRemoveV1Files);
  std::vector<RepairOutcome> outcomes;
  ASSERT_HRESULT_SUCCEEDED(ApplyUserRepairs(roots_, probes_, later, plan, &outcomes));
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes[0].result, RepairResult::kDone);
  for (const std::wstring& name : shippedFromSource_) {
    EXPECT_FALSE(Exists(paths_.userRoot / name)) << WideToUtf8Lossy(name);
  }
}

TEST_F(Setup, MigrationNeedsThisVersionInstalled) {
  Plant11UserFolder();
  PlantRunOnce();
  probes_.legacyTask = true;
  MigrateOptions options;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 5000;
  int launches = 0;
  options.launcher = [&](const ElevationRequest&, DWORD*, DWORD*) {
    ++launches;
    return S_OK;
  };
  MigrateResult result;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kNotInstalled);
  // 1.x stays usable: nothing changed.
  EXPECT_TRUE(HasValue(kLegacyRunOnce));
  EXPECT_TRUE(probes_.legacyTask);
  EXPECT_TRUE(Exists(paths_.userRoot / L"settings.ini"));
  EXPECT_EQ(launches, 0);

  // Elevated, the per-user steps refuse before anything is read.
  InstallForAllUsers();
  options.context->elevated = true;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  EXPECT_EQ(result.refusal, SetupRefusal::kElevated);
  EXPECT_TRUE(HasValue(kLegacyRunOnce));
  // Where the registry may be virtualized as well.
  options.context->elevated = false;
  options.context->virtualization = Virtualization::kYes;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, SetupRefusal::kWrongContext);
  EXPECT_TRUE(HasValue(kLegacyRunOnce));
}

TEST_F(Setup, MigrateV1AsksForTheMachinePartOnlyWhenNeeded) {
  InstallForAllUsers();
  Plant11UserFolder();
  MigrateOptions options;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 5000;
  std::vector<ElevationRequest> requests;
  DWORD exitCode = 0;
  HRESULT launch = S_OK;
  options.launcher = [&](const ElevationRequest& request, DWORD* processId, DWORD* code) {
    requests.push_back(request);
    *processId = 0;
    *code = exitCode;
    return launch;
  };
  MigrateResult result;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_FALSE(result.machineNeeded);
  EXPECT_TRUE(requests.empty());

  // A 1.x build folder for all users that no class uses.
  MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_TRUE(result.machineNeeded);
  ASSERT_EQ(requests.size(), 1u);
  EXPECT_TRUE(IEquals(requests[0].exe.native(), (paths_.machineRoot / kSettingsExe).native()));
  EXPECT_EQ(requests[0].parameters, L"--elevated machine-migrate-v1");
  EXPECT_EQ(result.code, ExitCode::kOk);
  // Done, but a file waits for the restart.
  exitCode = 1;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kProblems);
  // The prompt declined.
  launch = HRESULT_FROM_WIN32(ERROR_CANCELLED);
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kCancelled);
  // The step failed.
  launch = S_OK;
  exitCode = static_cast<DWORD>(E_ACCESSDENIED);
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kFailed);
  // Started without administrator rights, the step refused: exit 7, not listed as a failure.
  exitCode = static_cast<DWORD>(kNoAdministratorRightsHr);
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kNeedsAdmin);
  ASSERT_TRUE(result.run);
  EXPECT_TRUE(LackedAdministratorRights(*result.run));
  EXPECT_TRUE(result.failures.empty());
  // Refused below an app package: exit 8, not listed as a failure either.
  exitCode = static_cast<DWORD>(kBelowPackageHr);
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  ASSERT_TRUE(result.run);
  EXPECT_TRUE(RefusedBelowPackage(*result.run));
  EXPECT_TRUE(result.failures.empty());
  // --user-only never asks.
  requests.clear();
  options.scope = MigrateScope::kUserOnly;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_TRUE(requests.empty());
  // Without an install for all users there is nobody to ask: left for later.
  options.scope = MigrateScope::kMachineOnly;
  ASSERT_TRUE(DeleteFileW((paths_.machineRoot / kSettingsExe).c_str()));
  ASSERT_TRUE(WriteFileText(paths_.userRoot / kSettingsExe, ""));
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryTree(sandbox_.machine(), ClassKeyPath()));
  const path userBuild = MakeBuild(paths_.userRoot, L"2.0.0.0-1a2b3c4d");
  ASSERT_HRESULT_SUCCEEDED(WriteClass(roots_, Scope::kUser, userBuild / kHandlerDll));
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_TRUE(result.machineNoInstall);
  EXPECT_TRUE(requests.empty());
  EXPECT_EQ(result.code, ExitCode::kProblems);
}

// The step for all users failed after naming the part it went on after, and ended with that
// part's HRESULT: the part is the failure, in place of that HRESULT alone. An older build's
// report names none and leaves that one entry as it was. The step's report comes back through
// the real pipe from this test program's child.
TEST_F(Setup, MigrateV1ListsThePartsOfItsStepForAllUsers) {
  InstallForAllUsers();
  MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  MigrateOptions options;
  options.scope = MigrateScope::kMachineOnly;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 5000;
  std::wstring arguments =
      L"report-step machine-migrate-v1 0x80070020 0 \"migrate-v1: 0x80070020\"";
  options.launcher = [&](const ElevationRequest&, DWORD* processId, DWORD* exitCode) {
    return test::RunSelfChild(arguments, processId, exitCode);
  };
  MigrateResult result;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  EXPECT_TRUE(result.machineNeeded);
  ASSERT_TRUE(result.run);
  EXPECT_EQ(result.run->outcome, ElevatedOutcome::kFailed);
  EXPECT_EQ(result.failures, std::vector<std::wstring>{L"machine migrate-v1: 0x80070020"});
  EXPECT_EQ(result.code, ExitCode::kFailed);

  arguments =
      L"report 0x80070020 \"failed 1.1.0.0-67a1323e\" \"elevated machine-migrate-v1 end "
      L"hr=0x80070020\"";
  ASSERT_HRESULT_SUCCEEDED(MigrateV1(options, roots_, probes_, &result));
  ASSERT_TRUE(result.run);
  EXPECT_TRUE(result.run->parts.ended);
  EXPECT_FALSE(result.run->parts.counted);
  EXPECT_EQ(result.failures, std::vector<std::wstring>{L"machine: 0x80070020"});
  EXPECT_EQ(result.code, ExitCode::kFailed);
}

TEST_F(Setup, MachinePartRemoves1xFoldersButTheClasses) {
  const path used = MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  const path unused = MakeBuild(paths_.machineRoot, L"1.0.0.0-0badf00d");
  const path current = MakeBuild(paths_.machineRoot, L"2.0.0.0-1a2b3c4d");
  RegisterMachineClass(used / kHandlerDll);
  bool needed = false;
  ASSERT_HRESULT_SUCCEEDED(MachineV1RemovalNeeded(roots_, paths_.machineRoot, &needed));
  EXPECT_TRUE(needed);
  MachineRemoval removal;
  ASSERT_HRESULT_SUCCEEDED(
      MigrateV1Machine(roots_, paths_.machineRoot, path(temp_.path()), &removal));
  EXPECT_EQ(removal.removed, std::vector<std::wstring>{L"1.0.0.0-0badf00d"});
  EXPECT_TRUE(removal.classInV1);
  EXPECT_FALSE(removal.restartPending);
  EXPECT_TRUE(Exists(used));
  EXPECT_FALSE(Exists(unused));
  EXPECT_TRUE(Exists(current));
  ASSERT_HRESULT_SUCCEEDED(MachineV1RemovalNeeded(roots_, paths_.machineRoot, &needed));
  EXPECT_FALSE(needed);

  // The class on this version: the old 1.x folder goes now, and old 2.x builds beside it.
  RegisterMachineClass(current / kHandlerDll);
  const path old = MakeBuild(paths_.machineRoot, L"2.0.0.0-00000001");
  ASSERT_HRESULT_SUCCEEDED(
      MigrateV1Machine(roots_, paths_.machineRoot, path(temp_.path()), &removal));
  EXPECT_FALSE(Exists(used));
  ASSERT_HRESULT_SUCCEEDED(
      RemoveOldMachineBuilds(roots_, paths_.machineRoot, path(temp_.path()), &removal));
  EXPECT_EQ(removal.removed, std::vector<std::wstring>{L"2.0.0.0-00000001"});
  EXPECT_FALSE(Exists(old));
  EXPECT_TRUE(Exists(current));
}

TEST_F(Setup, MachinePartLeavesAFolderInUseUnelevated) {
  if (Elevated()) GTEST_SKIP() << "elevated: in-use files would go to the trash (system tests)";
  const path unused = MakeBuild(paths_.machineRoot, L"1.0.0.0-0badf00d");
  wil::unique_hfile user(CreateFileW((unused / kHandlerDll).c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(user);
  MachineRemoval removal;
  ASSERT_HRESULT_SUCCEEDED(
      MigrateV1Machine(roots_, paths_.machineRoot, path(temp_.path()), &removal));
  EXPECT_EQ(removal.left, std::vector<std::wstring>{L"1.0.0.0-0badf00d"});
  EXPECT_FALSE(removal.restartPending);
  EXPECT_TRUE(Exists(unused / kHandlerDll));
}

TEST_F(Setup, MachinePartRefusesAJunctionedAppFolder) {
  const path elsewhere = path(temp_.path()) / L"elsewhere";
  ASSERT_TRUE(MakeDirs(elsewhere / L"1.0.0.0-0badf00d"));
  ASSERT_TRUE(MakeDirs(paths_.machineRoot / L"app"));
  ASSERT_TRUE(test::MakeJunction((paths_.machineRoot / L"app").native(), elsewhere.native()));
  MachineRemoval removal;
  EXPECT_EQ(MigrateV1Machine(roots_, paths_.machineRoot, path(temp_.path()), &removal),
            HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  EXPECT_TRUE(Exists(elsewhere / L"1.0.0.0-0badf00d"));
}

TEST_F(Setup, SetupUserSetsUpTheUser) {
  const path build = InstallForAllUsers();
  Plant11UserFolder(130);
  PlantRunOnce();
  ASSERT_HRESULT_SUCCEEDED(
      WriteRegistryString(sandbox_.user(), kRunOnceKey, RunOnceValueName(1), L"x"));
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.user(), kRunOnceKey, L"OtherApp", L"y"));
  probes_.legacyTask = true;
  // Another handler in one slot, and a per-user class that would hide the machine-wide one.
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.user(), SlotKeyPath(0), L"",
                                               L"{9C60DE1E-E5FC-40F4-A487-460851A8D915}"));
  const path userBuild = MakeBuild(paths_.userRoot, L"2.0.0.0-00000001");
  ASSERT_HRESULT_SUCCEEDED(WriteClass(roots_, Scope::kUser, userBuild / kHandlerDll));
  // A thumbnail process of this session still holds the old 1.x build for all users.
  const path oldBuild = MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  Holder holder;
  holder.pid = 777;
  probes_.holders.push_back({oldBuild / kHandlerDll, holder});

  SetupUserOptions options;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 5000;
  SetupUserResult result;
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(result.runOnceDeleted, 2u);
  EXPECT_FALSE(HasValue(kLegacyRunOnce));
  EXPECT_TRUE(HasValue(L"OtherApp"));
  EXPECT_EQ(StoredZoom(), 130u);
  SlotValues slots;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &slots));
  EXPECT_TRUE(slots[0] && IsOurClsid(*slots[0]));
  EXPECT_TRUE(slots[1] && IsOurClsid(*slots[1]));
  EXPECT_TRUE(result.userClassDeleted);
  ASSERT_EQ(probes_.applied.size(), 1u);
  EXPECT_EQ(probes_.applied[0].options.atSignIn, TriggerSetting::kEnabled);
  EXPECT_EQ(probes_.applied[0].options.afterPdfCraft, TriggerSetting::kEnabled);
  EXPECT_FALSE(probes_.legacyTask);
  EXPECT_EQ(probes_.ended, std::vector<DWORD>{777});
  EXPECT_FALSE(Exists(paths_.userRoot / L"settings.ini"));
  EXPECT_FALSE(Exists(paths_.userRoot / L"app" / L"1.1.0.0-67a1323e"));
  EXPECT_FALSE(result.notified);  // a sandbox: Explorer is never told
  EXPECT_TRUE(Exists(build));

  // Again, with the user's own choices: their zoom and the triggers they asked for.
  options.zoom = 150;
  options.checks = ChecksSelection{false, true};
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(StoredZoom(), 150u);
  ASSERT_EQ(probes_.applied.size(), 2u);
  EXPECT_EQ(probes_.applied[1].options.afterPdfCraft, TriggerSetting::kAbsent);
  // And again without them: what is there stays.
  options.zoom.reset();
  options.checks.reset();
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  EXPECT_EQ(StoredZoom(), 150u);
  ASSERT_EQ(probes_.applied.size(), 3u);
  EXPECT_EQ(probes_.applied[2].options.afterPdfCraft, TriggerSetting::kAbsent);
  EXPECT_EQ(probes_.applied[2].options.atSignIn, TriggerSetting::kEnabled);
}

TEST_F(Setup, SetupUserWithoutAnySettingsWrites100) {
  InstallForAllUsers();
  SetupUserOptions options;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 5000;
  SetupUserResult result;
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(StoredZoom(), 100u);
}

// Another account's tasks hold every name the user's task may have: the task step fails in words
// that name those tasks and who can remove them, and the other steps still run.
TEST_F(Setup, SetupUserSaysWhenAnotherAccountHoldsTheTaskNames) {
  InstallForAllUsers();
  probes_.applyError = kTaskTakenHr;
  SetupUserOptions options;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 5000;
  SetupUserResult result;
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kFailed);
  EXPECT_TRUE(result.taskTaken);
  EXPECT_FALSE(result.taskRegistered);
  EXPECT_EQ(StoredZoom(), 100u);
  ASSERT_EQ(result.failures.size(), 1u);
  const std::wstring& failure = result.failures[0];
  const std::string text = WideToUtf8Lossy(failure);
  EXPECT_EQ(failure.rfind(L"task: " + TaskTakenText() + L" (" + HResultText(kTaskTakenHr) + L"). " +
                              L"An administrator can delete ",
                          0),
            0u)
      << text;
  const TaskLocation& location = result.before.taskLocation;
  EXPECT_NE(failure.find(location.folder + L"\\" + location.name), std::wstring::npos) << text;
  EXPECT_NE(failure.find(L"\\" + location.fallbackName), std::wstring::npos) << text;

  // Any other failure of the step keeps its usual line.
  probes_.applyError = E_ACCESSDENIED;
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  EXPECT_FALSE(result.taskTaken);
  EXPECT_EQ(result.failures, std::vector<std::wstring>{L"task: 0x80070005"});
}

TEST_F(Setup, SetupUserRefusals) {
  PlantRunOnce();
  SetupUserOptions options;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 5000;
  SetupUserResult result;
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kNotInstalled);
  EXPECT_TRUE(HasValue(kLegacyRunOnce));

  InstallForAllUsers();
  options.context->elevated = true;
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  EXPECT_EQ(result.refusal, SetupRefusal::kElevated);
  options.context->elevated = false;
  options.context->probe = ContextProbe::kReadOnly;  // not the canary's context
  ASSERT_HRESULT_SUCCEEDED(SetupUser(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, SetupRefusal::kWrongContext);
  EXPECT_TRUE(HasValue(kLegacyRunOnce));
  EXPECT_FALSE(StoredZoom());
  EXPECT_TRUE(probes_.applied.empty());
}

// The check deletes every value of the sign-in clean-up family while installed.
TEST_F(Setup, TheCheckDeletesTheWholeRunOnceFamily) {
  InstallForAllUsers();
  ASSERT_HRESULT_SUCCEEDED(
      WriteRegistryString(sandbox_.user(), kRunOnceKey, RunOnceValueName(1), L"x"));
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.user(), kRunOnceKey, L"OtherApp", L"y"));
  const Snapshot snapshot = Gather();
  EXPECT_FALSE(snapshot.legacy.runOnce);
  EXPECT_TRUE(snapshot.legacy.runOnceOthers);
  const std::vector<Finding> findings = Evaluate(snapshot);
  EXPECT_TRUE(HasFinding(findings, FindingId::kCleanupPending));
  EXPECT_FALSE(HasFinding(findings, FindingId::kV1Leftovers));  // not 1.x's
  const RepairPlan plan = PlanRepairs(snapshot, findings, RepairMode::kSafe);
  std::vector<RepairOutcome> outcomes;
  ASSERT_HRESULT_SUCCEEDED(ApplyUserRepairs(roots_, probes_, snapshot, plan, &outcomes));
  EXPECT_FALSE(HasValue(RunOnceValueName(1).c_str()));
  EXPECT_TRUE(HasValue(L"OtherApp"));
}

// A context check that refuses: below an app package, where the test write under
// HKEY_LOCAL_MACHINE came back virtualized.
MachineWriteCheck WritesNotShown() {
  MachineWriteCheck check;
  check.packageAbove = true;
  check.probed = true;
  check.machine = Virtualization::kYes;
  check.user = Virtualization::kNo;
  return check;
}

TEST_F(Setup, SetupMachineNeedsAnAdministrator) {
  if (Elevated()) GTEST_SKIP() << "elevated: SetupMachineInTestPlaces covers it";
  const path unused = MakeBuild(paths_.machineRoot, L"1.0.0.0-0badf00d");
  SetupMachineOptions options;
  options.paths = paths_;
  options.taskFolder = L"\\" + std::wstring(kProductName) + L"-Test-never-made";
  SetupMachineResult result;
  ASSERT_HRESULT_SUCCEEDED(SetupMachine(options, roots_, &result));
  EXPECT_EQ(result.code, ExitCode::kNeedsAdmin);
  EXPECT_EQ(result.refusal, SetupRefusal::kNotElevated);
  EXPECT_TRUE(Exists(unused));
  // The token's refusal comes first, whatever is above.
  options.machineWrites = WritesNotShown();
  ASSERT_HRESULT_SUCCEEDED(SetupMachine(options, roots_, &result));
  EXPECT_EQ(result.code, ExitCode::kNeedsAdmin);
  EXPECT_EQ(result.refusal, SetupRefusal::kNotElevated);
  EXPECT_TRUE(Exists(unused));
}

// Elevated (CI): below an app package whose test writes did not come back, the step changes
// nothing: no task folder, no build folder removed, exit 8.
TEST_F(Setup, SetupMachineRefusedWhereItsWritesWereNotShown) {
  if (!Elevated()) GTEST_SKIP() << "needs elevation; runs on CI";
  const path unused = MakeBuild(paths_.machineRoot, L"1.0.0.0-0badf00d");
  GUID guid = {};
  ASSERT_HRESULT_SUCCEEDED(CoCreateGuid(&guid));
  wchar_t id[40] = {};
  StringFromGUID2(guid, id, ARRAYSIZE(id));
  const std::wstring tail =
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(id + 1, 36);
  SetupMachineOptions options;
  options.paths = paths_;
  options.taskFolder = L"\\" + std::wstring(kProductName) + L"-Test-" + tail;
  options.trashParent = temp_.path();
  options.machineWrites = WritesNotShown();
  SetupMachineResult result;
  ASSERT_HRESULT_SUCCEEDED(SetupMachine(options, roots_, &result));
  EXPECT_EQ(result.refusal, SetupRefusal::kPackaged);
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  EXPECT_TRUE(RefusesMachineWrites(result.machineWrites));
  EXPECT_TRUE(result.failures.empty());
  EXPECT_TRUE(Exists(unused));
  // Nothing to delete: the folder was never made.
  EXPECT_EQ(DeleteTaskFolderAllUsers(options.taskFolder,
                                     std::wstring(kProductName) + L"-Test-" + tail + L" "),
            S_FALSE);
}

// Elevated (CI): the whole step against a test task folder, folders under %TEMP% and the sandbox,
// while another thread holds this user's operation lock. The step takes no lock, so neither a
// holder nor another account that made the lock's names first (in session 0, where a deployment
// tool may run it, those are the only names) can stop it. Nothing is in use, so nothing goes to
// a trash folder or waits for a restart.
TEST_F(Setup, SetupMachineInTestPlaces) {
  if (!Elevated()) GTEST_SKIP() << "needs elevation; runs on CI";
  MachineWriteCheck writes;
  CheckMachineWrites(&writes);
  if (RefusesMachineWrites(writes)) {
    GTEST_SKIP() << "the step would refuse here: "
                 << WideToUtf8Lossy(MachineWriteCheckText(writes));
  }
  const path unused = MakeBuild(paths_.machineRoot, L"1.0.0.0-0badf00d");
  const path old = MakeBuild(paths_.machineRoot, L"2.0.0.0-00000001");
  const path current = InstallForAllUsers();
  GUID guid = {};
  ASSERT_HRESULT_SUCCEEDED(CoCreateGuid(&guid));
  wchar_t id[40] = {};
  StringFromGUID2(guid, id, ARRAYSIZE(id));
  const std::wstring tail =
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(id + 1, 36);
  SetupMachineOptions options;
  options.paths = paths_;
  options.taskFolder = L"\\" + std::wstring(kProductName) + L"-Test-" + tail;
  options.trashParent = temp_.path();
  // A mutex is its owning thread's, so the holder is a thread of its own, which keeps the lock
  // until the step is done.
  wil::unique_event_nothrow locked;
  wil::unique_event_nothrow done;
  ASSERT_HRESULT_SUCCEEDED(locked.create(wil::EventOptions::ManualReset));
  ASSERT_HRESULT_SUCCEEDED(done.create(wil::EventOptions::ManualReset));
  HRESULT held = E_UNEXPECTED;
  std::thread holder([&] {
    std::optional<OperationLock> lock;
    held = OperationLock::Acquire(5000, &lock);
    locked.SetEvent();
    done.wait();
  });
  const auto letGo = wil::scope_exit([&] {
    done.SetEvent();
    holder.join();
  });
  locked.wait();
  ASSERT_HRESULT_SUCCEEDED(held);
  SetupMachineResult result;
  ASSERT_HRESULT_SUCCEEDED(SetupMachine(options, roots_, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  std::printf("[ setup ] --setup-machine in test places (elevated) -> exit %d, task folder %s\n",
              static_cast<int>(result.code),
              WideToUtf8Lossy(HResultText(result.taskFolder)).c_str());
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(result.taskFolder, S_OK);
  EXPECT_FALSE(Exists(unused));
  EXPECT_FALSE(Exists(old));
  EXPECT_TRUE(Exists(current));
  EXPECT_HRESULT_SUCCEEDED(DeleteTaskFolderAllUsers(
      options.taskFolder, std::wstring(kProductName) + L"-Test-" + tail + L" "));
}

// The architecture of this Windows, as --setup-machine --build reads it.
Arch ThisWindowsArch() {
  USHORT process = IMAGE_FILE_MACHINE_UNKNOWN;
  USHORT native = IMAGE_FILE_MACHINE_UNKNOWN;
  if (!IsWow64Process2(GetCurrentProcess(), &process, &native)) return Arch::kOther;
  return ArchFromMachine(native);
}

// The folder name of the built DLLs, as the installer computes it (cmake/BuildId.cmake).
std::wstring BuiltBuildName() {
  std::wstring name;
  EXPECT_HRESULT_SUCCEEDED(ComputeBuildId(BuiltFile(kHandlerDll), BuiltFile(kPdfiumDll), &name));
  return name;
}

// Everywhere: the build --setup-machine --build registers must be the folder it is named after,
// complete, made for this Windows, and of version 2 or later. Nothing is written.
TEST_F(Setup, FindBuildToRegisterChecksTheFolder) {
  const std::wstring name = BuiltBuildName();
  ASSERT_FALSE(name.empty());
  const Arch arch = ThisWindowsArch();
  ASSERT_NE(arch, Arch::kOther);
  path dll;
  EXPECT_EQ(FindBuildToRegister(paths_.machineRoot, name, arch, &dll),
            HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
  EXPECT_TRUE(dll.empty());
  const path build = MakeBuild(paths_.machineRoot, name);
  ASSERT_HRESULT_SUCCEEDED(FindBuildToRegister(paths_.machineRoot, name, arch, &dll));
  EXPECT_EQ(dll, build / kHandlerDll);
  EXPECT_TRUE(IsMachineClassPath(paths_.machineRoot, dll));

  const Arch other = arch == Arch::kArm64 ? Arch::kX64 : Arch::kArm64;
  EXPECT_EQ(FindBuildToRegister(paths_.machineRoot, name, other, &dll),
            HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT));
  EXPECT_TRUE(dll.empty());
  for (const wchar_t* bad :
       {L"", L"app", L"..", L"2.0.0.0-1a2b3c4d\\..", L"2.0.0.0-xyz", L"1.1.0.0-67a1323e"}) {
    EXPECT_EQ(FindBuildToRegister(paths_.machineRoot, bad, arch, &dll), E_INVALIDARG)
        << WideToUtf8Lossy(bad);
  }
  // A folder whose name is not its DLLs' build id.
  const std::wstring wrong = L"2.0.0.0-1a2b3c4d";
  ASSERT_NE(name, wrong);
  MakeBuild(paths_.machineRoot, wrong);
  EXPECT_EQ(FindBuildToRegister(paths_.machineRoot, wrong, arch, &dll),
            HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
  // An incomplete one.
  ASSERT_TRUE(DeleteFileW((build / kPdfiumDll).c_str()));
  EXPECT_EQ(FindBuildToRegister(paths_.machineRoot, name, arch, &dll),
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
}

// Elevated (CI): --setup-machine --build points the class for all users at the named build and
// then removes the build the class named before; with a build that does not check out it lists
// "class" and keeps every build.
TEST_F(Setup, SetupMachineRegistersTheNamedBuild) {
  if (!Elevated()) GTEST_SKIP() << "needs elevation; runs on CI";
  MachineWriteCheck writes;
  CheckMachineWrites(&writes);
  if (RefusesMachineWrites(writes)) {
    GTEST_SKIP() << "the step would refuse here: "
                 << WideToUtf8Lossy(MachineWriteCheckText(writes));
  }
  const path before = InstallForAllUsers();
  const std::wstring name = BuiltBuildName();
  ASSERT_NE(before.filename().native(), name);
  const path build = MakeBuild(paths_.machineRoot, name);
  GUID guid = {};
  ASSERT_HRESULT_SUCCEEDED(CoCreateGuid(&guid));
  wchar_t id[40] = {};
  StringFromGUID2(guid, id, ARRAYSIZE(id));
  const std::wstring tail =
      std::to_wstring(GetCurrentProcessId()) + L"-" + std::wstring(id + 1, 36);
  SetupMachineOptions options;
  options.paths = paths_;
  options.taskFolder = L"\\" + std::wstring(kProductName) + L"-Test-" + tail;
  options.trashParent = temp_.path();
  const auto deleteTaskFolder = wil::scope_exit([&] {
    (void)DeleteTaskFolderAllUsers(options.taskFolder,
                                   std::wstring(kProductName) + L"-Test-" + tail + L" ");
  });

  options.build = L"2.0.0.0-00000002";
  SetupMachineResult result;
  ASSERT_HRESULT_SUCCEEDED(SetupMachine(options, roots_, &result));
  EXPECT_EQ(result.buildClass, HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND));
  EXPECT_EQ(result.code, ExitCode::kFailed);
  EXPECT_TRUE(Exists(before));
  EXPECT_TRUE(Exists(build));
  std::optional<ClassReg> reg;
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg));
  ASSERT_TRUE(reg);
  EXPECT_TRUE(IEquals(reg->dll, (before / kHandlerDll).native()));

  options.build = name;
  ASSERT_HRESULT_SUCCEEDED(SetupMachine(options, roots_, &result));
  std::printf("[ setup ] --setup-machine --build in test places (elevated) -> exit %d, class %s\n",
              static_cast<int>(result.code),
              WideToUtf8Lossy(HResultText(result.buildClass)).c_str());
  EXPECT_EQ(result.buildClass, S_OK);
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg));
  ASSERT_TRUE(reg);
  EXPECT_TRUE(IEquals(reg->dll, (build / kHandlerDll).native()));
  EXPECT_FALSE(Exists(before));
  EXPECT_TRUE(Exists(build));
}

}  // namespace
}  // namespace pdft
