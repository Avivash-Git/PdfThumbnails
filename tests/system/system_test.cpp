// The system tests: the install, the uninstall and the 1.x clean-up for real, on a throw-away PC
// (the CI job system-x64, or a virtual machine made for it). They write HKLM and HKCU, Program
// Files, the user's install and data folders, the check task and 1.x's task, and have the
// administrator steps done through UAC. main.cpp lets them run only where they are asked for,
// elevated, and where nothing of 1.x or 2.x is installed.
//
// Each test starts the command-line executable the way a person would: as the signed-in user (a
// standard user's token at Medium integrity, made from this elevated process's own account), so
// every administrator step goes through runas; and the installer's administrator commands with
// this process's token. Each test checks that nothing is left afterwards, and a failed one is
// cleaned up after so the next starts from a clean PC.
//
// Lines starting with "[ system ]" are what the CI job looks for.

#include <windows.h>

#include <aclapi.h>
#include <gtest/gtest.h>
#include <sddl.h>
#include <wil/resource.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "build_folder.h"
#include "child_process.h"
#include "file_ops.h"
#include "instance.h"
#include "json_check.h"
#include "legacy.h"
#include "medium_il.h"
#include "migrate_v1.h"
#include "paths.h"
#include "project_config.h"
#include "registration.h"
#include "registry_roots.h"
#include "settings.h"
#include "strings.h"
#include "system_env.h"
#include "tasks.h"
#include "temp_dir.h"
#include "zoom.h"

namespace pdft::systest {
namespace {

using std::filesystem::path;

// A 1.1 build folder's name (1.1's real build id) and the 1.x files the fixture plants with their
// shipped contents.
constexpr wchar_t kV1BuildName[] = L"1.1.0.0-67a1323e";
constexpr const wchar_t* kV1ShippedFiles[] = {L"PdfThumbnails.ps1", L"PdfThumbnails.cmd",
                                              L"LICENSE"};

// The findings the runner may show without the install being at fault: the Explorer test needs a
// thumbnail surrogate that a CI runner without a signed-in desktop may not start.
constexpr std::string_view kExplorerTestFindings[] = {"LiveTestFailed", "LiveTestSkipped"};

bool Exists(const path& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string Text(std::wstring_view text) { return WideToUtf8Lossy(text); }

std::string Lines(const std::vector<std::string>& lines) {
  std::string text;
  for (const std::string& line : lines) text += "\n  " + line;
  return text;
}

bool WriteFileText(const path& file, const std::string& text) {
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
  DWORD written = 0;
  return handle &&
         WriteFile(handle.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
         written == text.size();
}

bool MakeDirs(const path& p) {
  std::error_code error;
  std::filesystem::create_directories(p, error);
  return !error;
}

// Gives |folder| a protected DACL that lets only this user, SYSTEM and Administrators change it
// (and, inherited, what is put in it later): what the install's permission check asks of a
// build's folder.
bool ProtectFolder(const path& folder) {
  std::wstring sid;
  if (FAILED(GetCurrentUserSid(&sid))) return false;
  const std::wstring sddl = L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
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

// The member at |keys| below |root|; nullptr when one is missing.
const test::JsonValue* At(const test::JsonValue& root,
                          std::initializer_list<std::string_view> keys) {
  const test::JsonValue* value = &root;
  for (std::string_view key : keys) {
    value = value->Get(key);
    if (!value) return nullptr;
  }
  return value;
}

std::string StringAt(const test::JsonValue& root, std::initializer_list<std::string_view> keys) {
  const test::JsonValue* value = At(root, keys);
  return value && value->type == test::JsonValue::Type::kString ? value->text : std::string();
}

bool BoolAt(const test::JsonValue& root, std::initializer_list<std::string_view> keys) {
  const test::JsonValue* value = At(root, keys);
  return value && value->type == test::JsonValue::Type::kBool && value->boolean;
}

std::optional<double> NumberAt(const test::JsonValue& root,
                               std::initializer_list<std::string_view> keys) {
  const test::JsonValue* value = At(root, keys);
  if (!value || value->type != test::JsonValue::Type::kNumber) return std::nullopt;
  return value->number;
}

struct Finding {
  std::string id;
  std::string text;
};

// --status --json, read as the signed-in user.
struct Status {
  bool parsed = false;
  test::JsonValue json;
  std::vector<Finding> breaking;  // findings that keep thumbnails from working
  std::vector<std::string> ids;   // every finding
};

class System : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.ok());
    ASSERT_HRESULT_SUCCEEDED(GetMachineRoot(&machineRoot_));
    ASSERT_HRESULT_SUCCEEDED(GetUserRoot(&userRoot_));
    ASSERT_HRESULT_SUCCEEDED(GetDataDir(&dataDir_));
    ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid_));
    const std::vector<std::string> left = Leftovers();
    ASSERT_TRUE(left.empty()) << "not a clean start:" << Lines(left);

