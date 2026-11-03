// Installing and uninstalling against the real file system and registry API, but only in
// private places: a build's folder, install roots and the data folder under %TEMP%, the registry
// through a private key (ScopedRegistrySandbox: HKCU, HKLM, RunOnce and a stand-in for
// HKEY_USERS with made-up user hives), and every Windows-wide question and write (the process
// context, the tasks, the processes holding files) through the fake probes. The administrator
// steps run in this process through fake launchers, unelevated, against the same private places;
// no test starts a real elevation, deletes a task or queues a deletion at the restart. The
// build's folder holds copies of the built binaries, which are never started.

#include <windows.h>

#include "install.h"

#include <aclapi.h>
#include <gtest/gtest.h>
#include <sddl.h>
#include <wil/resource.h>

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "build_folder.h"
#include "check.h"
#include "child_process.h"
#include "elevation.h"
#include "failure_trace.h"
#include "fake_probes.h"
#include "file_ops.h"
#include "instance.h"
#include "legacy.h"
#include "paths.h"
#include "project_config.h"
#include "registration.h"
#include "registry_sandbox.h"
#include "settings.h"
#include "strings.h"
#include "temp_dir.h"
#include "uninstall.h"
#include "zoom.h"

namespace pdft {
namespace {

using std::filesystem::path;
using test::FakeProbes;

constexpr wchar_t kOtherClsid[] = L"{9C60DE1E-E5FC-40F4-A487-460851A8D915}";

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

std::wstring BuiltFile(const wchar_t* name) { return test::SelfFolder() + L"\\" + name; }

// Gives |folder| the protected DACL |sddl| ("D:P...").
bool SetDacl(const path& folder, const std::wstring& sddl) {
  wil::unique_hlocal_security_descriptor descriptor;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            descriptor.put(), nullptr)) {
    return false;
  }
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  if (!GetSecurityDescriptorDacl(descriptor.get(), &present, &dacl, &defaulted)) return false;
  return SetNamedSecurityInfoW(const_cast<wchar_t*>(folder.c_str()), SE_FILE_OBJECT,
                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                               nullptr, nullptr, dacl, nullptr) == ERROR_SUCCESS;
}

// Gives |folder| a protected DACL: this user, SYSTEM and Administrators, plus |extra|.
bool SetFolderDacl(const path& folder, const std::wstring& extra) {
  std::wstring sid;
  if (FAILED(GetCurrentUserSid(&sid))) return false;
  return SetDacl(folder, L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)" + extra);
}

class Install : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sandbox_.ok()) << HResultText(sandbox_.error());
    ASSERT_TRUE(temp_.ok());
    roots_ = {sandbox_.user(), sandbox_.machine()};
    const path base = temp_.path();
    paths_.userRoot = base / L"user-root";
    paths_.machineRoot = base / L"machine-root";
    paths_.legacyUserRoot = paths_.userRoot;
    paths_.legacyMachineRoot = paths_.machineRoot;
    paths_.legacyIni = paths_.userRoot / L"settings.ini";
    paths_.ownExe = test::SelfPath();
    dataDir_ = base / L"data";
    ASSERT_TRUE(MakeDirs(paths_.userRoot));

    // A staged build: the four binaries (never started) and some notices.
    stage_ = base / L"stage";
    ASSERT_TRUE(MakeDirs(stage_ / L"licenses" / L"pdfium"));
    for (const wchar_t* name : {kSettingsExe, kCliExe, kHandlerDll, kPdfiumDll}) {
      ASSERT_TRUE(CopyFileW(BuiltFile(name).c_str(), (stage_ / name).c_str(), TRUE)) << name;
    }
    ASSERT_TRUE(WriteFileText(stage_ / L"LICENSE.txt", "license\n"));
    ASSERT_TRUE(WriteFileText(stage_ / L"licenses" / L"pdfium" / L"LICENSE.a", "a\n"));
    ASSERT_TRUE(WriteFileText(stage_ / L"licenses" / L"wil-LICENSE.txt", "wil\n"));
    ASSERT_TRUE(SetFolderDacl(stage_, L""));
    // Its final path, the way --install names it: the step for all users holds the folder as it
    // is given, where a link on the way is refused, and %TEMP% may be written with 8.3 names or
    // through a link.
    path canonicalStage;
    ASSERT_HRESULT_SUCCEEDED(Canonicalize(stage_, &canonicalStage));
    stage_ = canonicalStage;
    // The folders above %TEMP% are the PC's; where other accounts could change one of them, the
    // tests that copy from the stage for all users skip.
    const std::vector<std::wstring> binaries = {kSettingsExe, kCliExe, kHandlerDll, kPdfiumDll};
    ASSERT_HRESULT_SUCCEEDED(FindUnsafeBuildFolderAccess(stage_, binaries, &stageUnsafe_));

    HKEY users = nullptr;
    ASSERT_EQ(RegCreateKeyExW(sandbox_.user(), L"Users", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                              &users, nullptr),
              ERROR_SUCCESS);
    users_.reset(users);
  }

  // Why a test that copies from the stage for all users cannot run here.
  std::optional<std::string> StageUnsafe() const {
    if (!stageUnsafe_) return std::nullopt;
    return "other accounts could change " + WideToUtf8Lossy(stageUnsafe_->object.native()) + " (" +
           WideToUtf8Lossy(stageUnsafe_->access.principal) + ")";
  }

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

  std::optional<std::wstring> MachineClassDll() {
    std::optional<ClassReg> reg;
    EXPECT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg));
    if (!reg) return std::nullopt;
    return reg->dll;
  }

  std::optional<std::wstring> UserClassDll() {
    std::optional<ClassReg> reg;
    EXPECT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
    if (!reg) return std::nullopt;
    return reg->dll;
  }

  std::wstring BuildName() {
    std::wstring name;
    EXPECT_HRESULT_SUCCEEDED(ComputeBuildId(stage_ / kHandlerDll, stage_ / kPdfiumDll, &name));
    return name;
  }

  std::optional<DWORD> StoredZoom() {
    std::optional<RegistryValue> value;
    if (FAILED(ReadRegistryValue(sandbox_.user(), kRegKey, kZoomValueName, &value)) || !value) {
      return std::nullopt;
    }
    return RegistryDword(*value);
  }

  bool HasRunOnce(const std::wstring& name) {
    std::optional<RegistryValue> value;
    return SUCCEEDED(ReadRegistryValue(sandbox_.user(), kRunOnceKey, name, &value)) &&
           value.has_value();
  }

  // 1.x's per-user folder as 1.1 left it (stand-ins: the launcher marks it 1.x's by its name).
  void Plant11UserFolder(int zoom) {
    const path root = paths_.userRoot;
    ASSERT_TRUE(WriteFileText(root / L"settings.ini",
                              "[Thumbnails]\r\nZoom=" + std::to_string(zoom) + "\r\n"));
    ASSERT_TRUE(WriteFileText(root / L"repair.log", "repaired\r\n"));
    ASSERT_TRUE(WriteFileText(root / (std::wstring(kLegacyRootName) + L".cmd"), "@echo off\r\n"));
    MakeBuild(root, L"1.1.0.0-67a1323e");
  }

  // The administrator step machine-install, run in this process against the private places,
  // the way the elevated settings executable would run it from its own folder.
  ElevationLauncher MachineInstallLauncher() {
    return [this](const ElevationRequest& request, DWORD* processId, DWORD* exitCode) {
      requests_.push_back(request);
      MachineInstallOptions options;
      options.source = request.exe.parent_path();
      options.machineRoot = paths_.machineRoot;
      options.legacyMachineRoot = paths_.legacyMachineRoot;
      options.trashParent = temp_.path();
      options.taskFolder.clear();
      options.osArch = probes_.context.osArch;
      MachineInstallResult result;
      const HRESULT hr = InstallForAllUsers(options, roots_, &result);
      *processId = 0;
      *exitCode = FAILED(hr) ? static_cast<DWORD>(hr) : (result.restartPending ? 1u : 0u);
      return S_OK;
    };
  }

  // The administrator step machine-uninstall, the same way, with the stand-in user hives.
  ElevationLauncher MachineUninstallLauncher() {
    return [this](const ElevationRequest& request, DWORD* processId, DWORD* exitCode) {
      requests_.push_back(request);
      MachineUninstallOptions options;
      options.machineRoot = paths_.machineRoot;
      options.legacyMachineRoot = paths_.legacyMachineRoot;
      options.trashParent = temp_.path();
      options.taskFolder.clear();
      options.users = users_.get();
      options.retryMs = 0;
      MachineUninstallResult result;
      const HRESULT hr = UninstallForAllUsers(options, roots_, &result);
      *processId = 0;
      *exitCode = FAILED(hr)                  ? static_cast<DWORD>(hr)
                  : FAILED(result.firstError) ? static_cast<DWORD>(result.firstError)
                  : result.restartPending     ? 1u
                                              : 0u;
      return S_OK;
    };
  }

  // A launcher that only answers |launch| and |exitCode|.
  ElevationLauncher AnsweringLauncher(HRESULT launch, DWORD exitCode) {
    return
        [this, launch, exitCode](const ElevationRequest& request, DWORD* processId, DWORD* code) {
          requests_.push_back(request);
          *processId = 0;
          *code = exitCode;
          return launch;
        };
  }

  InstallOptions MakeInstallOptions(Scope scope) {
    InstallOptions options;
    options.scope = scope;
    options.context = probes_.context;
    options.paths = paths_;
    options.source = stage_;
    options.lockWaitMs = 5000;
    options.launcher = MachineInstallLauncher();
    return options;
  }

  UninstallOptions MakeUninstallOptions() {
    UninstallOptions options;
    options.yes = true;
    options.context = probes_.context;
    options.paths = paths_;
    options.dataDir = dataDir_;
    options.lockWaitMs = 5000;
    options.launcher = MachineUninstallLauncher();
    options.runner = [this](const path& exe, const std::wstring& arguments, DWORD* exitCode) {
      runs_.push_back(exe.native() + L" " + arguments);
      *exitCode = 0;
      return S_OK;
    };
    return options;
  }

  // A made-up user hive below the stand-in for HKEY_USERS.
  HKEY Hive(const std::wstring& name) {
    HKEY key = nullptr;
    EXPECT_EQ(RegCreateKeyExW(users_.get(), name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                              &key, nullptr),
              ERROR_SUCCESS);
    hives_.emplace_back(key);
    return key;
  }

  test::ScopedRegistrySandbox sandbox_;
  test::TempDir temp_{L"install"};
  RegistryRoots roots_;
  EnginePaths paths_;
  path stage_;
  path dataDir_;
  std::optional<UnsafeAccess> stageUnsafe_;
  wil::unique_hkey users_;
  std::vector<wil::unique_hkey> hives_;
  FakeProbes probes_;
  std::vector<ElevationRequest> requests_;
  std::vector<std::wstring> runs_;
};

