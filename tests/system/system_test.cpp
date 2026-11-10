// The system tests: the install, the uninstall and the 1.x clean-up for real, on a throw-away PC
// (the CI job system-x64, or a virtual machine made for it). They write HKLM and the user's HKCU,
// Program Files, the user's install and data folders, the check task and 1.x's task, and have the
// administrator steps done through UAC. main.cpp lets them run only where they are asked for,
// elevated, and where nothing of 1.x or 2.x is installed.
//
// Each test starts the command-line executable the way a person would: as the signed-in user,
// who is the test account (test_account.h: an ordinary member of Administrators, with the limited
// token of its interactive logon), so every administrator step goes through UAC into the
// account's linked token; and the installer's administrator commands with this process's own
// elevated token (another account than the user's, as an administrator who runs an installer may
// be). What the tests check about the user (HKCU values, the slots, RunOnce, the per-user and
// data folders, the check task) is the test account's: its loaded hive, its profile, its SID.
// Each test checks that nothing is left afterwards, and a failed one is cleaned up after so the
// next starts from a clean PC. If UAC does not elevate the account's step, the test that asked
// says so with the step's own token line, and the tests after it stop at once.
//
// A second user, the standard account (test_account.h: not a member of Administrators), runs the
// integration tests' task tests before anything is installed, and its own --setup-user once the
// install for all users has made the protected task folder; its state is checked like the test
// account's.
//
// With the install for all users in place, the test account also runs the thumbnail refresh
// experiment's automatic part (thumbtest experiment --non-interactive) on its own thumbnail cache,
// has --refresh --full refused while File Explorer on the desktop is another account's (the
// runner's), and asks its cache what became of two thumbnails once the uninstall unregistered the
// handler. The experiment's verdict and that answer are printed, not judged: they are this
// runner's, and a PC of the maintainer's decides.
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
#include <utility>
#include <vector>

#include "account_rules.h"
#include "build_folder.h"
#include "child_process.h"
#include "file_ops.h"
#include "json_check.h"
#include "legacy.h"
#include "medium_il.h"
#include "migrate_v1.h"
#include "paths.h"
#include "project_config.h"
#include "rebuild.h"
#include "refresh_judge.h"
#include "registration.h"
#include "registry_roots.h"
#include "settings.h"
#include "strings.h"
#include "system_env.h"
#include "tasks.h"
#include "test_account.h"
#include "test_pdfs.h"
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

// The integration tests, next to this program (in the build folder and in the test bundle): the
// standard account runs their task tests.
constexpr wchar_t kIntegrationTestsExe[] = L"pdft_integration_tests.exe";

// thumbtest, next to this program too: the test account runs it for the thumbnail refresh.
constexpr wchar_t kThumbtestExe[] = L"thumbtest.exe";

// The refresh experiment's automatic part fills the cache at every size, so it gets longer than
// one command (at its limit the runner ends it, and its report is lost); its timing folder and
// the files made again each in a thumbnail process of their own (where that pass runs at all:
// the experiment leaves it out while the product's refresh refuses its flags) are kept few, since
// this runner's timings decide nothing.
constexpr DWORD kExperimentTimeoutMs = 30 * 60 * 1000;
constexpr int kExperimentTimingFiles = 200;
constexpr int kExperimentInstanceFiles = 40;

// The starts of the experiment's lines that are repeated as "[ system ] refresh experiment:
// <line>".
constexpr std::string_view kExperimentLines[] = {
    // The fill, the baseline and the check that the thumbnail process sees the new zoom.
    "size ", "fill ", "cache files", "baseline", "not held:", "asked again", "VALID ", "EXCLUDED ",
    "files not held", "untouched ", "sanity check", "elapsed ",
    // How the cache answers other flags.
    "flags 0x", "the old question", "WTS_IDEALCACHESIZEONLY", "which size answered",
    "scaled to the size asked", "the product's extraction flags", "stored at its file's size",
    "thumbnail IDs",
    // The verdicts per folder, and what they decide.
    "PASS ", "FAIL ", "HELD ", "CHANGED ", "INVALID ", "UNDECIDED ", "Pass A", "No Pass A",
    "dropped since", "dropped whole", "round T-touch after",
    // The timing folder filled after them.
    "timing fill", "control after", "timing files held",
    // Why it stopped, and what it put back.
    "STOPPED:", "zoom: ", "File Explorer not restarted:"};

// The experiment's report is printed after its lines, up to this many lines and bytes.
constexpr size_t kMaxReportLines = 600;
constexpr DWORD kMaxReportBytes = 1024 * 1024;

// The eviction question asks the cache about these two files at one size the refresh makes again.
constexpr UINT kEvictionSize = 256;
constexpr const wchar_t* kEvictionFiles[] = {L"01-portrait.pdf", L"02-landscape.pdf"};

// Set once UAC did not elevate the test account's step: every test needs that, so the ones after
// stop at once instead of failing for reasons that follow from it.
bool g_uacDidNotElevate = false;

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