    // The staged runtime, copied into a folder only this user and administrators can change.
    const std::wstring runtime = EnvironmentValue(kRuntimeEnv);
    ASSERT_FALSE(runtime.empty())
        << Text(kRuntimeEnv)
        << " must name the staged runtime (cmake --install <build> --prefix <folder> "
           "--component runtime)";
    stage_ = path(temp_.path()) / L"runtime";
    ASSERT_TRUE(MakeDirs(stage_));
    ASSERT_TRUE(ProtectFolder(stage_));
    std::error_code error;
    std::filesystem::copy(runtime, stage_, std::filesystem::copy_options::recursive, error);
    ASSERT_FALSE(error) << error.message();
    const std::vector<std::wstring> binaries = {kSettingsExe, kCliExe, kHandlerDll, kPdfiumDll};
    std::optional<UnsafeAccess> unsafe;
    ASSERT_HRESULT_SUCCEEDED(FindUnsafeBuildFolderAccess(stage_, binaries, &unsafe));
    ASSERT_FALSE(unsafe) << "other accounts could change " << Text(unsafe->object.native()) << " ("
                         << Text(unsafe->access.principal) << "), so --install would refuse";
    ASSERT_HRESULT_SUCCEEDED(
        ComputeBuildId(stage_ / kHandlerDll, stage_ / kPdfiumDll, &buildName_));
  }

  void TearDown() override {
    const std::vector<std::string> left = Leftovers();
    if (!left.empty()) {
      std::printf("[ system ] cleaning up what the test left:%s\n", Lines(left).c_str());
      ForceClean();
    }
    for (const path& trash : TrashFolders()) {
      if (FAILED(RemoveTreeNoFollow(trash))) {
        std::printf("[ system ] %s stays until the restart\n", Text(trash.native()).c_str());
      }
    }
  }

  path Cli() const { return stage_ / kCliExe; }
  path InstalledCli() const { return machineRoot_ / kCliExe; }
  path BuildDll() const { return machineRoot_ / L"app" / buildName_ / kHandlerDll; }