// ---- A build's folder and copying it ----------------------------------------------------------

TEST_F(Install, ReadsABuildsFolder) {
  BuildSource source;
  SourceCheck check;
  ASSERT_HRESULT_SUCCEEDED(ReadBuildSource(stage_, probes_.context.osArch, &source, &check));
  EXPECT_EQ(check.problem, SourceProblem::kNone);
  EXPECT_EQ(source.buildName, BuildName());
  EXPECT_TRUE(IsInstallableVersion(source.version)) << WideToUtf8Lossy(source.version);
  const std::vector<path> notices = {L"LICENSE.txt", L"licenses\\pdfium\\LICENSE.a",
                                     L"licenses\\wil-LICENSE.txt"};
  EXPECT_EQ(source.notices, notices);

  // Another architecture than Windows': the build is not for this PC.
  const Arch other = probes_.context.osArch == Arch::kX64 ? Arch::kArm64 : Arch::kX64;
  ASSERT_HRESULT_SUCCEEDED(ReadBuildSource(stage_, other, &source, &check));
  EXPECT_EQ(check.problem, SourceProblem::kWrongArch);
  EXPECT_EQ(check.file, kSettingsExe);

  ASSERT_TRUE(DeleteFileW((stage_ / kCliExe).c_str()));
  ASSERT_HRESULT_SUCCEEDED(ReadBuildSource(stage_, probes_.context.osArch, &source, &check));
  EXPECT_EQ(check.problem, SourceProblem::kMissingFile);
  EXPECT_EQ(check.file, kCliExe);
}