// Whether a link, a junction or a mount point is at |p| itself (a reparse tag that names another
// place), not looking at where it leads. One whose tag cannot be read counts as one.
bool LeadsElsewhere(const path& p) {
  const wil::unique_hfile handle(CreateFileW(
      p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  FILE_ATTRIBUTE_TAG_INFO info = {};
  if (!handle ||
      !GetFileInformationByHandleEx(handle.get(), FileAttributeTagInfo, &info, sizeof(info))) {
    return true;
  }
  return (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
         IsReparseTagNameSurrogate(info.ReparseTag) != 0;
}

// Why the staged runtime |runtime| must not be copied with this process's administrator rights;
// empty when it may. The copy reads what is there with those rights, so the folder must be on a
// local drive (a share, or a drive letter mapped to one, is served by another computer), and no
// link, junction or mount point may send it somewhere else: not on the way to the folder, not
// the folder itself, not anything in it. Whoever can change the folder can still change what is
// installed, so nobody but administrators should be able to (system_env.h; not checked here).
std::string RuntimeSourceProblem(const path& runtime) {
  const path source = runtime.lexically_normal();
  const std::wstring& text = source.native();
  const bool drive = text.size() > 3 &&
                     ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z'));
  if (!drive || text[1] != L':' || text[2] != L'\\') {
    return "is not a folder on a drive (X:\\...); shares and \\\\?\\ paths are refused";
  }
  const UINT type = GetDriveTypeW(text.substr(0, 3).c_str());
  if (type == DRIVE_REMOTE || type == DRIVE_UNKNOWN || type == DRIVE_NO_ROOT_DIR) {
    return "is not on a local drive (drive type " + std::to_string(type) + ")";
  }
  for (path p = source; p.has_relative_path(); p = p.parent_path()) {
    if (LeadsElsewhere(p)) {
      return "leads through a link, junction or mount point: " + Text(p.native());
    }
  }
  // The walk lists links but never enters them.
  std::error_code error;
  const std::filesystem::recursive_directory_iterator end;
  for (std::filesystem::recursive_directory_iterator entry(source, error); !error && entry != end;
       entry.increment(error)) {
    if (LeadsElsewhere(entry->path())) {
      return "holds a link, junction or mount point: " + Text(entry->path().native());
    }
  }
  if (error) return "cannot be listed: " + error.message();
  return {};
}

// Gives |folder| a protected DACL that lets only the accounts |sids|, SYSTEM and Administrators
// change it (and, inherited, what is put in it later): what the install's permission check asks
// of a build's folder.
bool ProtectFolder(const path& folder, std::initializer_list<std::wstring_view> sids) {
  std::wstring sddl = L"D:P";
  for (const std::wstring_view sid : sids) sddl += L"(A;OICI;FA;;;" + std::wstring(sid) + L")";
  sddl += L"(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)";
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

// "[ system ] <exe> <arguments> (<who>) -> exit <code>", then the program's output, indented.
void Print(const char* who, const path& exe, std::wstring_view arguments, const Outcome& run) {
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

// The line of |text| that starts at |at|, without its line end.
std::string LineAt(const std::string& text, size_t at) {
  const size_t end = text.find('\n', at);
  std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
  if (!line.empty() && line.back() == '\r') line.pop_back();
  return line;
}

// The lines of a program's output |text|, without their line ends.
std::vector<std::string> OutputLines(const std::string& text) {
  std::vector<std::string> lines;
  std::istringstream stream(text);
  for (std::string line; std::getline(stream, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
  }
  return lines;
}

// The SID of the account process |pid| runs as ("S-1-5-..."); empty when it cannot be read.
std::wstring ProcessUserSid(DWORD pid) {
  const wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
  wil::unique_handle token;
  if (!process || !OpenProcessToken(process.get(), TOKEN_QUERY, token.put())) return {};
  alignas(TOKEN_USER) BYTE user[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE];
  DWORD size = 0;
  wil::unique_hlocal_string text;
  if (!GetTokenInformation(token.get(), TokenUser, user, sizeof(user), &size) ||
      !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user)->User.Sid, text.put())) {
    return {};
  }
  return text.get();
}

// What became of the thumbnails |before| holds, in |after|: "<k> kept, <e> evicted, <r>
// replaced". Kept: held (CacheRecord::cached: from the size's own file) with the same size and
// thumbnail ID; replaced: held with another; evicted: not held any more.
std::string EvictionText(const std::vector<test::CacheRecord>& before,
                         const std::vector<test::CacheRecord>& after) {
  size_t kept = 0, evicted = 0, replaced = 0;
  for (const test::CacheRecord& was : before) {
    if (!was.cached) continue;
    const auto now = std::find_if(after.begin(), after.end(), [&was](const test::CacheRecord& r) {
      return r.key == was.key && r.size == was.size;
    });
    if (now == after.end() || !now->cached) {
      ++evicted;
    } else if (now->w == was.w && now->h == was.h && now->id == was.id) {
      ++kept;
    } else {
      ++replaced;
    }
  }
  return std::to_string(kept) + " kept, " + std::to_string(evicted) + " evicted, " +
         std::to_string(replaced) + " replaced";
}

// The text of the refresh experiment's report.txt, which its line "The report: <file>" in
// |output| names, at most kMaxReportBytes of it (*cut set when there is more); std::nullopt with
// *why set when it is not read. The test account wrote that name and can change what is in
// |scratch|, and this process reads with administrator rights, so only a plain file in
// |scratch|\refresh-experiment is read: opened without following a link at its name, and read
// only when where it really is (links and junctions on the way followed) is still in that folder.
std::optional<std::string> ExperimentReport(const std::string& output, const path& scratch,
                                            bool* cut, std::string* why) {
  constexpr std::string_view kReportLine = "The report: ";
  *cut = false;
  const size_t at = output.rfind(kReportLine);
  std::wstring name;
  path real;
  if (at == std::string::npos) {
    *why = "the experiment named no report";
  } else if (FAILED(Utf8ToWide(LineAt(output, at + kReportLine.size()), &name)) || name.empty()) {
    *why = "its name cannot be read";
  } else if (FAILED(Canonicalize(scratch, &real))) {
    *why = "the scratch folder cannot be found";
  }
  if (!why->empty()) return std::nullopt;
  const path file(name);
  const path folder = real / L"refresh-experiment";
  if (!IsUnderLexically(file, scratch / L"refresh-experiment") && !IsUnderLexically(file, folder)) {
    *why = "it is not in the experiment's folder";
    return std::nullopt;
  }
  const wil::unique_hfile handle(CreateFileW(
      file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!handle || !GetFileInformationByHandle(handle.get(), &info)) {
    *why = "it cannot be opened";
    return std::nullopt;
  }
  // Where the opened file really is: a junction on the way is followed in this name.
  std::wstring opened(32768, L'\0');
  const DWORD length =
      GetFinalPathNameByHandleW(handle.get(), opened.data(), static_cast<DWORD>(opened.size()),
                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  opened.resize(length < opened.size() ? length : 0);
  if (opened.empty() ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0 ||
      !IsUnderLexically(opened, folder)) {
    *why = "it is not a plain file in the experiment's folder";
    return std::nullopt;
  }
  std::string text(kMaxReportBytes, '\0');
  DWORD read = 0;
  if (!ReadFile(handle.get(), text.data(), kMaxReportBytes, &read, nullptr)) {
    *why = "it cannot be read";
    return std::nullopt;
  }
  text.resize(read);
  *cut = info.nFileSizeHigh != 0 || info.nFileSizeLow > kMaxReportBytes;
  return text;
}

// The refresh experiment's report (ExperimentReport), one "[ system ] report.txt: <line>" each
// (at most kMaxReportLines; it names no path of the test account's), or one line that says why
// it is not there.
void PrintExperimentReport(const std::string& output, const path& scratch) {
  bool cut = false;
  std::string why;
  const std::optional<std::string> report = ExperimentReport(output, scratch, &cut, &why);
  if (!report) {
    std::printf("[ system ] report.txt: none (%s)\n", why.c_str());
    return;
  }
  size_t printed = 0, more = 0;
  for (const std::string& line : OutputLines(*report)) {
    if (printed == kMaxReportLines) {
      ++more;
      continue;
    }
    std::printf("[ system ] report.txt: %s\n", line.c_str());
    ++printed;
  }
  if (more > 0) std::printf("[ system ] report.txt: (%zu more lines not shown)\n", more);
  if (cut) {
    std::printf("[ system ] report.txt: (cut after %lu bytes)\n",
                static_cast<unsigned long>(kMaxReportBytes));
  }
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

// What the asking process's log says about the last run of an administrator step: its report's
// first line ("elevated <step> start (token: ...)", or "... refused hr=<HRESULT>: <why> (token:
// ...)"), the line of a step that tested its writes below an app package ("elevated <step> writes
// checked: ..."), its end line ("elevated <step> end hr=<HRESULT>[ restart-pending]
// not-done=<n>") and the line RunElevated logs after the report ("<step> launch=<HRESULT>
// exit=<HRESULT>", with " no-report" when nothing came through the pipe, " report-incomplete"
// when its end did not, and " not-done=<n>" when it did not complete some of its parts). Empty
// when missing.
struct StepLog {
  std::string first;
  std::string writesChecked;
  std::string end;
  std::string summary;
};

class System : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(g_uacDidNotElevate)
        << "UAC did not elevate the test account's step in an earlier test (its \"[ system ] UAC "
           "did not elevate\" line says how), and every test here needs it";
    const TestAccount* account = CurrentTestAccount();
    ASSERT_NE(account, nullptr) << "no test account";
    const UserPlaces user = account->Places();
    hive_ = user.hive;
    sid_ = user.sid;
    userRoot_ = user.userRoot;
    dataDir_ = user.dataDir;
    ASSERT_HRESULT_SUCCEEDED(GetMachineRoot(&machineRoot_));
    const std::vector<std::string> left = Leftovers();
    ASSERT_TRUE(left.empty()) << "not a clean start:" << Lines(left);

    // The staged runtime, copied into the test account's temp folder: it can read that folder
    // and every folder above it (the install's permission check reads their permissions as the
    // user), and only it, SYSTEM and Administrators can change the copy. This elevated program
    // copies as an administrator; its own account is left out, which the check would trust only
    // where it is the built-in Administrator (as on GitHub's runners), not on a VM of your own.
    const std::wstring runtime = EnvironmentValue(kRuntimeEnv);
    ASSERT_FALSE(runtime.empty())
        << Text(kRuntimeEnv)
        << " must name the staged runtime (cmake --install <build> --prefix <folder> "
           "--component runtime)";
    const std::string problem = RuntimeSourceProblem(runtime);
    ASSERT_TRUE(problem.empty()) << Text(kRuntimeEnv) << " (" << Text(runtime) << ") " << problem;
    static int count = 0;
    scratch_ = account->tempDir() / (L"pdft-system-" + std::to_wstring(GetCurrentProcessId()) +
                                     L"-" + std::to_wstring(++count));
    stage_ = scratch_ / L"runtime";
    ASSERT_TRUE(MakeDirs(stage_));
    ASSERT_TRUE(ProtectFolder(stage_, {sid_}));
    // Links and junctions that appear after the check are left out of the copy, not followed.
    std::error_code error;
    std::filesystem::copy(
        runtime, stage_,
        std::filesystem::copy_options::recursive | std::filesystem::copy_options::skip_symlinks,
        error);
    ASSERT_FALSE(error) << error.message();
    const std::vector<std::wstring> binaries = {kSettingsExe, kCliExe, kHandlerDll, kPdfiumDll};
    // Judged for the test account, whose --install the tests start: it trusts only itself and
    // the administrators.
    std::optional<UnsafeAccess> unsafe;
    ASSERT_HRESULT_SUCCEEDED(FindUnsafeBuildFolderAccess(stage_, binaries, sid_, &unsafe));
    ASSERT_FALSE(unsafe) << Text(UnsafeAccessText(*unsafe)) << ", so --install would refuse";
    ASSERT_HRESULT_SUCCEEDED(
        ComputeBuildId(stage_ / kHandlerDll, stage_ / kPdfiumDll, &buildName_));
  }

  void TearDown() override {
    if (!CurrentTestAccount()) return;
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
    if (!scratch_.empty() && FAILED(RemoveTreeNoFollow(scratch_))) {
      std::printf("[ system ] the test's copy of the runtime stays: %s\n",
                  Text(scratch_.native()).c_str());
    }
  }

  path Cli() const { return stage_ / kCliExe; }
  path InstalledCli() const { return machineRoot_ / kCliExe; }
  path BuildDll() const { return machineRoot_ / L"app" / buildName_ / kHandlerDll; }
  // The user's registry: the test account's hive, and HKLM.
  RegistryRoots Roots() const { return RegistryRoots{hive_, HKEY_LOCAL_MACHINE}; }

  // As the test account, with the limited token of its logon.
  Outcome User(const path& exe, std::wstring_view arguments) {
    const Outcome run = RunAsUser(exe, std::wstring(arguments));
    Print("user", exe, arguments, run);
    EXPECT_HRESULT_SUCCEEDED(run.hr) << "starting " << Text(exe.native());
    return run;
  }

  // With this process's own (elevated) token.
  Outcome Admin(const path& exe, std::wstring_view arguments) {
    const Outcome run = RunAsAdministrator(exe, std::wstring(arguments));
    Print("administrator", exe, arguments, run);
    EXPECT_HRESULT_SUCCEEDED(run.hr) << "starting " << Text(exe.native());
    return run;
  }

  // ---- The real state ----

  std::optional<std::wstring> ClassDll(Scope scope) const {
    std::optional<ClassReg> reg;
    EXPECT_HRESULT_SUCCEEDED(ReadClass(Roots(), scope, &reg));
    if (!reg) return std::nullopt;
    return reg->dll;
  }

  std::optional<DWORD> StoredZoom() const {
    std::optional<RegistryValue> value;
    if (FAILED(ReadRegistryValue(hive_, kRegKey, kZoomValueName, &value)) || !value) {
      return std::nullopt;
    }
    return RegistryDword(*value);
  }

  std::vector<std::wstring> RunOnceValues() const {
    std::vector<std::wstring> names;
    EXPECT_HRESULT_SUCCEEDED(FindRunOnceRemovals(Roots(), &names));
    return names;
  }

  static bool LegacyTask() {
    bool exists = false;
    EXPECT_HRESULT_SUCCEEDED(LegacyTaskExists(&exists));
    return exists;
  }

  // The test account's check task (named after its SID).
  TaskState UserTask() const {
    TaskLocation location;
    TaskState state;
    EXPECT_HRESULT_SUCCEEDED(DefaultTaskLocation(sid_, &location));
    EXPECT_HRESULT_SUCCEEDED(ReadTask(location, &state));
    return state;
  }

  bool SlotsOurs() const {
    SlotValues slots;
    if (FAILED(ReadSlots(Roots(), &slots))) return false;
    return std::all_of(slots.begin(), slots.end(),
                       [](const auto& slot) { return slot && IsOurClsid(*slot); });
  }

  bool MigratedFromV1() const {
    bool migrated = false;
    EXPECT_HRESULT_SUCCEEDED(ReadMigratedFromV1(Roots(), &migrated));
    return migrated;
  }

  // The log of the app the test account runs: paths.h's log, in the account's data folder.
  path UserLog() const {
    path data;
    path log;
    if (FAILED(GetDataDir(&data)) || FAILED(GetLogPath(&log))) return {};
    return dataDir_ / log.lexically_relative(data);
  }

  StepLog ReadStepLog(std::wstring_view verb) const {
    StepLog step;
    std::ifstream file(UserLog(), std::ios::binary);
    if (!file.good()) return step;
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const std::string name = Text(verb);
    const std::string summaryStart = name + " launch=";
    const size_t summary = text.rfind(summaryStart);
    if (summary == std::string::npos) return step;
    step.summary = LineAt(text, summary);
    // The report's lines come right before the summary, after the summary of the run before.
    const size_t previous =
        summary == 0 ? std::string::npos : text.rfind(summaryStart, summary - 1);
    const size_t from = previous == std::string::npos ? 0 : previous;
    size_t first = std::string::npos;
    for (const char* kind : {" start (", " refused hr="}) {
      const size_t at = text.rfind("elevated " + name + kind, summary);
      if (at != std::string::npos && at >= from && (first == std::string::npos || at > first)) {
        first = at;
      }
    }
    if (first != std::string::npos) step.first = LineAt(text, first);
    const size_t checked = text.rfind("elevated " + name + " writes checked: ", summary);
    if (checked != std::string::npos && checked >= from) step.writesChecked = LineAt(text, checked);
    const size_t end = text.rfind("elevated " + name + " end hr=", summary);
    if (end != std::string::npos && end >= from) step.end = LineAt(text, end);
    return step;
  }

  // The Explorer test of the last run of the command logged as |component|, as that run's own
  // summary line records it: the first line after the last "<component> INFO start" line that
  // has the same process id and contains |summary|. "0x00000000" or another HRESULT when it ran,
  // "could-not-start" or "skipped"; empty when the line or its live= field is missing. Other
  // processes write to the same log (a scheduled check among them): the process id keeps their
  // lines out.
  std::string LoggedExplorerTest(std::string_view component, std::string_view summary) const {
    std::ifstream file(UserLog(), std::ios::binary);
    if (!file.good()) return {};
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    const size_t start = text.rfind(" " + std::string(component) + " INFO start ");
    if (start == std::string::npos) return {};
    // "<time> <pid> <component> INFO start ...": the process id is the second field.
    const size_t lineStart = text.rfind('\n', start);
    const std::string startLine = LineAt(text, lineStart == std::string::npos ? 0 : lineStart + 1);
    const size_t pidAt = startLine.find(' ');
    if (pidAt == std::string::npos) return {};
    const std::string pid = startLine.substr(pidAt + 1, startLine.find(' ', pidAt + 1) - pidAt - 1);
    for (size_t at = text.find('\n', start); at != std::string::npos; at = text.find('\n', at)) {
      const std::string line = LineAt(text, ++at);
      const size_t own = line.find(' ');
      if (own == std::string::npos || line.compare(own + 1, pid.size() + 1, pid + " ") != 0 ||
          line.find(summary) == std::string::npos) {
        continue;
      }
      const size_t live = line.find(" live=");
      if (live == std::string::npos) return {};
      const size_t value = live + 6;
      return line.substr(value, line.find(' ', value) - value);
    }
    return {};
  }

  // ---- Expectations ----

  // What a machine-wide install leaves: the files, the class for all users pointing at this
  // build, the user's slots and check task (in the task folder, running the installed settings
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

  // A standard user's own per-user step once an administrator made the protected task folder
  // (the install for all users did): the standard account's --setup-user, with its own token,
  // quiet and exit 0, puts its check task into that folder (not under the root-folder name),
  // with both triggers and the installed settings executable, and sets its slots and zoom.
  void ExpectStandardAccountsSetupUser() {
    const TestAccount* standard = CurrentStandardAccount();
    if (!standard) {
      ADD_FAILURE() << "no standard account (a \"[ system ] the standard account\" line says why)";
      return;
    }
    const Outcome run = standard->Run(InstalledCli(), L"--setup-user", kCommandTimeoutMs);
    Print("standard account", InstalledCli(), L"--setup-user", run);
    EXPECT_HRESULT_SUCCEEDED(run.hr);
    EXPECT_EQ(run.exitCode, 0u);
    EXPECT_EQ(run.Output(), "");
    TaskLocation location;
    TaskState task;
    EXPECT_HRESULT_SUCCEEDED(DefaultTaskLocation(standard->sid(), &location));
    EXPECT_HRESULT_SUCCEEDED(ReadTask(location, &task));
    std::printf("[ system ] standard account's check task %s: after PdfCraft %d, at sign-in %d\n",
                task.exists ? Text(task.path).c_str() : "(none)", task.afterPdfCraft,
                task.atSignIn);
    std::printf(
        "[ system ] the standard account's --setup-user put its check task in the protected task "
        "folder: %s\n",
        task.exists && !task.atFallback ? "yes" : "no");
    ASSERT_TRUE(task.exists);
    EXPECT_FALSE(task.atFallback) << "under the root-folder name: " << Text(task.path);
    EXPECT_TRUE(task.enabled);
    EXPECT_TRUE(task.afterPdfCraft);
    EXPECT_TRUE(task.atSignIn);
    EXPECT_TRUE(IEquals(task.actionPath, (machineRoot_ / kSettingsExe).native()))
        << Text(task.actionPath);
    EXPECT_EQ(task.actionArgs, kCheckTaskArguments);
    SlotValues slots;
    ASSERT_HRESULT_SUCCEEDED(
        ReadSlots(RegistryRoots{standard->hive(), HKEY_LOCAL_MACHINE}, &slots));
    for (const auto& slot : slots) EXPECT_TRUE(slot && IsOurClsid(*slot));
    std::optional<RegistryValue> zoom;
    EXPECT_HRESULT_SUCCEEDED(ReadRegistryValue(standard->hive(), kRegKey, kZoomValueName, &zoom));
    EXPECT_EQ(zoom ? RegistryDword(*zoom) : std::nullopt, std::optional<DWORD>(100));
  }

  // The standard account does not uninstall: the step for all users takes its slots, settings and
  // task from its loaded hive and the task folder (ExpectNothingLeft checks), and its data folder,
  // with the log of its --setup-user, stays as every such user's does. It never signs in again,
  // so the folder goes here.
  static void RemoveStandardAccountsDataFolder() {
    const TestAccount* standard = CurrentStandardAccount();
    if (!standard) return;
    const path data = standard->Places().dataDir;
    if (data.empty() || !Exists(data)) return;
    std::printf(
        "[ system ] the standard account's data folder (its log) removed by the tests: %s\n",
        SUCCEEDED(RemoveTreeNoFollow(data)) ? "yes" : "no");
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
    // It may fail on a runner without a desktop, but it must start.
    EXPECT_NE(StringAt(status.json, {"liveTest", "skipReason"}), "couldNotStart");
    return status;
  }

  // The status is healthy, or only the Explorer test keeps it from being so; |command| (logged as
  // |component|, its summary line containing |summary|) is judged by its own Explorer test, not
  // by the status's: it must have started, and the command exited 1 when it failed, or, where
  // the command needs the test's answer (|testRequired|), when it did not run; else 0.
  void ExpectHealthy(const Outcome& command, const Status& status, std::string_view component,
                     std::string_view summary, bool testRequired) {
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
    const std::string own = LoggedExplorerTest(component, summary);
    std::printf("[ system ] %s's own Explorer test: %s\n", std::string(component).c_str(),
                own.empty() ? "(not in the log)" : own.c_str());
    EXPECT_FALSE(own.empty()) << "no live= field on " << component << "'s summary line";
    // Starting it needs no desktop (only %TEMP%, an event and a thread): a start failure is this
    // program's, not the runner's.
    EXPECT_NE(own, "could-not-start");
    const bool ownFailed = own != "0x00000000" && (own.starts_with("0x") || testRequired);
    EXPECT_EQ(command.exitCode, (!others.empty() || ownFailed) ? 1u : 0u) << command.Output();
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

  // The administrator step |verb| that |command| (the test account's) asked for got
  // administrator rights through UAC: its report starts with "start", which the elevated side
  // writes only after its entry check found an elevated token with Administrators enabled at
  // High integrity. Otherwise this fails with what happened instead; when UAC did not elevate
  // the step (it started without administrator rights, it could not be started, or nobody
  // answered for 10 minutes), a line says so with the step's own token line, and the tests after
  // this one stop at once. Use it as ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(...)).
  void ExpectAdministratorRights(std::wstring_view verb, const Outcome& command) {
    const std::string name = Text(verb);
    const StepLog step = ReadStepLog(verb);
    if (step.first.find(" start (") != std::string::npos) {
      // "elevated <step> start (token: ...)": the token part.
      std::string token = step.first.substr(step.first.find(" start (") + 8);
      if (!token.empty() && token.back() == ')') token.pop_back();
      std::printf("[ system ] %s got administrator rights through UAC: yes (%s)\n", name.c_str(),
                  token.c_str());
      return;
    }
    std::printf("[ system ] %s got administrator rights through UAC: no\n", name.c_str());
    const size_t launch = step.summary.find("launch=");
    const bool launchFailed =
        launch != std::string::npos && step.summary.compare(launch + 7, 10, "0x00000000") != 0;
    std::string why;
    bool uac = true;
    if (command.hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
      why = "no answer within " + std::to_string(kCommandTimeoutMs / 60000) +
            " minutes (a prompt nobody can answer?)";
    } else if (step.first.find(": no administrator rights") != std::string::npos) {
      why = "the step started without administrator rights; its report: " + step.first;
    } else if (launchFailed) {
      why = "the step could not be started: " + step.summary;
    } else if (!step.first.empty()) {
      uac = false;  // elevated, but refused for another reason
      why = "the step refused: " + step.first;
    } else if (!step.summary.empty()) {
      uac = false;
      why = "the step's report did not come back (no token line): " + step.summary;
    } else {
      uac = false;
      why = "the command did not start the step (see its output above)";
    }
    if (uac) {
      g_uacDidNotElevate = true;
      std::printf("[ system ] UAC did not elevate %s from the test account's limited token: %s\n",
                  name.c_str(), why.c_str());
    }
    FAIL() << name << ": " << why;
  }

  // The administrator step |verb| reported back through the asking process's pipe: its first
  // line reached the log of the unelevated process, and the summary line after it does not say
  // "no-report". That pipe takes lines only from the elevated process if its parent is the
  // pipe's server, so this proves that UAC makes the asking process the parent. The first line
  // is printed too: it tells what token UAC gave the step. Its report came back whole, with every
  // part done: the end line counts none ("not-done=0"), which the elevated step writes itself,
  // and the summary says neither "not-done=" nor "report-incomplete". Nothing above the step is
  // packaged here, so it tested no writes of its own (no "writes checked" line).
  void ExpectReportThroughThePipe(std::wstring_view verb) {
    ASSERT_TRUE(Exists(UserLog())) << Text(UserLog().native());
    const StepLog step = ReadStepLog(verb);
    if (!step.first.empty()) std::printf("[ system ] log: %s\n", step.first.c_str());
    if (!step.writesChecked.empty()) {
      std::printf("[ system ] log: %s\n", step.writesChecked.c_str());
    }
    if (!step.end.empty()) std::printf("[ system ] log: %s\n", step.end.c_str());
    if (!step.summary.empty()) std::printf("[ system ] log: %s\n", step.summary.c_str());
    const bool reported = !step.first.empty() && !step.summary.empty() &&
                          step.summary.find(" no-report") == std::string::npos;
    std::printf("[ system ] %s reported through the asking process's pipe: %s\n",
                Text(verb).c_str(), reported ? "yes" : "no");
    const bool taken = step.summary.find("(taken by another process)") != std::string::npos;
    EXPECT_TRUE(reported) << (taken ? "another process had made the report pipe's name first"
                                    : "the elevated step's report did not reach the unelevated "
                                      "process");
    EXPECT_TRUE(step.end.ends_with(" not-done=0"))
        << "the step's end line: " << (step.end.empty() ? "(none)" : step.end);
    EXPECT_EQ(step.summary.find(" not-done="), std::string::npos) << step.summary;
    EXPECT_EQ(step.summary.find(" report-incomplete"), std::string::npos) << step.summary;
    EXPECT_TRUE(step.writesChecked.empty()) << step.writesChecked;
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

  // 1.x's sign-in clean-up of its per-user folder, and a second value of the family, in the
  // user's RunOnce key.
  void PlantRunOnce() {
    const std::wstring command = L"cmd.exe /d /c rd /s /q \"" + userRoot_.native() + L"\"";
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(hive_, kRunOnceKey, kLegacyRunOnce, command));
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(hive_, kRunOnceKey, RunOnceValueName(1), command));
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

  // ---- The thumbnail refresh and the cache rebuild ----

  // thumbtest, copied where the test account can run it: three folders down in the scratch
  // folder, since the experiment writes its report three folders up from itself
  // (out\refresh-experiment from a build's bin folder), which keeps that in the scratch folder
  // too. Empty when it cannot be copied.
  path CopyThumbtest() {
    const path from = path(test::SelfFolder()) / kThumbtestExe;
    const path bin = scratch_ / L"out" / L"build" / L"bin";
    if (!Exists(from)) {
      ADD_FAILURE() << Text(from.native()) << " is missing (the build makes it with the tools, "
                    << "and the test bundle carries it)";
      return {};
    }
    if (!MakeDirs(bin) || !CopyFileW(from.c_str(), (bin / kThumbtestExe).c_str(), FALSE)) {
      ADD_FAILURE() << "cannot copy " << Text(from.native()) << " (error " << GetLastError() << ")";
      return {};
    }
    return bin / kThumbtestExe;
  }

  // The last line of the test account's log that contains |text|; empty when there is none.
  std::string LastLogLine(std::string_view text) const {
    std::ifstream file(UserLog(), std::ios::binary);
    if (!file.good()) return {};
    const std::string log((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const size_t at = log.rfind(text);
    if (at == std::string::npos) return {};
    const size_t start = log.rfind('\n', at);
    return LineAt(log, start == std::string::npos ? 0 : start + 1);
  }

  // The refresh experiment's automatic part as the test account (thumbtest experiment
  // --non-interactive), with the installed command-line executable and its folder in the scratch
  // folder: it sets the account's zoom to 100 % and 142 % and puts it back, fills the account's
  // thumbnail cache with synthetic PDFs at every size, makes them again with each extraction
  // variant the product's refresh allows (the product's own; it names the others as left out)
  // and judges them by the bitmaps' sizes (Pass A). Its verdict is printed, not judged
  // (its main lines, then its whole report): the experiment must only have reached one (exit 0,
  // or 1 without a pass or after a stop) and put the zoom back.
  void RunRefreshExperiment(const path& thumbtest) {
    const std::optional<DWORD> zoom = StoredZoom();
    const std::wstring arguments =
        L"experiment --non-interactive --cli " + test::QuoteArgument(InstalledCli().native()) +
        L" --root " + test::QuoteArgument((scratch_ / L"experiment").native()) +
        L" --timing-files " + std::to_wstring(kExperimentTimingFiles) + L" --instance-files " +
        std::to_wstring(kExperimentInstanceFiles);
    const Outcome run = CurrentTestAccount()->Run(thumbtest, arguments, kExperimentTimeoutMs);
    Print("user", thumbtest, arguments, run);
    for (const std::string& line : OutputLines(run.out)) {
      if (std::any_of(std::begin(kExperimentLines), std::end(kExperimentLines),
                      [&line](std::string_view start) { return line.starts_with(start); })) {
        std::printf("[ system ] refresh experiment: %s\n", line.c_str());
      }
    }
    PrintExperimentReport(run.out, scratch_);
    if (FAILED(run.hr)) {
      // Not started, or ended at its time limit: there is no exit code to read.
      const std::string why =
          run.hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)
              ? "ended after " + std::to_string(kExperimentTimeoutMs / 60000) + " minutes"
              : Text(HResultText(run.hr));
      std::printf("[ system ] refresh experiment as the test account: no verdict (%s)\n",
                  why.c_str());
      ADD_FAILURE() << "running " << Text(thumbtest.native()) << ": " << why;
    } else {
      std::printf("[ system ] refresh experiment as the test account: exit %lu\n", run.exitCode);
      EXPECT_TRUE(run.exitCode == 0 || run.exitCode == 1)
          << "the experiment did not reach a verdict (2: it cannot run here, 8: refused here)";
    }
    EXPECT_EQ(StoredZoom(), zoom) << "the experiment did not put the zoom back";
  }

  // --refresh --full --yes as the test account while File Explorer on this desktop is another
  // account's (the runner's): refused before anything changes (exit 8, logged as
  // other-shell-user), and File Explorer's process is the same one afterwards. It runs only once
  // that refusal is certain, with a shell that runs as another account than the test account;
  // otherwise a line says why it did not run.
  void ExpectRebuildRefusedForAnotherAccountsExplorer() {
    const char* what = "--refresh --full --yes as the test account";
    const std::optional<ShellProcess> before = RealRebuildSystem().Shell();
    const std::wstring owner = before ? ProcessUserSid(before->pid) : std::wstring();
    if (!before || owner.empty() || IEquals(owner, sid_)) {
      std::printf("[ system ] %s: not run (%s)\n", what,
                  !before         ? "no File Explorer on this desktop"
                  : owner.empty() ? "File Explorer's account cannot be read"
                                  : "File Explorer is the test account's own");
      return;
    }
    const Outcome run = User(InstalledCli(), L"--refresh --full --yes");
    const std::optional<ShellProcess> after = RealRebuildSystem().Shell();
    const bool same =
        after && after->pid == before->pid && CompareFileTime(&after->start, &before->start) == 0;
    std::printf("[ system ] %s -> exit %lu, File Explorer's process unchanged: %s\n", what,
                run.exitCode, same ? "yes" : "no");
    EXPECT_EQ(run.exitCode, 8u) << run.Output();
    EXPECT_TRUE(same) << "File Explorer (pid " << before->pid << ") is not the process it was";
    const std::string logged = LastLogLine(" refresh full ");
    if (!logged.empty()) std::printf("[ system ] log: %s\n", logged.c_str());
    EXPECT_NE(logged.find("refresh full refused=other-shell-user"), std::string::npos) << logged;
  }

  // The eviction files' thumbnails at kEvictionSize, as the test account's cache answers for them
  // (thumbtest incache, which only asks), read back from the record it writes as |name|.tsv in
  // the scratch folder.
  std::vector<test::CacheRecord> AskTheCache(const path& thumbtest, const path& folder,
                                             std::wstring_view name) {
    const path record = scratch_ / (std::wstring(name) + L".tsv");
    (void)User(thumbtest, L"incache --sizes " + std::to_wstring(kEvictionSize) + L" --record " +
                              test::QuoteArgument(record.native()) + L" " +
                              test::QuoteArgument(folder.native()));
    std::vector<test::CacheRecord> records;
    EXPECT_TRUE(test::ReadRecords(record, &records)) << "no record: " << Text(record.native());
    return records;
  }

  path scratch_;  // in the test account's temp folder; removed after the test
  path stage_;
  path machineRoot_;
  // The user's: the test account's hive, SID and folders.
  HKEY hive_ = nullptr;
  std::wstring sid_;
  path userRoot_;
  path dataDir_;
  std::wstring buildName_;
};

// ---- The task tests as a standard user -----------------------------------------------------

// The check task's writing side from a real standard account (not a member of Administrators),
// logged on interactively, before anything is installed: the integration tests' task tests, run
// as it, must all pass but the one that needs a Windows session of the account's own, and the
// protected task folder's tests must skip, since they need an administrator
// (StandardAccountTaskTestProblems). Nothing of them may be left in Task Scheduler, and nothing
// of the product anywhere (they write only test keys and test tasks of their own).
TEST(StandardAccount, TaskTestsPassAndTheProtectedFolderOnesSkip) {
  const TestAccount* standard = CurrentStandardAccount();
  ASSERT_NE(standard, nullptr) << "no standard account (a \"[ system ] the standard account\" "
                                  "line says why)";
  // The test program and pdft-run-medium.exe, which the task tests point their tasks at, copied
  // where the account can read them.
  const path tools = standard->tempDir() / L"pdft-system-task-tests";
  ASSERT_TRUE(MakeDirs(tools));
  const auto removeTools = wil::scope_exit([&tools] { (void)RemoveTreeNoFollow(tools); });
  for (const wchar_t* name : {kIntegrationTestsExe, L"pdft-run-medium.exe"}) {
    const path from = path(test::SelfFolder()) / name;
    ASSERT_TRUE(CopyFileW(from.c_str(), (tools / name).c_str(), FALSE)) << Text(from.native());
  }
  const std::wstring arguments =
      std::wstring(L"--gtest_filter=") + kStandardAccountTaskFilter + L" --gtest_color=no";
  const Outcome run = standard->Run(tools / kIntegrationTestsExe, arguments, kCommandTimeoutMs);
  Print("standard account", tools / kIntegrationTestsExe, arguments, run);
  // A run that could not start or was ended at the timeout fails below, after the clean-up: it
  // may have registered test tasks and folders that are still there.

  const std::vector<GTestCase> cases = ReadGTestCases(run.out);
  size_t counts[4] = {};
  for (const GTestCase& test : cases) {
    ++counts[static_cast<size_t>(test.result)];
    if (test.result == GTestCase::Result::kSkipped) {
      std::printf("[ system ] as the standard account, %s skipped: %s\n", test.name.c_str(),
                  test.skipReason.c_str());
    }
  }
  std::printf(
      "[ system ] task tests as the standard account: %zu passed, %zu skipped, %zu failed, %zu "
      "did not finish (exit %lu)\n",
      counts[static_cast<size_t>(GTestCase::Result::kPassed)],
      counts[static_cast<size_t>(GTestCase::Result::kSkipped)],
      counts[static_cast<size_t>(GTestCase::Result::kFailed)],
      counts[static_cast<size_t>(GTestCase::Result::kUnfinished)], run.exitCode);
  std::vector<std::string> problems = StandardAccountTaskTestProblems(cases, run.exitCode);
  if (FAILED(run.hr)) {
    problems.insert(problems.begin(),
                    "the program did not run to its end: " + Text(HResultText(run.hr)));
  }
  for (const std::string& problem : problems) {
    std::printf("[ system ] task tests as the standard account: %s\n", problem.c_str());
  }
  const std::vector<std::string> left = RemoveTaskTestLeftovers();
  for (const std::string& line : left) {
    std::printf("[ system ] left in Task Scheduler by the task tests: %s\n", line.c_str());
  }
  const std::vector<std::string> product = Leftovers();
  if (!product.empty()) {
    std::printf("[ system ] cleaning up what the task tests left:%s\n", Lines(product).c_str());
    ForceClean();
  }
  std::printf("[ system ] task tests as the standard account as expected: %s\n",
              problems.empty() && left.empty() && product.empty() ? "yes" : "no");
  EXPECT_TRUE(problems.empty()) << Lines(problems);
  EXPECT_TRUE(left.empty()) << Lines(left);
  EXPECT_TRUE(product.empty()) << Lines(product);
}

// ---- Installed for all users, used, uninstalled --------------------------------------------

TEST_F(System, InstallChecksZoomFixAndUninstall) {
  const Outcome install = User(Cli(), L"--install --machine");
  ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(L"machine-install", install));
  ExpectInstalledForAllUsers();
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(100));
  const Status installed = ReadStatus();
  ExpectHealthy(install, installed, "install", " install scope=", true);
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
  // refuses an administrator (this process); the one for all users needs an administrator: the
  // user's limited token is refused, this process's is not.
  const Outcome setupUser = User(InstalledCli(), L"--setup-user");
  EXPECT_EQ(setupUser.exitCode, 0u);
  EXPECT_EQ(setupUser.Output(), "");
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(130));
  ExpectTask(true, true);
  EXPECT_EQ(Admin(InstalledCli(), L"--setup-user").exitCode, 8u);
  EXPECT_EQ(User(InstalledCli(), L"--setup-machine").exitCode, 7u);
  EXPECT_EQ(Admin(InstalledCli(), L"--setup-machine").exitCode, 0u);
  EXPECT_EQ(TaskFolderExists(kTaskFolder), std::optional<bool>(true));
  // Another user of this PC, a standard one, sets itself up in the folder an administrator made.
  ExpectStandardAccountsSetupUser();

  // The class for all users points at a build that is not there: --fix has the administrator
  // step point it at the newest complete build, through UAC.
  const path missing = machineRoot_ / L"app" / L"2.0.0.0-00000000" / kHandlerDll;
  ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(
      HKEY_LOCAL_MACHINE, ClassKeyPath() + L"\\InprocServer32", L"", missing.native()));
  const Outcome fix = User(InstalledCli(), L"--fix");
  ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(L"machine-repair-class", fix));
  const std::optional<std::wstring> repaired = ClassDll(Scope::kMachine);
  ASSERT_TRUE(repaired.has_value());
  EXPECT_TRUE(IEquals(*repaired, BuildDll().native())) << Text(*repaired);
  ExpectHealthy(fix, ReadStatus(), "fix", " check result=", false);
  ExpectReportThroughThePipe(L"machine-repair-class");

  // The uninstall, from the installed executable: the running programs go to a trash folder
  // that is deleted at the restart. (Its log goes with the user's data folder, so its step's
  // report is not read back; what is left shows whether the step worked.) The standard
  // account's part goes with the step for all users, but for its data folder.
  const Outcome uninstall = User(InstalledCli(), L"--uninstall --yes");
  EXPECT_EQ(uninstall.exitCode, 0u);
  EXPECT_FALSE(StoredZoom().has_value());
  RemoveStandardAccountsDataFolder();
  ExpectNothingLeft();
}

// ---- 1.1 found by the install ------------------------------------------------------------

// 1.1 installed for all users, with its per-user files, settings, task and sign-in clean-up:
// --install --machine replaces it.
TEST_F(System, InstallReplaces11ForAllUsers) {
  const path v1 = PlantV1Build(machineRoot_);
  PlantClass(HKEY_LOCAL_MACHINE, v1);
  for (size_t i = 0; i < kSlotCount; ++i) {
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(hive_, SlotKeyPath(i), L"", kHandlerClsidString));
  }
  PlantV1UserFolder(120);
  PlantRunOnce();
  PlantLegacyTask();

  const Outcome install = User(Cli(), L"--install --machine");
  ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(L"machine-install", install));
  ExpectInstalledForAllUsers();
  EXPECT_FALSE(Exists(v1.parent_path()));
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(120));
  ExpectV1UserFilesGone();
  const Status status = ReadStatus();
  ExpectHealthy(install, status, "install", " install scope=", true);
  ExpectStatusForAllUsers(status, 120);

  // The clean-up again finds nothing to do.
  EXPECT_EQ(User(InstalledCli(), L"--migrate-v1").exitCode, 0u);

  EXPECT_EQ(User(InstalledCli(), L"--uninstall --yes").exitCode, 0u);
  ExpectNothingLeft();
}