  static void Print(const char* who, const path& exe, std::wstring_view arguments,
                    const Outcome& run) {
    std::printf("[ system ] %s %s (%s) -> exit %lu", Text(exe.filename().native()).c_str(),
                Text(arguments).c_str(), who, run.exitCode);
    if (FAILED(run.hr)) std::printf(", not run: %s", Text(HResultText(run.hr)).c_str());
    std::printf("\n");
    std::istringstream lines(run.Output());
    for (std::string line; std::getline(lines, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::printf("    %s\n", line.c_str());
    }
  }

  Outcome User(const path& exe, std::wstring_view arguments) {
    const Outcome run = RunAsUser(exe, std::wstring(arguments));
    Print("user", exe, arguments, run);
    EXPECT_HRESULT_SUCCEEDED(run.hr) << "starting " << Text(exe.native());
    return run;
  }

  Outcome Admin(const path& exe, std::wstring_view arguments) {
    const Outcome run = RunAsAdministrator(exe, std::wstring(arguments));
    Print("administrator", exe, arguments, run);
    EXPECT_HRESULT_SUCCEEDED(run.hr) << "starting " << Text(exe.native());
    return run;
  }

  // ---- The real state ----

  static std::optional<std::wstring> ClassDll(Scope scope) {
    std::optional<ClassReg> reg;
    EXPECT_HRESULT_SUCCEEDED(ReadClass(RegistryRoots{}, scope, &reg));
    if (!reg) return std::nullopt;
    return reg->dll;
  }

  static std::optional<DWORD> StoredZoom() {
    std::optional<RegistryValue> value;
    if (FAILED(ReadRegistryValue(HKEY_CURRENT_USER, kRegKey, kZoomValueName, &value)) || !value) {
      return std::nullopt;
    }
    return RegistryDword(*value);
  }

  static std::vector<std::wstring> RunOnceValues() {
    std::vector<std::wstring> names;
    EXPECT_HRESULT_SUCCEEDED(FindRunOnceRemovals(RegistryRoots{}, &names));
    return names;
  }

  static bool LegacyTask() {
    bool exists = false;
    EXPECT_HRESULT_SUCCEEDED(LegacyTaskExists(&exists));
    return exists;
  }

  TaskState UserTask() const {
    TaskLocation location;
    TaskState state;
    EXPECT_HRESULT_SUCCEEDED(DefaultTaskLocation(sid_, &location));
    EXPECT_HRESULT_SUCCEEDED(ReadTask(location, &state));
    return state;
  }

  static bool SlotsOurs() {
    SlotValues slots;
    if (FAILED(ReadSlots(RegistryRoots{}, &slots))) return false;
    return std::all_of(slots.begin(), slots.end(),
                       [](const auto& slot) { return slot && IsOurClsid(*slot); });
  }

  static bool MigratedFromV1() {
    bool migrated = false;
    EXPECT_HRESULT_SUCCEEDED(ReadMigratedFromV1(RegistryRoots{}, &migrated));
    return migrated;
  }

  // ---- Expectations ----

  // What a machine-wide install leaves: the files, the class for all users pointing at this
  // build, this user's slots and check task (in the task folder, running the installed settings
  // executable), no per-user class and no RunOnce clean-up.
  void ExpectInstalledForAllUsers() {
    for (const wchar_t* name : {kSettingsExe, kCliExe}) {
      EXPECT_TRUE(Exists(machineRoot_ / name)) << Text(name);
    }
    EXPECT_TRUE(Exists(BuildDll()));
    EXPECT_TRUE(Exists(BuildDll().parent_path() / kPdfiumDll));
    const std::optional<std::wstring> machine = ClassDll(Scope::kMachine);
    ASSERT_TRUE(machine.has_value());
    EXPECT_TRUE(IEquals(*machine, BuildDll().native())) << Text(*machine);
    EXPECT_FALSE(ClassDll(Scope::kUser).has_value());
    EXPECT_TRUE(SlotsOurs());
    EXPECT_EQ(RunOnceValues(), std::vector<std::wstring>{});
    EXPECT_EQ(TaskFolderExists(kTaskFolder), std::optional<bool>(true));
    ExpectTask(true, true);
  }

  void ExpectTask(bool afterPdfCraft, bool atSignIn) {
    const TaskState task = UserTask();
    ASSERT_TRUE(task.exists);
    std::printf("[ system ] check task %s: after PdfCraft %d, at sign-in %d\n",
                Text(task.path).c_str(), task.afterPdfCraft, task.atSignIn);
    EXPECT_FALSE(task.atFallback) << "the task is not in the task folder";
    EXPECT_TRUE(task.enabled);
    EXPECT_EQ(task.afterPdfCraft, afterPdfCraft);
    EXPECT_EQ(task.atSignIn, atSignIn);
    EXPECT_TRUE(IEquals(task.actionPath, (machineRoot_ / kSettingsExe).native()))
        << Text(task.actionPath);
    EXPECT_EQ(task.actionArgs, kCheckTaskArguments);
  }

  Status ReadStatus() {
    Status status;
    const Outcome run = User(InstalledCli(), L"--status --json");
    std::string error;
    status.parsed = test::ParseJson(run.out, &status.json, &error);
    EXPECT_TRUE(status.parsed) << error << "\n" << run.out;
    if (!status.parsed) return status;
    EXPECT_EQ(NumberAt(status.json, {"schema"}), std::optional<double>(1));
    if (const test::JsonValue* findings = status.json.Get("findings")) {
      for (const test::JsonValue& finding : findings->items) {
        const std::string id = StringAt(finding, {"id"});
        status.ids.push_back(id);
        if (BoolAt(finding, {"breaks"}))
          status.breaking.push_back({id, StringAt(finding, {"text"})});
      }
    }
    std::printf("[ system ] status: healthy %d, Explorer test ran %d hr %s\n",
                BoolAt(status.json, {"healthy"}), BoolAt(status.json, {"liveTest", "ran"}),
                StringAt(status.json, {"liveTest", "hr"}).c_str());
    return status;
  }

  // The status is healthy, or only the Explorer test keeps it from being so; |command| exited
  // 0 when healthy and 1 otherwise.
  void ExpectHealthy(const Outcome& command, const Status& status) {
    std::vector<std::string> others;
    for (const Finding& finding : status.breaking) {
      const bool explorerTest =
          std::find(std::begin(kExplorerTestFindings), std::end(kExplorerTestFindings),
                    finding.id) != std::end(kExplorerTestFindings);
      std::printf("[ system ] not healthy: %s: %s%s\n", finding.id.c_str(), finding.text.c_str(),
                  explorerTest ? " (the Explorer test: informational on this runner)" : "");
      if (!explorerTest) others.push_back(finding.id + ": " + finding.text);
    }
    EXPECT_TRUE(others.empty()) << Lines(others);
    if (status.breaking.empty()) {
      EXPECT_EQ(command.exitCode, 0u) << command.Output();
    } else {
      EXPECT_EQ(command.exitCode, 1u) << command.Output();
    }
  }

  void ExpectStatusForAllUsers(const Status& status, DWORD zoom) {
    if (!status.parsed) return;
    EXPECT_EQ(StringAt(status.json, {"install", "scope"}), "machine");
    EXPECT_TRUE(
        IEquals(Utf8(StringAt(status.json, {"classes", "machine", "dll"})), BuildDll().native()));
    EXPECT_EQ(NumberAt(status.json, {"zoom", "value"}), std::optional<double>(zoom));
    EXPECT_EQ(StringAt(status.json, {"zoom", "origin"}), "registry");
    EXPECT_FALSE(BoolAt(status.json, {"context", "elevated"}));
    EXPECT_TRUE(BoolAt(status.json, {"checks", "afterPdfCraft"}));
    EXPECT_TRUE(BoolAt(status.json, {"checks", "atSignIn"}));
    EXPECT_EQ(std::count(status.ids.begin(), status.ids.end(), "V1Leftovers"), 0);
  }

  static std::wstring Utf8(const std::string& text) {
    std::wstring wide;
    EXPECT_HRESULT_SUCCEEDED(Utf8ToWide(text, &wide));
    return wide;
  }

  // The administrator step |verb| reported back through the asking process's pipe: its "start"
  // line reached the log of the unelevated process, and the summary line after it does not say
  // "no-report". That pipe takes lines only from the elevated process if its parent is the
  // pipe's server, so this proves that UAC makes the asking process the parent. The start line
  // is printed too: it tells what token UAC gave the step.
  void ExpectReportThroughThePipe(std::wstring_view verb) {
    path log;
    ASSERT_HRESULT_SUCCEEDED(GetLogPath(&log));
    std::ifstream file(log, std::ios::binary);
    ASSERT_TRUE(file.good()) << Text(log.native());
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const std::string name = Text(verb);
    const size_t start = text.rfind("elevated " + name + " start");
    if (start != std::string::npos) {
      std::string line = text.substr(start, text.find('\n', start) - start);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      std::printf("[ system ] log: %s\n", line.c_str());
    }
    const size_t summary = text.rfind(name + " launch=");
    bool reported = false;
    if (summary != std::string::npos) {
      const size_t end = text.find('\n', summary);
      const std::string line = text.substr(summary, end == std::string::npos ? end : end - summary);
      std::printf("[ system ] log: %s\n", line.c_str());
      reported = start != std::string::npos && start < summary &&
                 line.find(" no-report") == std::string::npos;
    }
    std::printf("[ system ] %s reported through the asking process's pipe: %s\n", name.c_str(),
                reported ? "yes" : "no");
    EXPECT_TRUE(reported) << "the elevated step's report did not reach the unelevated process";
  }

  // Nothing of the product is left; trash folders only where they are queued for deletion at the
  // restart, and they go now that nothing runs from them.
  void ExpectNothingLeft() {
    const std::vector<std::string> left = Leftovers();
    EXPECT_TRUE(left.empty()) << Lines(left);
    for (const path& trash : TrashFolders()) {
      const bool queued = QueuedForRestart(trash);
      const bool removed = SUCCEEDED(RemoveTreeNoFollow(trash));
      std::printf("[ system ] trash folder %s: queued for the restart %s, removed now %s\n",
                  Text(trash.filename().native()).c_str(), queued ? "yes" : "no",
                  removed ? "yes" : "no");
      EXPECT_TRUE(queued) << Text(trash.native());
    }
    if (left.empty()) std::printf("[ system ] nothing left\n");
  }

  // ---- 1.1's state, planted ----

  static path FixtureFolder() {
    const std::wstring folder = EnvironmentValue(kFixtureEnv);
    return folder.empty() ? path(test::SelfFolder()) / L"system-fixture" : path(folder);
  }

  // A 1.1 build folder under |root| with stand-ins for its DLLs (text, so nothing could ever load
  // them); returns the handler's path.
  static path PlantV1Build(const path& root) {
    const path dir = root / L"app" / kV1BuildName;
    EXPECT_TRUE(MakeDirs(dir));
    EXPECT_TRUE(WriteFileText(dir / kHandlerDll, "1.x stand-in"));
    EXPECT_TRUE(
        WriteFileText(dir / (std::wstring(kLegacyRootName) + L"-pdfium.dll"), "1.x stand-in"));
    return dir / kHandlerDll;
  }

  static void PlantClass(HKEY root, const path& dll) {
    const std::wstring inproc = ClassKeyPath() + L"\\InprocServer32";
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(root, ClassKeyPath(), L"", kHandlerClassName));
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(root, inproc, L"", dll.native()));
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(root, inproc, L"ThreadingModel", L"Apartment"));
  }

  // 1.x's per-user folder as 1.1 leaves it: its manager, launcher and license with their shipped
  // contents, settings.ini with |zoom|, its repair log and an empty app folder.
  void PlantV1UserFolder(int zoom) {
    ASSERT_TRUE(MakeDirs(userRoot_ / L"app"));
    for (const wchar_t* name : kV1ShippedFiles) {
      const path from = FixtureFolder() / name;
      ASSERT_TRUE(CopyFileW(from.c_str(), (userRoot_ / name).c_str(), TRUE)) << Text(from.native());
      Sha256Digest digest = {};
      ASSERT_HRESULT_SUCCEEDED(Sha256File(userRoot_ / name, &digest));
      const std::vector<Sha256Digest> shipped = V1ShippedDigests(name);
      ASSERT_NE(std::find(shipped.begin(), shipped.end(), digest), shipped.end())
          << Text(name) << " in the fixture is not 1.x's";
    }
    ASSERT_TRUE(WriteFileText(userRoot_ / L"settings.ini",
                              "[Thumbnails]\r\nZoom=" + std::to_string(zoom) + "\r\n"));
    ASSERT_TRUE(WriteFileText(userRoot_ / L"repair.log", "repaired\r\n"));
  }

  // 1.x's sign-in clean-up of its per-user folder, and a second value of the family.
  void PlantRunOnce() {
    const std::wstring command = L"cmd.exe /d /c rd /s /q \"" + userRoot_.native() + L"\"";
    ASSERT_HRESULT_SUCCEEDED(
        WriteRegistryString(HKEY_CURRENT_USER, kRunOnceKey, kLegacyRunOnce, command));
    ASSERT_HRESULT_SUCCEEDED(
        WriteRegistryString(HKEY_CURRENT_USER, kRunOnceKey, RunOnceValueName(1), command));
  }

  // 1.x's task, registered by the user as 1.1 did (a daily trigger instead of 1.1's sign-in and
  // event triggers: only its name matters here).
  void PlantLegacyTask() {
    wchar_t system[MAX_PATH] = {};
    ASSERT_NE(GetSystemDirectoryW(system, ARRAYSIZE(system)), 0u);
    const path schtasks = path(system) / L"schtasks.exe";
    const std::wstring name = std::wstring(kLegacyTask).substr(1);  // without the root's "\"
    const std::wstring action = (path(system) / L"cmd.exe").native() + L" /d /c exit 0";
    const Outcome run =
        User(schtasks, L"/Create /TN " + test::QuoteArgument(name) + L" /TR " +
                           test::QuoteArgument(action) + L" /SC DAILY /ST 03:00 /F");
    ASSERT_EQ(run.exitCode, 0u) << run.Output();
    ASSERT_TRUE(LegacyTask());
  }

  // What the 1.x clean-up leaves in the per-user folder: none of 1.x's files.
  void ExpectV1UserFilesGone() {
    for (const wchar_t* name : kV1ShippedFiles) {
      EXPECT_FALSE(Exists(userRoot_ / name)) << Text(name);
    }
    for (const wchar_t* name : {L"settings.ini", L"repair.log"}) {
      EXPECT_FALSE(Exists(userRoot_ / name)) << Text(name);
    }
    EXPECT_FALSE(Exists(userRoot_ / L"app" / kV1BuildName));
    EXPECT_FALSE(LegacyTask());
    EXPECT_EQ(RunOnceValues(), std::vector<std::wstring>{});
    EXPECT_TRUE(MigratedFromV1());
  }

  test::TempDir temp_{L"system"};
  path stage_;
  path machineRoot_;
  path userRoot_;
  path dataDir_;
  std::wstring sid_;
  std::wstring buildName_;
};