TEST_F(Install, CopiesABuildAndReusesIt) {
  BuildSource source;
  SourceCheck check;
  ASSERT_HRESULT_SUCCEEDED(ReadBuildSource(stage_, probes_.context.osArch, &source, &check));
  const path root = path(temp_.path()) / L"root";
  CopyBuildResult result;
  ASSERT_HRESULT_SUCCEEDED(CopyBuild(source, root, CopyBuildOptions{}, &result));
  EXPECT_EQ(result.buildDir, root / L"app" / BuildName());
  EXPECT_FALSE(result.reused);
  EXPECT_EQ(result.copied, 2u + 2u + source.notices.size());
  for (const path& file :
       {root / kSettingsExe, root / kCliExe, root / L"LICENSE.txt",
        root / L"licenses" / L"pdfium" / L"LICENSE.a", result.dll, result.buildDir / kPdfiumDll}) {
    EXPECT_TRUE(Exists(file)) << file;
  }
  EXPECT_FALSE(Exists(root / kHandlerDll));  // the DLLs only in their build folder

  // Again: nothing to write.
  ASSERT_HRESULT_SUCCEEDED(CopyBuild(source, root, CopyBuildOptions{}, &result));
  EXPECT_TRUE(result.reused);
  EXPECT_EQ(result.copied, 0u);

  // A build folder of that name with other DLLs is not the same build: it is copied again.
  ASSERT_TRUE(WriteFileText(result.buildDir / kPdfiumDll, "other"));
  ASSERT_HRESULT_SUCCEEDED(CopyBuild(source, root, CopyBuildOptions{}, &result));
  EXPECT_FALSE(result.reused);
  EXPECT_EQ(result.copied, 1u);

  // An app folder that is a junction leads the copy nowhere.
  const path linked = path(temp_.path()) / L"linked";
  const path elsewhere = path(temp_.path()) / L"elsewhere";
  ASSERT_TRUE(MakeDirs(linked / L"app"));
  ASSERT_TRUE(MakeDirs(elsewhere));
  ASSERT_TRUE(test::MakeJunction((linked / L"app").native(), elsewhere.native()));
  EXPECT_EQ(CopyBuild(source, linked, CopyBuildOptions{}, &result),
            HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  EXPECT_TRUE(std::filesystem::is_empty(elsewhere));
}

// ---- The step for all users, in test places -------------------------------------------------

TEST_F(Install, InstallForAllUsersInTestPlaces) {
  if (const auto why = StageUnsafe()) GTEST_SKIP() << *why;
  const path v1 = MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  const path old = MakeBuild(paths_.machineRoot, L"2.0.0.0-00000001");
  RegisterMachineClass(v1 / kHandlerDll);
  MachineInstallOptions options;
  options.source = stage_;
  options.machineRoot = paths_.machineRoot;
  options.legacyMachineRoot = paths_.legacyMachineRoot;
  options.trashParent = temp_.path();
  options.taskFolder.clear();
  options.osArch = probes_.context.osArch;
  MachineInstallResult result;
  ASSERT_HRESULT_SUCCEEDED(InstallForAllUsers(options, roots_, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  const path dll = paths_.machineRoot / L"app" / BuildName() / kHandlerDll;
  EXPECT_EQ(MachineClassDll(), dll.native());
  EXPECT_TRUE(Exists(paths_.machineRoot / kSettingsExe));
  EXPECT_TRUE(Exists(paths_.machineRoot / L"licenses" / L"wil-LICENSE.txt"));
  // The old builds went last, once the class pointed at the new one.
  EXPECT_FALSE(Exists(v1));
  EXPECT_FALSE(Exists(old));
  EXPECT_EQ(result.v1.removed, std::vector<std::wstring>{L"1.1.0.0-67a1323e"});
  EXPECT_FALSE(result.restartPending);

  ASSERT_HRESULT_SUCCEEDED(InstallForAllUsers(options, roots_, &result));
  EXPECT_TRUE(result.copy.reused);
  EXPECT_EQ(result.copy.copied, 0u);

  // The elevated side checks the folder's permissions itself.
  ASSERT_TRUE(SetFolderDacl(stage_, L"(A;OICI;0x12019f;;;WD)"));
  EXPECT_EQ(InstallForAllUsers(options, roots_, &result), kUnsafeBuildFolderHr);
  ASSERT_TRUE(result.unsafe);
  EXPECT_EQ(result.unsafe->access.principal, L"Everyone");
  ASSERT_TRUE(SetFolderDacl(stage_, L""));
  // ... and never copies a build from inside the install folder.
  options.source = paths_.machineRoot;
  options.checkSourceAccess = false;
  ASSERT_TRUE(
      CopyFileW((stage_ / kHandlerDll).c_str(), (paths_.machineRoot / kHandlerDll).c_str(), FALSE));
  ASSERT_TRUE(
      CopyFileW((stage_ / kPdfiumDll).c_str(), (paths_.machineRoot / kPdfiumDll).c_str(), FALSE));
  EXPECT_EQ(InstallForAllUsers(options, roots_, &result), E_INVALIDARG);
}

// The step for all users says where it failed (failure_trace.h): the part of the step, the
// source file's name, the function, the HRESULT and a name of the product's own, never a path.
TEST_F(Install, TheStepForAllUsersSaysWhereItFailed) {
  MachineInstallOptions options;
  options.source = stage_;
  options.machineRoot = paths_.machineRoot;
  options.legacyMachineRoot = paths_.legacyMachineRoot;
  options.trashParent = temp_.path();
  options.taskFolder.clear();
  options.checkSourceAccess = false;  // the folders above %TEMP% are the PC's
  options.osArch = probes_.context.osArch;
  MachineInstallResult result;
  {
    const FailureTrace trace;
    ASSERT_HRESULT_SUCCEEDED(InstallForAllUsers(options, roots_, &result));
    EXPECT_FALSE(trace.failure());  // nothing pending once it worked
  }

  // An install folder this user may not make, as in Program Files for a process without the
  // Administrators group: the first folder of the copy fails, and nothing is made.
  const path locked = path(temp_.path()) / L"locked";
  ASSERT_TRUE(MakeDirs(locked));
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  ASSERT_TRUE(SetDacl(locked, L"D:P(A;;0x1200a9;;;" + sid + L")"));  // read and list only
  // Changeable again however the checks below end, or the temporary folder could not go.
  auto unlock = wil::scope_exit([&]() noexcept { SetFolderDacl(locked, L""); });
  options.machineRoot = locked / kProductName;
  {
    const FailureTrace trace;
    EXPECT_EQ(InstallForAllUsers(options, roots_, &result), E_ACCESSDENIED);
    ASSERT_TRUE(trace.failure());
    const TracedFailure& failure = *trace.failure();
    EXPECT_EQ(failure.hr, E_ACCESSDENIED);
    EXPECT_STREQ(failure.step, L"copy-build");
    EXPECT_STREQ(failure.site.file, "install.cpp");
    EXPECT_STREQ(failure.site.function, "MakeRealFolder");
    EXPECT_STREQ(failure.detail.data(), kProductName);
    const std::wstring text = TracedFailureText(failure);
    std::printf("traced: %ls\n", text.c_str());
    EXPECT_EQ(text.find_first_of(L"\\/"), std::wstring::npos) << WideToUtf8Lossy(text);
  }
  EXPECT_FALSE(Exists(options.machineRoot));
  unlock.release();
  ASSERT_TRUE(SetFolderDacl(locked, L""));

  // An install folder that is a file.
  const path file = path(temp_.path()) / L"a-file";
  ASSERT_TRUE(WriteFileText(file, "not a folder"));
  options.machineRoot = file;
  {
    const FailureTrace trace;
    EXPECT_EQ(InstallForAllUsers(options, roots_, &result), HRESULT_FROM_WIN32(ERROR_DIRECTORY));
    ASSERT_TRUE(trace.failure());
    EXPECT_STREQ(trace.failure()->step, L"copy-build");
    EXPECT_STREQ(trace.failure()->site.function, "MakeRealFolder");
    EXPECT_STREQ(trace.failure()->detail.data(), L"a-file");
  }

  // A binary gone from the build's folder: the permission check names it.
  options.machineRoot = paths_.machineRoot;
  options.checkSourceAccess = true;
  ASSERT_TRUE(DeleteFileW((stage_ / kPdfiumDll).c_str()));
  {
    const FailureTrace trace;
    EXPECT_EQ(InstallForAllUsers(options, roots_, &result),
              HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
    ASSERT_TRUE(trace.failure());
    EXPECT_STREQ(trace.failure()->step, L"folder-access");
    EXPECT_STREQ(trace.failure()->site.file, "file_ops.cpp");
    EXPECT_STREQ(trace.failure()->site.function, "HoldBuildFolder");
    EXPECT_STREQ(trace.failure()->detail.data(), kPdfiumDll);
  }
}

// ---- The build's folder held for its copy ---------------------------------------------------

// What a staged runtime holds: the binaries, the notice at the top and everything in licenses\,
// and nobody but this user and the administrators can change any of it.
TEST_F(Install, TheStagedRuntimeIsHeldWhole) {
  HeldBuildFolder held;
  std::optional<UnsafeAccess> unsafe;
  ASSERT_HRESULT_SUCCEEDED(HoldBuildFolder(stage_, BuildHoldOptions(), &held, &unsafe));
  if (unsafe && !IEquals(unsafe->object.native(), stage_.native()) &&
      !IsUnderLexically(unsafe->object, stage_)) {
    GTEST_SKIP() << "the folders above %TEMP% are the PC's: "
                 << WideToUtf8Lossy(UnsafeAccessText(*unsafe));
  }
  ASSERT_FALSE(unsafe) << WideToUtf8Lossy(UnsafeAccessText(*unsafe));
  const std::vector<path> files = {kSettingsExe,
                                   kCliExe,
                                   kHandlerDll,
                                   kPdfiumDll,
                                   L"LICENSE.txt",
                                   L"licenses\\pdfium\\LICENSE.a",
                                   L"licenses\\wil-LICENSE.txt"};
  EXPECT_EQ(held.files(), files);
}

// The copy reads what is held, and the folder cannot be renamed or replaced until it is done.
TEST_F(Install, CopiesFromWhatIsHeld) {
  BuildSource source;
  SourceCheck check;
  ASSERT_HRESULT_SUCCEEDED(ReadBuildSource(stage_, probes_.context.osArch, &source, &check));
  HeldBuildFolder held;
  std::optional<UnsafeAccess> unsafe;
  BuildFolderHoldOptions hold = BuildHoldOptions();
  hold.checkAccess = false;  // the folders above %TEMP% are the PC's
  ASSERT_HRESULT_SUCCEEDED(HoldBuildFolder(stage_, hold, &held, &unsafe));
  ASSERT_FALSE(unsafe);
  const path moved = path(temp_.path()) / L"moved";
  EXPECT_FALSE(MoveFileExW(stage_.c_str(), moved.c_str(), 0));
  CopyBuildOptions options;
  options.held = &held;
  const path root = path(temp_.path()) / L"root";
  CopyBuildResult result;
  ASSERT_HRESULT_SUCCEEDED(CopyBuild(source, root, options, &result));
  EXPECT_EQ(result.copied, 2u + 2u + source.notices.size());
  EXPECT_TRUE(Exists(root / L"licenses" / L"pdfium" / L"LICENSE.a"));
  EXPECT_FALSE(MoveFileExW(stage_.c_str(), moved.c_str(), 0));
  held.Release();

  // A file put there after the folder was held is not held, and never read by its path instead.
  ASSERT_HRESULT_SUCCEEDED(HoldBuildFolder(stage_, hold, &held, &unsafe));
  ASSERT_TRUE(WriteFileText(stage_ / L"not-held.txt", "x"));
  source.notices.push_back(L"not-held.txt");
  EXPECT_EQ(CopyBuild(source, root, options, &result), E_UNEXPECTED);
  EXPECT_FALSE(Exists(root / L"not-held.txt"));
  held.Release();
}

TEST_F(Install, TheStepForAllUsersRefusesALinkInLicenses) {
  const path elsewhere = path(temp_.path()) / L"elsewhere";
  ASSERT_TRUE(MakeDirs(elsewhere));
  ASSERT_TRUE(WriteFileText(elsewhere / L"other.txt", "other\n"));
  const path link = stage_ / L"licenses" / L"link";
  ASSERT_TRUE(MakeDirs(link));
  ASSERT_TRUE(test::MakeJunction(link.native(), elsewhere.native()));
  MachineInstallOptions options;
  options.source = stage_;
  options.machineRoot = paths_.machineRoot;
  options.legacyMachineRoot = paths_.legacyMachineRoot;
  options.trashParent = temp_.path();
  options.taskFolder.clear();
  options.checkSourceAccess = false;  // links are refused all the same
  options.osArch = probes_.context.osArch;
  MachineInstallResult result;
  EXPECT_EQ(InstallForAllUsers(options, roots_, &result), kUnsafeBuildFolderHr);
  ASSERT_TRUE(result.unsafe);
  EXPECT_EQ(result.unsafe->kind, UnsafeKind::kLink);
  EXPECT_TRUE(IEquals(result.unsafe->object.native(), link.native()));
  EXPECT_FALSE(Exists(paths_.machineRoot));
  EXPECT_FALSE(MachineClassDll());
}

TEST_F(Install, PerUserInstallRefusesALicensesFolderOthersMayChange) {
  const std::wstring other =
      L"S-1-5-21-"
      L"1-2-3-1005";
  const path pdfium = stage_ / L"licenses" / L"pdfium";
  ASSERT_TRUE(SetFolderDacl(pdfium, L"(A;OICI;0x1301bf;;;" + other + L")"));
  InstallResult result;
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kUser), roots_, probes_, &result));
  EXPECT_EQ(result.refusal, InstallRefusal::kUnsafeFolder);
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  ASSERT_TRUE(result.unsafe);
  EXPECT_TRUE(IEquals(result.unsafe->object.native(), pdfium.native()));
  EXPECT_EQ(result.unsafe->access.sid, other);
  EXPECT_FALSE(Exists(paths_.userRoot / kSettingsExe));
  EXPECT_FALSE(UserClassDll());
}

TEST_F(Install, RepairMachineClassPicksTheNewestCompleteBuild) {
  const path v1 = MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  std::filesystem::path dll;
  EXPECT_EQ(RepairMachineClass(roots_, paths_.machineRoot, probes_.context.osArch, &dll),
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND));
  EXPECT_FALSE(MachineClassDll());
  MakeBuild(paths_.machineRoot, L"2.0.0.0-00000001");
  const path newest = MakeBuild(paths_.machineRoot, L"2.0.0.1-00000002");
  const path incomplete = paths_.machineRoot / L"app" / L"2.0.0.2-00000003";
  ASSERT_TRUE(MakeDirs(incomplete));
  ASSERT_TRUE(CopyFileW(BuiltFile(kHandlerDll).c_str(), (incomplete / kHandlerDll).c_str(), TRUE));
  ASSERT_HRESULT_SUCCEEDED(
      RepairMachineClass(roots_, paths_.machineRoot, probes_.context.osArch, &dll));
  EXPECT_EQ(dll, newest / kHandlerDll);
  EXPECT_EQ(MachineClassDll(), (newest / kHandlerDll).native());
  EXPECT_TRUE(Exists(v1));
}