// 1.1 installed for this user only (its class in the user's HKCU): --install --machine takes
// over, and the per-user class and build go.
TEST_F(System, InstallReplaces11ForThisUser) {
  const path v1 = PlantV1Build(userRoot_);
  PlantClass(hive_, v1);
  for (size_t i = 0; i < kSlotCount; ++i) {
    ASSERT_HRESULT_SUCCEEDED(WriteRegistryString(hive_, SlotKeyPath(i), L"", kHandlerClsidString));
  }
  PlantV1UserFolder(120);
  PlantRunOnce();
  PlantLegacyTask();

  const Outcome install = User(Cli(), L"--install --machine");
  ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(L"machine-install", install));
  ExpectInstalledForAllUsers();
  EXPECT_EQ(StoredZoom(), std::optional<DWORD>(120));
  ExpectV1UserFilesGone();
  const Status status = ReadStatus();
  ExpectHealthy(install, status, "install", " install scope=", true);
  ExpectStatusForAllUsers(status, 120);

  EXPECT_EQ(User(InstalledCli(), L"--uninstall --yes").exitCode, 0u);
  ExpectNothingLeft();
}

// ---- --migrate-v1 after the install ------------------------------------------------------

// 1.1's leftovers next to an installed 2.0: --migrate-v1 removes the per-user ones and has the
// administrator step remove 1.1's build folder for all users, through UAC, by the installed
// settings executable.
TEST_F(System, MigrateV1RemovesWhat11Left) {
  const Outcome install = User(Cli(), L"--install --machine");
  ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(L"machine-install", install));
  ExpectInstalledForAllUsers();
  const path v1 = PlantV1Build(machineRoot_);
  PlantV1UserFolder(120);
  PlantRunOnce();
  PlantLegacyTask();

  const Outcome migrate = User(InstalledCli(), L"--migrate-v1");
  ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(L"machine-migrate-v1", migrate));
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