// ---- Installed for all users, used, uninstalled --------------------------------------------

TEST_F(System, InstallChecksZoomFixAndUninstall) {
  const Outcome install = User(Cli(), L"--install --machine");
  ExpectInstalledForAllUsers();
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(100));
  const Status installed = ReadStatus();
  ExpectHealthy(install, installed);
  ExpectStatusForAllUsers(installed, 100);
  ExpectReportThroughThePipe(L"machine-install");

  // The scheduled checks off and on again.
  EXPECT_EQ(User(InstalledCli(), L"--checks none").exitCode, 0u);
  EXPECT_FALSE(UserTask().exists);
  EXPECT_EQ(User(InstalledCli(), L"--checks default").exitCode, 0u);
  ExpectTask(true, true);

  // The zoom saved without a refresh, and shown.
  EXPECT_EQ(User(InstalledCli(), L"--zoom 130 --no-refresh").exitCode, 0u);
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(130));
  const Outcome zoom = User(InstalledCli(), L"--zoom");
  EXPECT_EQ(zoom.exitCode, 0u);
  EXPECT_NE(zoom.out.find("130 %"), std::string::npos) << zoom.out;

  // The installer's steps run again: the per-user one quietly keeps the user's choices and
  // refuses an administrator; the one for all users needs an administrator.
  const Outcome setupUser = User(InstalledCli(), L"--setup-user");
  EXPECT_EQ(setupUser.exitCode, 0u);
  EXPECT_EQ(setupUser.Output(), "");
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(130));
  ExpectTask(true, true);
  EXPECT_EQ(Admin(InstalledCli(), L"--setup-user").exitCode, 8u);
  EXPECT_EQ(User(InstalledCli(), L"--setup-machine").exitCode, 7u);
  EXPECT_EQ(Admin(InstalledCli(), L"--setup-machine").exitCode, 0u);
  EXPECT_EQ(TaskFolderExists(kTaskFolder), std::optional<bool>(true));

  // The class for all users points at a build that is not there: --fix has the administrator
  // step point it at the newest complete build, through UAC.
  const path missing = machineRoot_ / L"app" / L"2.0.0.0-00000000" / kHandlerDll;
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(
      HKEY_LOCAL_MACHINE, ClassKeyPath() + L"\\InprocServer32", L"", missing.native()));
  const Outcome fix = User(InstalledCli(), L"--fix");
  const std::optional<std::wstring> repaired = ClassDll(Scope::kMachine);
  ASSERT_TRUE(repaired.has_value());
  EXPECT_TRUE(IEquals(*repaired, BuildDll().native())) << Text(*repaired);
  ExpectHealthy(fix, ReadStatus());
  ExpectReportThroughThePipe(L"machine-repair-class");

  // The uninstall, from the installed executable: the running programs go to a trash folder
  // that is deleted at the restart.
  const Outcome uninstall = User(InstalledCli(), L"--uninstall --yes");
  EXPECT_EQ(uninstall.exitCode, 0u);
  EXPECT_FALSE(StoredZoom().has_value());
  ExpectNothingLeft();
}