// ---- --install ------------------------------------------------------------------------------

TEST_F(Install, InstallForAllUsersEndToEnd) {
  if (const auto why = StageUnsafe()) GTEST_SKIP() << *why;
  // 1.1 installed for all users and its per-user leftovers; a thumbnail process of this session
  // holds the 1.1 build.
  const path v1 = MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  RegisterMachineClass(v1 / kHandlerDll);
  Plant11UserFolder(120);
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.user(), kRunOnceKey, kLegacyRunOnce,
                                               L"cmd.exe /d /c rd /s /q \"x\""));
  probes_.legacyTask = true;
  Holder holder;
  holder.pid = 4242;
  probes_.holders.push_back({v1 / kHandlerDll, holder});

  InstallResult result;
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kMachine), roots_, probes_, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(result.refusal, InstallRefusal::kNone);
  ASSERT_EQ(requests_.size(), 1u);
  // The install names the folder by its final path; %TEMP% may be written with 8.3 names or
  // through a link.
  path stage;
  ASSERT_HRESULT_SUCCEEDED(Canonicalize(stage_, &stage));
  EXPECT_TRUE(IEquals(requests_[0].exe.native(), (stage / kSettingsExe).native()))
      << WideToUtf8Lossy(requests_[0].exe.native());
  EXPECT_EQ(requests_[0].parameters, L"--elevated machine-install");
  // The holder was ended before the step, so the old build could go.
  EXPECT_EQ(probes_.ended, std::vector<DWORD>{4242});
  EXPECT_EQ(result.holdersEnded, 1u);
  EXPECT_FALSE(Exists(v1));
  EXPECT_EQ(MachineClassDll(), (paths_.machineRoot / L"app" / BuildName() / kHandlerDll).native());
  // The per-user step: 1.x's zoom kept, its sign-in clean-up and its task gone, the slots ours,
  // the check task registered.
  EXPECT_EQ(StoredZoom(), 120u);
  EXPECT_FALSE(HasRunOnce(kLegacyRunOnce));
  EXPECT_FALSE(probes_.legacyTask);
  SlotValues slots;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &slots));
  EXPECT_TRUE(slots[0] && IsOurClsid(*slots[0]));
  ASSERT_FALSE(probes_.applied.empty());
  EXPECT_TRUE(IEquals(probes_.applied.back().program.native(),
                      (paths_.machineRoot / kSettingsExe).native()));
  EXPECT_FALSE(Exists(paths_.userRoot / L"settings.ini"));
  EXPECT_FALSE(Exists(paths_.userRoot / L"app" / L"1.1.0.0-67a1323e"));
  // The status after it, with the (fake) Explorer test.
  EXPECT_EQ(result.after.scope, Scope::kMachine);
  EXPECT_GE(probes_.liveRuns, 1);
  EXPECT_TRUE(HealthyFromFindings(result.findings));

  // Installing again changes nothing and asks again (the step is idempotent).
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kMachine), roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(requests_.size(), 2u);
}