// --uninstall-cleanup, as an installer's uninstaller runs it (elevated, here as this process,
// another account than the user's): the part for all users and the loaded user hives (the test
// account's among them), exit 0, with nothing left out or failed on its error output; the data
// folder is the signed-in user's, left to their own uninstall or a sign-in clean-up.
TEST_F(System, UninstallCleanupAsTheInstallersUninstaller) {
  const Outcome install = User(Cli(), L"--install --machine");
  ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(L"machine-install", install));
  ExpectInstalledForAllUsers();

  const Outcome cleanup = Admin(InstalledCli(), L"--uninstall-cleanup");
  EXPECT_EQ(cleanup.exitCode, 0u);
  EXPECT_EQ(cleanup.Output(), "");
  EXPECT_FALSE(ClassDll(Scope::kMachine).has_value());
  EXPECT_EQ(TaskFolderExists(kTaskFolder), std::optional<bool>(false));
  EXPECT_FALSE(UserTask().exists);
  EXPECT_FALSE(Exists(machineRoot_));
  SlotValues slots;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(Roots(), &slots));
  for (const auto& slot : slots) EXPECT_FALSE(slot && IsOurClsid(*slot));
  bool settings = true;
  ASSERT_HRESULT_SUCCEEDED(RegistryKeyExists(hive_, kRegKey, &settings));
  EXPECT_FALSE(settings);

  // What is left for the user: the data folder, removed by a sign-in clean-up or their own
  // --uninstall; a sign-in clean-up is not left for a sign-in that never comes.
  for (const std::wstring& name : RunOnceValues()) {
    std::optional<std::wstring> command;
    EXPECT_HRESULT_SUCCEEDED(ReadRegistryString(hive_, kRunOnceKey, name, &command));
    std::printf("[ system ] sign-in clean-up %s: %s\n", Text(name).c_str(),
                Text(command.value_or(L"")).c_str());
  }
  ASSERT_HRESULT_SUCCEEDED(DeleteRunOnceRemovals(Roots()));
  if (Exists(dataDir_)) {
    const Outcome uninstall = User(Cli(), L"--uninstall --yes");
    EXPECT_TRUE(uninstall.exitCode == 0 || uninstall.exitCode == 6) << uninstall.Output();
  }
  ExpectNothingLeft();
}