// ---- 1.1 found by the install ------------------------------------------------------------

// 1.1 installed for all users, with its per-user files, settings, task and sign-in clean-up:
// --install --machine replaces it.
TEST_F(System, InstallReplaces11ForAllUsers) {
  const path v1 = PlantV1Build(machineRoot_);
  PlantClass(HKEY_LOCAL_MACHINE, v1);
  for (size_t i = 0; i < kSlotCount; ++i) {
    ASSERT_HRESULT_SUCCEEDED(
        WriteRegistryString(HKEY_CURRENT_USER, SlotKeyPath(i), L"", kHandlerClsidString));
  }
  PlantV1UserFolder(120);
  PlantRunOnce();
  PlantLegacyTask();

  const Outcome install = User(Cli(), L"--install --machine");
  ExpectInstalledForAllUsers();
  EXPECT_FALSE(Exists(v1.parent_path()));
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(120));
  ExpectV1UserFilesGone();
  const Status status = ReadStatus();
  ExpectHealthy(install, status);
  ExpectStatusForAllUsers(status, 120);

  // The clean-up again finds nothing to do.
  EXPECT_EQ(User(InstalledCli(), L"--migrate-v1").exitCode, 0u);

  EXPECT_EQ(User(InstalledCli(), L"--uninstall --yes").exitCode, 0u);
  ExpectNothingLeft();
}