TEST_F(Install, TheStepForAllUsersDeclinedOrFailed) {
  if (const auto why = StageUnsafe()) GTEST_SKIP() << *why;
  InstallOptions options = MakeInstallOptions(Scope::kMachine);
  InstallResult result;
  options.launcher = AnsweringLauncher(HRESULT_FROM_WIN32(ERROR_CANCELLED), 0);
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kCancelled);
  EXPECT_FALSE(MachineClassDll());
  EXPECT_FALSE(StoredZoom());  // the per-user step did not run either
  EXPECT_TRUE(probes_.applied.empty());

  options.launcher = AnsweringLauncher(S_OK, static_cast<DWORD>(kUnsafeBuildFolderHr));
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  EXPECT_EQ(result.refusal, InstallRefusal::kUnsafeFolder);

  options.launcher = AnsweringLauncher(S_OK, static_cast<DWORD>(E_ACCESSDENIED));
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kFailed);
  EXPECT_TRUE(probes_.applied.empty());

  // Started without administrator rights, the step refused: exit 7, no refusal of --install's
  // own, nothing listed as failed, and nothing installed for anyone.
  options.launcher = AnsweringLauncher(S_OK, static_cast<DWORD>(kNoAdministratorRightsHr));
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kNeedsAdmin);
  EXPECT_EQ(result.refusal, InstallRefusal::kNone);
  ASSERT_TRUE(result.run);
  EXPECT_TRUE(LackedAdministratorRights(*result.run));
  EXPECT_TRUE(result.failures.empty());
  EXPECT_FALSE(MachineClassDll());
  EXPECT_FALSE(StoredZoom());
  EXPECT_TRUE(probes_.applied.empty());
}

TEST_F(Install, InstallForThisUser) {
  if (const auto why = StageUnsafe()) GTEST_SKIP() << *why;
  Plant11UserFolder(130);
  ASSERT_HRESULT_SUCCEEDED(
      WriteRegistryString(sandbox_.user(), kRunOnceKey, RunOnceValueName(1), L"x"));
  InstallResult result;
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kUser), roots_, probes_, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_EQ(result.scope, Scope::kUser);
  EXPECT_TRUE(requests_.empty());  // no administrator step
  const path dll = paths_.userRoot / L"app" / BuildName() / kHandlerDll;
  EXPECT_EQ(UserClassDll(), dll.native());
  EXPECT_TRUE(Exists(paths_.userRoot / kSettingsExe));
  EXPECT_TRUE(Exists(paths_.userRoot / L"licenses" / L"pdfium" / L"LICENSE.a"));
  // 1.x's per-user files and its build: the build folder went before the copy (nothing held
  // it), settings.ini once the zoom was in the registry.
  ASSERT_FALSE(result.v1Plan.steps.empty());
  EXPECT_FALSE(Exists(paths_.userRoot / L"app" / L"1.1.0.0-67a1323e"));
  EXPECT_FALSE(Exists(paths_.userRoot / L"repair.log"));
  EXPECT_FALSE(Exists(paths_.userRoot / L"settings.ini"));
  EXPECT_EQ(StoredZoom(), 130u);
  EXPECT_FALSE(HasRunOnce(RunOnceValueName(1)));
  EXPECT_EQ(result.runOnceDeleted, 1u);  // before the copy, not only in the per-user step
  EXPECT_EQ(result.after.scope, Scope::kUser);
}

// A pending sign-in clean-up of the per-user folder goes before anything is copied there, so a
// per-user install that stops half-way does not lose its files at the next sign-in.
TEST_F(Install, PerUserInstallDropsSignInCleanupsBeforeCopying) {
  if (const auto why = StageUnsafe()) GTEST_SKIP() << *why;
  ASSERT_HRESULT_SUCCEEDED(
      WriteRegistryString(sandbox_.user(), kRunOnceKey, RunOnceValueName(0), L"x"));
  // A file where the copy needs the licenses folder: the copy fails half-way.
  ASSERT_TRUE(WriteFileText(paths_.userRoot / L"licenses", "in the way\n"));
  InstallResult result;
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kUser), roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kFailed);
  ASSERT_FALSE(result.failures.empty());
  EXPECT_EQ(result.failures.back().rfind(L"copy: ", 0), 0u)
      << WideToUtf8Lossy(result.failures.back());
  EXPECT_FALSE(UserClassDll());
  EXPECT_TRUE(probes_.applied.empty());  // the per-user step did not run
  EXPECT_EQ(result.runOnceDeleted, 1u);
  EXPECT_FALSE(HasRunOnce(RunOnceValueName(0)));
}