// ---- The thumbnail refresh and the cache rebuild -----------------------------------------

// With the install for all users, on the test account's thumbnail cache: whether the Explorer
// test leaves its process holding the cache (the settings window runs it in its own), the refresh
// experiment's automatic part, --refresh --full refused while File Explorer is the runner's, and
// the eviction question: what the cache answers for two thumbnails it held once the uninstall has
// unregistered the handler, and again after a forced extraction (kept, evicted or replaced;
// printed, not judged). A cache that refuses the product's question fails the test: --refresh
// would then refresh nothing.
TEST_F(System, ThumbnailRefreshAndRebuildAsTheUser) {
  const path thumbtest = CopyThumbtest();
  ASSERT_FALSE(thumbtest.empty());
  // Two synthetic PDFs the test account can read: the scratch folder is in its temp folder.
  const path eviction = scratch_ / L"eviction";
  ASSERT_TRUE(MakeDirs(eviction));
  for (const test::GeneratedFile& file : test::MakeTestPdfs()) {
    if (std::find(std::begin(kEvictionFiles), std::end(kEvictionFiles), file.name) !=
        std::end(kEvictionFiles)) {
      ASSERT_TRUE(WriteFileText(eviction / file.name, file.bytes)) << Text(file.name);
    }
  }

  const Outcome install = User(Cli(), L"--install --machine");
  ASSERT_NO_FATAL_FAILURE(ExpectAdministratorRights(L"machine-install", install));
  ExpectInstalledForAllUsers();

  // Read-only: the holders of the account's cache files by image name, once the Explorer test has
  // run in that process.
  const Outcome info = User(thumbtest, L"shellinfo --holders --after-explorer-test");
  for (const std::string& line : OutputLines(info.out)) {
    if (line.starts_with("this process holds a cache file")) {
      std::printf("[ system ] the test account's shellinfo: %s\n", line.c_str());
    }
  }
  RunRefreshExperiment(thumbtest);
  ExpectRebuildRefusedForAnotherAccountsExplorer();

  // The thumbnails made and stored while the handler is registered.
  const std::wstring extract =
      L"cache " + std::to_wstring(kEvictionSize) + L" - " + test::QuoteArgument(eviction.native());
  (void)User(thumbtest, extract);
  const std::vector<test::CacheRecord> before =
      AskTheCache(thumbtest, eviction, L"eviction-before");
  // A refusal is no answer: the product asks the same question, so --refresh would refresh
  // nothing.
  const bool refused = std::any_of(before.begin(), before.end(),
                                   [](const test::CacheRecord& r) { return r.hr == E_INVALIDARG; });
  const size_t cached = static_cast<size_t>(std::count_if(
      before.begin(), before.end(), [](const test::CacheRecord& r) { return r.cached; }));
  if (refused) {
    std::printf("[ system ] eviction: the cache refuses the question at %u (0x%08lX)\n",
                kEvictionSize, static_cast<unsigned long>(E_INVALIDARG));
    ADD_FAILURE() << "the cache refuses the product's question: --refresh would refresh nothing";
  } else {
    std::printf("[ system ] eviction: %zu of %zu thumbnails held at %u before the uninstall\n",
                cached, std::size(kEvictionFiles), kEvictionSize);
  }

  // Printed, not judged: whether using the cache in a process leaves its files open there. In a
  // folder of its own, so that this refresh does not touch the eviction files' thumbnails.
  const path hold = scratch_ / L"cache-hold";
  ASSERT_TRUE(MakeDirs(hold));
  for (const test::GeneratedFile& file : test::MakeTestPdfs()) {
    if (file.name == kEvictionFiles[0]) {
      ASSERT_TRUE(WriteFileText(hold / file.name, file.bytes)) << Text(file.name);
    }
  }
  (void)User(thumbtest, L"cache " + std::to_wstring(kEvictionSize) + L" - " +
                            test::QuoteArgument(hold.native()));
  (void)User(thumbtest, L"cachehold " + test::QuoteArgument(hold.native()));

  const Outcome uninstall = User(InstalledCli(), L"--uninstall --yes");
  EXPECT_EQ(uninstall.exitCode, 0u);
  const std::vector<test::CacheRecord> unregistered =
      AskTheCache(thumbtest, eviction, L"eviction-after");
  const Outcome forced = User(thumbtest, extract);
  const std::vector<test::CacheRecord> extracted =
      AskTheCache(thumbtest, eviction, L"eviction-forced");
  if (refused) {
    std::printf(
        "[ system ] eviction after the uninstall: not answered (the cache refused the question)\n");
  } else if (cached == 0) {
    std::printf(
        "[ system ] eviction after the uninstall: not answered (nothing was held before it)\n");
  } else {
    std::printf("[ system ] eviction after the uninstall: %s\n",
                EvictionText(before, unregistered).c_str());
    std::printf("[ system ] eviction after the uninstall and a forced extraction (exit %lu): %s\n",
                forced.exitCode, EvictionText(before, extracted).c_str());
  }
  ExpectNothingLeft();
}

}  // namespace
}  // namespace pdft::systest