// 1.1 installed for this user only (its class in HKCU): --install --machine takes over, and the
// per-user class and build go.
TEST_F(System, InstallReplaces11ForThisUser) {
  const path v1 = PlantV1Build(userRoot_);
  PlantClass(HKEY_CURRENT_USER, v1);
  for (size_t i = 0; i < kSlotCount; ++i) {
    ASSERT_HRESULT_SUCCEEDED(
        WriteRegistryString(HKEY_CURRENT_USER, SlotKeyPath(i), L"", kHandlerClsidString));
  }
  PlantV1UserFolder(120);
  PlantRunOnce();
  PlantLegacyTask();

  const Outcome install = User(Cli(), L"--install --machine");
  ExpectInstalledForAllUsers();
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(120));
  ExpectV1UserFilesGone();
  const Status status = ReadStatus();
  ExpectHealthy(install, status);
  ExpectStatusForAllUsers(status, 120);

  EXPECT_EQ(User(InstalledCli(), L"--uninstall --yes").exitCode, 0u);
  ExpectNothingLeft();
}

// ---- --migrate-v1 after the install ------------------------------------------------------

// 1.1's leftovers next to an installed 2.0: --migrate-v1 removes the per-user ones and has the
// administrator step remove 1.1's build folder for all users, through UAC, by the installed
// settings executable.
TEST_F(System, MigrateV1RemovesWhat11Left) {
  User(Cli(), L"--install --machine");
  ExpectInstalledForAllUsers();
  const path v1 = PlantV1Build(machineRoot_);
  PlantV1UserFolder(120);
  PlantRunOnce();
  PlantLegacyTask();

  const Outcome migrate = User(InstalledCli(), L"--migrate-v1");
  EXPECT_EQ(migrate.exitCode, 0u);
  EXPECT_FALSE(Exists(v1.parent_path()));
  // The zoom the install wrote stays: 1.x's is taken only where there is none.
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(100));
  ExpectV1UserFilesGone();
  ExpectReportThroughThePipe(L"machine-migrate-v1");
  ExpectInstalledForAllUsers();

  EXPECT_EQ(User(InstalledCli(), L"--uninstall --yes").exitCode, 0u);
  ExpectNothingLeft();
}