TEST_F(Install, PerUserFollows1xRules) {
  if (const auto why = StageUnsafe()) GTEST_SKIP() << *why;
  // A class for all users of this version: the install is for all users instead.
  const path build = MakeBuild(paths_.machineRoot, L"2.0.0.0-00000001");
  RegisterMachineClass(build / kHandlerDll);
  InstallResult result;
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kUser), roots_, probes_, &result));
  EXPECT_TRUE(result.switchedToMachine);
  EXPECT_EQ(result.scope, Scope::kMachine);
  EXPECT_EQ(requests_.size(), 1u);
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_FALSE(UserClassDll());

  // 1.x's class for all users, in a root of 1.x's that holds only its build: refused, unless
  // forced.
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryTree(sandbox_.machine(), ClassKeyPath()));
  std::error_code error;
  std::filesystem::remove_all(paths_.machineRoot, error);
  ASSERT_FALSE(error) << error.message();
  const path v1 = MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  RegisterMachineClass(v1 / kHandlerDll);
  requests_.clear();
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kUser), roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  EXPECT_EQ(result.refusal, InstallRefusal::kV1Machine);
  EXPECT_FALSE(Exists(paths_.userRoot / kSettingsExe));

  // Forced, with 1.x's task and a pending sign-in clean-up of the per-user folder.
  probes_.legacyTask = true;
  probes_.applied.clear();
  ASSERT_HRESULT_SUCCEEDED(
      WriteRegistryString(sandbox_.user(), kRunOnceKey, RunOnceValueName(0), L"x"));
  InstallOptions forced = MakeInstallOptions(Scope::kUser);
  forced.force = true;
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(forced, roots_, probes_, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  EXPECT_EQ(result.scope, Scope::kUser);
  EXPECT_NE(result.code, ExitCode::kFailed);
  EXPECT_TRUE(requests_.empty());
  EXPECT_FALSE(Exists(paths_.machineRoot / kSettingsExe));
  // The per-user step ran with the per-user program, and keeps the forced per-user class: it
  // does not hide this version's class.
  EXPECT_EQ(result.setup.refusal, SetupRefusal::kNone);
  EXPECT_EQ(UserClassDll(), (paths_.userRoot / L"app" / BuildName() / kHandlerDll).native());
  EXPECT_EQ(MachineClassDll(), (v1 / kHandlerDll).native());
  EXPECT_FALSE(HasRunOnce(RunOnceValueName(0)));
  EXPECT_EQ(result.runOnceDeleted, 1u);
  ASSERT_FALSE(probes_.applied.empty());
  EXPECT_TRUE(
      IEquals(probes_.applied.back().program.native(), (paths_.userRoot / kSettingsExe).native()));
  EXPECT_TRUE(result.setup.taskRegistered);
  EXPECT_FALSE(probes_.legacyTask);
  EXPECT_TRUE(result.setup.migration.marked);
  // The status after it names the same program.
  EXPECT_EQ(result.after.scope, Scope::kMachine);
  const std::optional<path> program = InstalledSettingsExe(result.after);
  ASSERT_TRUE(program);
  EXPECT_TRUE(IEquals(program->native(), (paths_.userRoot / kSettingsExe).native()));
}

TEST_F(Install, Refusals) {
  InstallOptions options = MakeInstallOptions(Scope::kUser);
  InstallResult result;
  options.context->elevated = true;
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, InstallRefusal::kElevated);
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  options.context->elevated = false;
  options.context->virtualization = Virtualization::kYes;
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, InstallRefusal::kWrongContext);
  options.context->virtualization = Virtualization::kNo;

  // The installer's install, in either hive.
  ASSERT_HRESULT_SUCCEEDED(
      WriteRegistryString(sandbox_.user(), InnoUninstallKey(), L"UninstallString", L"x"));
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, InstallRefusal::kInnoInstall);
  ASSERT_TRUE(result.inno);
  EXPECT_EQ(result.inno->scope, Scope::kUser);
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryTree(sandbox_.user(), InnoUninstallKey()));

  // A build's folder inside the install folder.
  const path inside = paths_.userRoot / L"build";
  ASSERT_TRUE(MakeDirs(inside));
  for (const wchar_t* name : {kSettingsExe, kCliExe, kHandlerDll, kPdfiumDll}) {
    ASSERT_TRUE(CopyFileW((stage_ / name).c_str(), (inside / name).c_str(), TRUE));
  }
  options.source = inside;
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, InstallRefusal::kInsideRoot);

  // Not a build.
  options.source = temp_.path();
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, InstallRefusal::kNotABuild);
  EXPECT_EQ(result.sourceCheck.problem, SourceProblem::kMissingFile);

  // A folder other accounts could change, for all users and for this user.
  ASSERT_TRUE(SetFolderDacl(stage_, L"(A;OICI;0x12019f;;;AU)"));
  options = MakeInstallOptions(Scope::kMachine);
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, InstallRefusal::kUnsafeFolder);
  ASSERT_TRUE(result.unsafe);
  EXPECT_TRUE(requests_.empty());
  EXPECT_FALSE(Exists(paths_.machineRoot));
  EXPECT_FALSE(StoredZoom());
  options = MakeInstallOptions(Scope::kUser);
  ASSERT_HRESULT_SUCCEEDED(InstallFromOwnFolder(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, InstallRefusal::kUnsafeFolder);
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  EXPECT_EQ(result.scope, Scope::kUser);
  ASSERT_TRUE(result.unsafe);
  EXPECT_FALSE(Exists(paths_.userRoot / kSettingsExe));
  EXPECT_FALSE(UserClassDll());
  EXPECT_FALSE(StoredZoom());
}

// ---- --fix's administrator step -------------------------------------------------------------

TEST_F(Install, FixRepairsTheClassForAllUsers) {
  const path build = MakeBuild(paths_.machineRoot, L"2.0.0.0-1a2b3c4d");
  RegisterMachineClass(paths_.machineRoot / L"app" / L"2.0.0.0-gone0000" / kHandlerDll);
  ASSERT_TRUE(WriteFileText(paths_.machineRoot / kSettingsExe, ""));
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 100));

  CheckOptions options;
  options.repair = RepairMode::kSafe;
  options.liveTest = false;
  options.context = probes_.context;
  options.paths = paths_;
  options.lockWaitMs = 5000;
  options.launcher = AnsweringLauncher(HRESULT_FROM_WIN32(ERROR_CANCELLED), 0);
  CheckResult result;
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  EXPECT_EQ(result.plan.machine, ElevatedVerb::kMachineRepairClass);
  ASSERT_TRUE(result.elevated);
  EXPECT_EQ(result.code, ExitCode::kCancelled);
  ASSERT_EQ(requests_.size(), 1u);
  EXPECT_TRUE(IEquals(requests_[0].exe.native(), (paths_.machineRoot / kSettingsExe).native()));
  EXPECT_EQ(requests_[0].parameters, L"--elevated machine-repair-class");

  // Started without administrator rights: exit 7, the class still broken.
  options.launcher = AnsweringLauncher(S_OK, static_cast<DWORD>(kNoAdministratorRightsHr));
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  ASSERT_TRUE(result.elevated);
  EXPECT_TRUE(LackedAdministratorRights(*result.elevated));
  EXPECT_EQ(result.code, ExitCode::kNeedsAdmin);
  EXPECT_TRUE(result.adminStepNeeded);
  EXPECT_TRUE(HasFinding(result.findings, FindingId::kClassDamagedMachine));

  options.launcher = [this](const ElevationRequest& request, DWORD* processId, DWORD* exitCode) {
    requests_.push_back(request);
    *processId = 0;
    *exitCode =
        static_cast<DWORD>(RepairMachineClass(roots_, paths_.machineRoot, probes_.context.osArch));
    return S_OK;
  };
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_FALSE(result.adminStepNeeded);
  EXPECT_FALSE(HasFinding(result.findings, FindingId::kClassDamagedMachine));
  EXPECT_EQ(MachineClassDll(), (build / kHandlerDll).native());

  // A quiet check never asks; without the settings executable for all users nothing can.
  RegisterMachineClass(paths_.machineRoot / L"app" / L"2.0.0.0-gone0000" / kHandlerDll);
  requests_.clear();
  options.quiet = true;
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kNeedsAdmin);
  EXPECT_TRUE(requests_.empty());
  options.quiet = false;
  ASSERT_TRUE(DeleteFileW((paths_.machineRoot / kSettingsExe).c_str()));
  ASSERT_HRESULT_SUCCEEDED(RunCheck(options, roots_, probes_, &result));
  EXPECT_TRUE(result.adminStepUnavailable);
  EXPECT_TRUE(requests_.empty());
  EXPECT_EQ(result.code, ExitCode::kProblems);
}