// ---- The installer's uninstall step ------------------------------------------------------

// --uninstall-cleanup, as an installer's uninstaller runs it (elevated): the part for all users
// and the loaded user hives, exit 0; the data folder is the signed-in user's, left to their own
// uninstall or a sign-in clean-up.
TEST_F(System, UninstallCleanupAsTheInstallersUninstaller) {
  User(Cli(), L"--install --machine");
  ExpectInstalledForAllUsers();

  EXPECT_EQ(Admin(InstalledCli(), L"--uninstall-cleanup").exitCode, 0u);
  EXPECT_FALSE(ClassDll(Scope::kMachine).has_value());
  EXPECT_EQ(TaskFolderExists(kTaskFolder), std::optional<bool>(false));
  EXPECT_FALSE(UserTask().exists);
  EXPECT_FALSE(Exists(machineRoot_));
  SlotValues slots;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(RegistryRoots{}, &slots));
  for (const auto& slot : slots) EXPECT_FALSE(slot && IsOurClsid(*slot));
  bool settings = true;
  ASSERT_HRESULT_SUCCEEDED(RegistryKeyExists(HKEY_CURRENT_USER, kRegKey, &settings));
  EXPECT_FALSE(settings);

  // What is left for the user: the data folder, removed by a sign-in clean-up or their own
  // --uninstall; the sign-in clean-up is not left for the next sign-in of the runner.
  for (const std::wstring& name : RunOnceValues()) {
    std::optional<std::wstring> command;
    EXPECT_HRESULT_SUCCEEDED(ReadRegistryString(HKEY_CURRENT_USER, kRunOnceKey, name, &command));
    std::printf("[ system ] sign-in clean-up %s: %s\n", Text(name).c_str(),
                Text(command.value_or(L"")).c_str());
  }
  ASSERT_HRESULT_SUCCEEDED(DeleteRunOnceRemovals(RegistryRoots{}));
  if (Exists(dataDir_)) {
    const Outcome uninstall = User(Cli(), L"--uninstall --yes");
    EXPECT_TRUE(uninstall.exitCode == 0 || uninstall.exitCode == 6) << uninstall.Output();
  }
  ExpectNothingLeft();
}

}  // namespace
}  // namespace pdft::systest