// ---- --uninstall ----------------------------------------------------------------------------

TEST_F(Install, UninstallRemovesEverything) {
  if (const auto why = StageUnsafe()) GTEST_SKIP() << *why;
  InstallResult installed;
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kMachine), roots_, probes_, &installed));
  ASSERT_EQ(installed.code, ExitCode::kOk);
  ASSERT_TRUE(MakeDirs(dataDir_ / L"Logs"));
  ASSERT_TRUE(WriteFileText(dataDir_ / L"Logs" / L"x.log", "log\n"));
  // Two other users signed in (their hives loaded): one with our slots and settings, one whose
  // slot names another handler; a service account's hive is never touched.
  const std::wstring slot0 = SlotKeyPath(0);
  HKEY first = Hive(
      L"S-1-5-21-"
      L"1-2-3-1001");
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(first, slot0, L"", kHandlerClsidString));
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryDword(first, kRegKey, kZoomValueName, 150));
  HKEY firstClasses = Hive(
      L"S-1-5-21-"
      L"1-2-3-1001_Classes");
  const std::wstring classesSlot =
      SlotKeyPath(1).substr(std::wstring(L"Software\\Classes\\").size());
  ASSERT_HRESULT_SUCCEEDED(
      WriteRegistryString(firstClasses, classesSlot, L"", kHandlerClsidString));
  HKEY second = Hive(
      L"S-1-5-21-"
      L"1-2-3-1002");
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(second, slot0, L"", kOtherClsid));
  HKEY service = Hive(L"S-1-5-18");
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(service, slot0, L"", kHandlerClsidString));
  requests_.clear();

  UninstallResult result;
  ASSERT_HRESULT_SUCCEEDED(Uninstall(MakeUninstallOptions(), roots_, probes_, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  EXPECT_EQ(result.code, ExitCode::kOk);
  ASSERT_EQ(requests_.size(), 1u);
  EXPECT_TRUE(IEquals(requests_[0].exe.native(), (paths_.machineRoot / kSettingsExe).native()));
  EXPECT_EQ(requests_[0].parameters, L"--elevated machine-uninstall");
  EXPECT_FALSE(result.machineLeft);
  EXPECT_FALSE(MachineClassDll());
  EXPECT_FALSE(Exists(paths_.machineRoot));
  EXPECT_FALSE(Exists(dataDir_));
  SlotValues slots;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &slots));
  EXPECT_FALSE(slots[0]);
  EXPECT_FALSE(slots[1]);
  bool settings = true;
  ASSERT_HRESULT_SUCCEEDED(RegistryKeyExists(sandbox_.user(), kRegKey, &settings));
  EXPECT_FALSE(settings);
  // The check task deleted (both triggers off), 1.x's asked for.
  EXPECT_TRUE(HasNoTriggers(probes_.applied.back().options));
  EXPECT_GE(probes_.legacyDeletes, 1);
  // The other users' hives.
  std::optional<std::wstring> value;
  ASSERT_HRESULT_SUCCEEDED(ReadRegistryString(first, slot0, L"", &value));
  EXPECT_FALSE(value);
  ASSERT_HRESULT_SUCCEEDED(RegistryKeyExists(first, kRegKey, &settings));
  EXPECT_FALSE(settings);
  ASSERT_HRESULT_SUCCEEDED(ReadRegistryString(firstClasses, classesSlot, L"", &value));
  EXPECT_FALSE(value);
  ASSERT_HRESULT_SUCCEEDED(ReadRegistryString(second, slot0, L"", &value));
  EXPECT_EQ(value, std::wstring(kOtherClsid));
  ASSERT_HRESULT_SUCCEEDED(ReadRegistryString(service, slot0, L"", &value));
  EXPECT_EQ(value, std::wstring(kHandlerClsidString));

  // Again: nothing is left to remove.
  ASSERT_HRESULT_SUCCEEDED(Uninstall(MakeUninstallOptions(), roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kNotInstalled);
}

TEST_F(Install, UninstallLeavesWhatIsInUseForTheNextSignIn) {
  if (const auto why = StageUnsafe()) GTEST_SKIP() << *why;
  InstallResult installed;
  ASSERT_HRESULT_SUCCEEDED(
      InstallFromOwnFolder(MakeInstallOptions(Scope::kUser), roots_, probes_, &installed));
  ASSERT_EQ(installed.code, ExitCode::kOk);
  // A DLL Explorer still has open, without letting anyone delete it.
  const path dll = paths_.userRoot / L"app" / BuildName() / kHandlerDll;
  wil::unique_hfile held(CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(held);
  UninstallResult result;
  ASSERT_HRESULT_SUCCEEDED(Uninstall(MakeUninstallOptions(), roots_, probes_, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  EXPECT_EQ(result.code, ExitCode::kOk);
  EXPECT_FALSE(result.machinePart);
  EXPECT_TRUE(requests_.empty());
  EXPECT_FALSE(UserClassDll());
  EXPECT_FALSE(Exists(paths_.userRoot / kSettingsExe));
  EXPECT_TRUE(Exists(dll));
  ASSERT_EQ(result.files.scheduled, std::vector<path>{paths_.userRoot});
  std::optional<std::wstring> command;
  ASSERT_HRESULT_SUCCEEDED(
      ReadRegistryString(sandbox_.user(), kRunOnceKey, RunOnceValueName(0), &command));
  ASSERT_TRUE(command);
  EXPECT_NE(command->find(L"rd /s /q \"" + paths_.userRoot.native() + L"\""), std::wstring::npos);
}

TEST_F(Install, UninstallMachinePartDeclinedOrNotPossible) {
  // 1.x for all users: no settings executable there to remove it.
  const path v1 = MakeBuild(paths_.machineRoot, L"1.1.0.0-67a1323e");
  RegisterMachineClass(v1 / kHandlerDll);
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
  UninstallResult result;
  ASSERT_HRESULT_SUCCEEDED(Uninstall(MakeUninstallOptions(), roots_, probes_, &result));
  EXPECT_TRUE(result.machineNoProgram);
  EXPECT_TRUE(result.machineLeft);
  EXPECT_EQ(result.code, ExitCode::kProblems);
  EXPECT_TRUE(requests_.empty());
  EXPECT_TRUE(Exists(v1));

  // This version for all users, the prompt declined.
  ASSERT_TRUE(WriteFileText(paths_.machineRoot / kSettingsExe, ""));
  UninstallOptions options = MakeUninstallOptions();
  options.launcher = AnsweringLauncher(HRESULT_FROM_WIN32(ERROR_CANCELLED), 0);
  ASSERT_HRESULT_SUCCEEDED(Uninstall(options, roots_, probes_, &result));
  ASSERT_TRUE(result.run);
  EXPECT_EQ(result.run->outcome, ElevatedOutcome::kCancelled);
  EXPECT_TRUE(result.machineLeft);
  EXPECT_EQ(result.code, ExitCode::kProblems);

  // Started without administrator rights: the part stays, exit 7, not listed as a failure.
  options.launcher = AnsweringLauncher(S_OK, static_cast<DWORD>(kNoAdministratorRightsHr));
  ASSERT_HRESULT_SUCCEEDED(Uninstall(options, roots_, probes_, &result));
  ASSERT_TRUE(result.run);
  EXPECT_TRUE(LackedAdministratorRights(*result.run));
  EXPECT_TRUE(result.machineLeft);
  EXPECT_TRUE(result.failures.empty());
  EXPECT_EQ(result.code, ExitCode::kNeedsAdmin);
}

TEST_F(Install, UninstallRefusalsAndTheInstallersUninstaller) {
  UninstallOptions options = MakeUninstallOptions();
  UninstallResult result;
  ASSERT_HRESULT_SUCCEEDED(Uninstall(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kNotInstalled);

  options.context->elevated = true;
  ASSERT_HRESULT_SUCCEEDED(Uninstall(options, roots_, probes_, &result));
  EXPECT_EQ(result.refusal, UninstallRefusal::kElevated);
  EXPECT_EQ(result.code, ExitCode::kWrongContext);
  options.context->elevated = false;

  // The installer's install: its uninstaller runs instead, silently with --yes.
  const std::wstring uninstaller = (path(temp_.path()) / L"unins000.exe").native();
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(sandbox_.machine(), InnoUninstallKey(),
                                               L"UninstallString", L"\"" + uninstaller + L"\""));
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
  ASSERT_HRESULT_SUCCEEDED(Uninstall(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kOk);
  ASSERT_EQ(runs_, std::vector<std::wstring>{uninstaller + L" /SILENT"});
  SlotValues slots;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &slots));
  EXPECT_TRUE(slots[0]);  // the uninstaller's to remove, not this command's
  options.yes = false;
  options.runner = [](const path&, const std::wstring&, DWORD* exitCode) {
    *exitCode = 0;
    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
  };
  ASSERT_HRESULT_SUCCEEDED(Uninstall(options, roots_, probes_, &result));
  EXPECT_EQ(result.code, ExitCode::kCancelled);
}

TEST_F(Install, UserFilesKeep1xsLicensesWhileItsManagerIsThere) {
  Plant11UserFolder(120);
  ASSERT_TRUE(MakeDirs(paths_.userRoot / L"licenses"));
  ASSERT_TRUE(WriteFileText(paths_.userRoot / L"licenses" / L"pdfium-binaries-LICENSE.txt", "x"));
  ASSERT_TRUE(WriteFileText(paths_.userRoot / kSettingsExe, ""));
  UserFilesRemoval removal;
  ASSERT_HRESULT_SUCCEEDED(RemoveUserFiles(roots_, paths_.userRoot, dataDir_, &removal));
  EXPECT_TRUE(removal.failures.empty());
  EXPECT_FALSE(Exists(paths_.userRoot / kSettingsExe));
  EXPECT_FALSE(Exists(paths_.userRoot / L"app"));
  EXPECT_TRUE(Exists(paths_.userRoot / L"licenses" / L"pdfium-binaries-LICENSE.txt"));
  EXPECT_TRUE(Exists(paths_.userRoot / L"settings.ini"));
  EXPECT_TRUE(removal.scheduled.empty());
}

TEST_F(Install, UninstallCleanupDoesTheUsersPartUnelevated) {
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 130));
  ASSERT_TRUE(MakeDirs(dataDir_));
  const path build = MakeBuild(paths_.machineRoot, L"2.0.0.0-1a2b3c4d");
  RegisterMachineClass(build / kHandlerDll);
  UninstallCleanupOptions options;
  options.paths = paths_;
  options.dataDir = dataDir_;
  options.machine.taskFolder.clear();
  options.machine.users = nullptr;
  UninstallCleanupResult result;
  ASSERT_HRESULT_SUCCEEDED(UninstallCleanup(options, roots_, probes_, &result));
  bool elevated = false;
  ASSERT_HRESULT_SUCCEEDED(GetTokenElevation(&elevated));
  EXPECT_EQ(result.machinePart, elevated);
  EXPECT_TRUE(result.userPart);
  SlotValues slots;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &slots));
  EXPECT_FALSE(slots[0]);
  EXPECT_FALSE(StoredZoom());
  // The data folder goes at the next sign-in.
  EXPECT_EQ(result.scheduled, std::vector<path>{dataDir_});
  EXPECT_TRUE(Exists(dataDir_));
  if (elevated) {
    // The part for all users ran against the test places: the class and the root are gone.
    for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
    EXPECT_HRESULT_SUCCEEDED(result.machine.firstError);
    EXPECT_FALSE(MachineClassDll());
    EXPECT_FALSE(Exists(paths_.machineRoot));
    std::printf("[ install ] --uninstall-cleanup machine part in test places (elevated): %s\n",
                result.failures.empty() && !MachineClassDll() && !Exists(paths_.machineRoot)
                    ? "removed"
                    : "NOT removed");
  } else {
    EXPECT_TRUE(MachineClassDll());
  }

  // Where writes may not reach the real hive, the per-user part is left out.
  probes_.context.virtualization = Virtualization::kYes;
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 130));
  ASSERT_HRESULT_SUCCEEDED(UninstallCleanup(options, roots_, probes_, &result));
  EXPECT_FALSE(result.userPart);
  EXPECT_EQ(StoredZoom(), 130u);
}

// ---- The status after an elevated step left files for the restart ---------------------------

TEST_F(Install, TrashFoldersAreReportedUntilTheRestart) {
  const path build = MakeBuild(paths_.machineRoot, L"2.0.0.0-1a2b3c4d");
  RegisterMachineClass(build / kHandlerDll);
  ASSERT_TRUE(WriteFileText(paths_.machineRoot / kSettingsExe, ""));
  path trash;
  ASSERT_HRESULT_SUCCEEDED(MakeTrashFolder(temp_.path(), &trash));
  ASSERT_TRUE(MakeDirs(path(temp_.path()) / (std::wstring(kProductName) + L".trash-x")));
  GatherOptions gather;
  gather.liveTest = false;
  gather.context = probes_.context;
  gather.paths = paths_;
  Snapshot snapshot;
  ASSERT_HRESULT_SUCCEEDED(GatherSnapshot(roots_, gather, probes_, &snapshot));
  EXPECT_EQ(snapshot.pendingTrash, std::vector<std::wstring>{trash.filename().native()});
  const std::vector<Finding> findings = Evaluate(snapshot);
  ASSERT_TRUE(HasFinding(findings, FindingId::kCleanupPending));
}

}  // namespace
}  // namespace pdft
