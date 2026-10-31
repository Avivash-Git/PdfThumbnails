// Reading builds from disk: PE machine and version of real DLLs (pe_info.h), build ids and build
// folders (build_folder.h), and the 1.x leftovers of a fake per-user folder (legacy.h). Files are
// only read, except in private temporary folders; the registry is a private key per test.
//
// Two optional comparisons need things only a developer PC has, and skip otherwise:
// - the build-tree test registration sets PDFT_TEST_CMAKE and PDFT_TEST_BUILDID_SCRIPT, and the
//   build id is then compared with what cmake/BuildId.cmake computes for the same DLLs;
// - where the 1.1 release is installed machine-wide, its folder name is recomputed from its DLLs.

#include <windows.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "build_folder.h"
#include "child_process.h"
#include "legacy.h"
#include "medium_il.h"
#include "pe_info.h"
#include "project_config.h"
#include "registration.h"
#include "registry_sandbox.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

#if defined(_M_ARM64)
constexpr USHORT kOwnMachine = IMAGE_FILE_MACHINE_ARM64;
#else
constexpr USHORT kOwnMachine = IMAGE_FILE_MACHINE_AMD64;
#endif

std::wstring BuiltDll() { return test::SelfFolder() + L"\\" + kHandlerDll; }
std::wstring BuiltPdfium() { return test::SelfFolder() + L"\\" + kPdfiumDll; }

std::wstring System32File(const wchar_t* name) {
  wchar_t folder[MAX_PATH] = {};
  const UINT length = GetSystemDirectoryW(folder, MAX_PATH);
  return std::wstring(folder, length) + L"\\" + name;
}

void WriteBytes(const std::wstring& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << bytes;
}

std::string ReadBytes(const std::wstring& path, size_t limit = SIZE_MAX) {
  std::ifstream file(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (bytes.size() > limit) bytes.resize(limit);
  return bytes;
}

bool MakeFolders(const std::wstring& path) {
  std::error_code error;
  std::filesystem::create_directories(path, error);
  return !error;
}

std::optional<std::wstring> Environment(const wchar_t* name) {
  wchar_t value[1024] = {};
  const DWORD length = GetEnvironmentVariableW(name, value, ARRAYSIZE(value));
  if (length == 0 || length >= ARRAYSIZE(value)) return std::nullopt;
  return std::wstring(value, length);
}

// ---- PE machine and version -----------------------------------------------------------------

TEST(PeInfo, TheBuiltHandler) {
  USHORT machine = 0;
  ASSERT_HRESULT_SUCCEEDED(ReadPeMachine(BuiltDll(), &machine));
  EXPECT_EQ(machine, kOwnMachine);
  ASSERT_HRESULT_SUCCEEDED(ReadPeMachine(BuiltPdfium(), &machine));
  EXPECT_EQ(machine, kOwnMachine);
  std::wstring version;
  const HRESULT hr = ReadFileVersionString(BuiltDll(), &version);
  if (test::IsApplicationControlBlock(hr)) {
    GTEST_SKIP() << "Application Control refused to map the handler as a resource ("
                 << WideToUtf8Lossy(HResultText(hr)) << ")";
  }
  ASSERT_HRESULT_SUCCEEDED(hr);
  EXPECT_EQ(version, kFileVersion);
}

TEST(PeInfo, ASystemDll) {
  USHORT processMachine = 0;
  USHORT nativeMachine = 0;
  ASSERT_TRUE(IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine));
  USHORT machine = 0;
  ASSERT_HRESULT_SUCCEEDED(ReadPeMachine(System32File(L"kernel32.dll"), &machine));
  // System32 holds the native architecture's DLLs (ARM64X ones on ARM64, whose header says ARM64).
  EXPECT_EQ(MachineToArch(machine), MachineToArch(nativeMachine));
  std::wstring version;
  ASSERT_HRESULT_SUCCEEDED(ReadFileVersionString(System32File(L"kernel32.dll"), &version));
  ASSERT_FALSE(version.empty());
  EXPECT_TRUE(version[0] >= L'0' && version[0] <= L'9') << WideToUtf8Lossy(version);
}

TEST(PeInfo, FilesThatAreNotDlls) {
  test::TempDir temp(L"pe");
  ASSERT_TRUE(temp.ok());
  USHORT machine = 0;
  std::wstring version;

  const std::wstring text = temp.File(L"text.dll");
  WriteBytes(text, std::string(4096, 'x'));
  EXPECT_EQ(ReadPeMachine(text, &machine), HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT));
  EXPECT_HRESULT_FAILED(ReadFileVersionString(text, &version));

  // The handler cut short inside its headers.
  const std::wstring truncated = temp.File(L"truncated.dll");
  WriteBytes(truncated, ReadBytes(BuiltDll(), 100));
  EXPECT_EQ(ReadPeMachine(truncated, &machine), HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));

  const std::wstring empty = temp.File(L"empty.dll");
  WriteBytes(empty, "");
  EXPECT_EQ(ReadPeMachine(empty, &machine), HRESULT_FROM_WIN32(ERROR_HANDLE_EOF));

  EXPECT_EQ(ReadPeMachine(temp.File(L"missing.dll"), &machine),
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
  EXPECT_HRESULT_FAILED(ReadPeMachine(temp.path(), &machine));  // a folder
  EXPECT_HRESULT_FAILED(ReadFileVersionString(temp.File(L"missing.dll"), &version));
  // A device is not a file on disk, and is never read.
  EXPECT_HRESULT_FAILED(ReadPeMachine(L"\\\\.\\NUL", &machine));
  EXPECT_EQ(machine, IMAGE_FILE_MACHINE_UNKNOWN);
}

// ---- build ids ------------------------------------------------------------------------------

TEST(BuildIds, HashingFilesMatchesHashingBytes) {
  test::TempDir temp(L"hash");
  ASSERT_TRUE(temp.ok());
  const std::string content(200000, 'p');  // more than one hashing chunk
  WriteBytes(temp.File(L"f.bin"), content);
  Sha256Digest fromFile = {};
  Sha256Digest fromBytes = {};
  ASSERT_HRESULT_SUCCEEDED(Sha256File(temp.File(L"f.bin"), &fromFile));
  ASSERT_HRESULT_SUCCEEDED(Sha256Bytes(
      std::span<const BYTE>(reinterpret_cast<const BYTE*>(content.data()), content.size()),
      &fromBytes));
  EXPECT_EQ(fromFile, fromBytes);
  EXPECT_HRESULT_FAILED(Sha256File(temp.File(L"missing.bin"), &fromFile));
}

TEST(BuildIds, TheFixedVectorOfTheBuildScript) {
  test::TempDir temp(L"buildid");
  ASSERT_TRUE(temp.ok());
  WriteBytes(temp.File(L"a.bin"), "build-id test vector: dll");
  WriteBytes(temp.File(L"b.bin"), "build-id test vector: pdfium");
  std::wstring name;
  ASSERT_HRESULT_SUCCEEDED(
      ComputeBuildId(temp.File(L"a.bin"), temp.File(L"b.bin"), &name, L"1.2.3.4"));
  EXPECT_EQ(name, L"1.2.3.4-ff226b2d");
  // Without a version, it comes from the DLL, and these files have none.
  EXPECT_HRESULT_FAILED(ComputeBuildId(temp.File(L"a.bin"), temp.File(L"b.bin"), &name));
  EXPECT_TRUE(name.empty());
}

TEST(BuildIds, TheBuiltDllsAgreeWithTheBuildScript) {
  std::wstring name;
  const HRESULT hr = ComputeBuildId(BuiltDll(), BuiltPdfium(), &name);
  if (test::IsApplicationControlBlock(hr)) {
    GTEST_SKIP() << "Application Control refused to map the handler as a resource";
  }
  ASSERT_HRESULT_SUCCEEDED(hr);
  const std::optional<BuildFolderNameParts> parts = ParseBuildFolderName(name);
  ASSERT_TRUE(parts) << WideToUtf8Lossy(name);
  EXPECT_EQ(parts->version, kFileVersion);

  const std::optional<std::wstring> cmake = Environment(L"PDFT_TEST_CMAKE");
  const std::optional<std::wstring> script = Environment(L"PDFT_TEST_BUILDID_SCRIPT");
  if (!cmake || !script) {
    GTEST_SKIP() << "no cmake and BuildId.cmake to compare with (only the build tree has them)";
  }
  test::TempDir temp(L"buildid");
  ASSERT_TRUE(temp.ok());
  const std::wstring output = temp.File(L"id.txt");
  const std::wstring arguments = test::QuoteArgument(L"-DDLL=" + BuiltDll()) + L" " +
                                 test::QuoteArgument(L"-DPDFIUM=" + BuiltPdfium()) + L" " +
                                 test::QuoteArgument(L"-DOUTPUT_FILE=" + output) + L" -P " +
                                 test::QuoteArgument(*script);
  const test::CapturedRun run = test::RunCaptured(*cmake, arguments, 60000);
  ASSERT_HRESULT_SUCCEEDED(run.hr);
  ASSERT_EQ(run.exitCode, 0u) << run.out << run.err;
  std::wstring expected;
  ASSERT_HRESULT_SUCCEEDED(Utf8ToWide(ReadBytes(output), &expected));
  EXPECT_EQ(name, expected);
}

TEST(BuildIds, TheInstalled11Release) {
  // 1.x installed its release build machine-wide here on the developer's PC; read only.
  std::filesystem::path root;
  ASSERT_HRESULT_SUCCEEDED(GetLegacyMachineRoot(&root));
  const std::filesystem::path folder = root / L"app" / L"1.1.0.0-67a1323e";
  const std::filesystem::path dll = folder / (std::wstring(kLegacyRootName) + L".dll");
  const std::filesystem::path pdfium = folder / (std::wstring(kLegacyRootName) + L"-pdfium.dll");
  if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES ||
      GetFileAttributesW(pdfium.c_str()) == INVALID_FILE_ATTRIBUTES) {
    GTEST_SKIP() << "the 1.1 release is not installed here";
  }
  std::wstring name;
  ASSERT_HRESULT_SUCCEEDED(ComputeBuildId(dll, pdfium, &name));
  EXPECT_EQ(name, L"1.1.0.0-67a1323e");
}

// ---- build folders --------------------------------------------------------------------------

TEST(BuildFolders, ListsFoldersAndSkipsLinks) {
  test::TempDir temp(L"apps");
  ASSERT_TRUE(temp.ok());
  const std::wstring app = temp.File(L"app");
  const std::wstring v1Dll = std::wstring(kLegacyRootName) + L".dll";
  const std::wstring v1Pdfium = std::wstring(kLegacyRootName) + L"-pdfium.dll";
  for (const std::wstring& file :
       {app + L"\\2.0.0.0-1a2b3c4d\\" + kHandlerDll, app + L"\\2.0.0.0-1a2b3c4d\\" + kPdfiumDll,
        app + L"\\1.1.0.0-67a1323e\\" + v1Dll, app + L"\\1.1.0.0-67a1323e\\" + v1Pdfium,
        app + L"\\2.0.0.0-deadbeef\\" + kHandlerDll, temp.File(L"elsewhere\\") + kHandlerDll,
        temp.File(L"elsewhere\\") + kPdfiumDll}) {
    ASSERT_TRUE(MakeFolders(std::filesystem::path(file).parent_path()));
    WriteBytes(file, "x");
  }
  ASSERT_TRUE(MakeFolders(app + L"\\junk"));
  ASSERT_TRUE(MakeFolders(app + L"\\2.0.0.0-dddddddd\\" + kHandlerDll));  // a folder, not a file
  WriteBytes(app + L"\\2.0.0.0-dddddddd\\" + kPdfiumDll, "x");
  WriteBytes(app + L"\\notes.txt", "x");
  ASSERT_TRUE(MakeFolders(app + L"\\2.0.0.0-00000000"));
  ASSERT_TRUE(test::MakeJunction(app + L"\\2.0.0.0-00000000", temp.File(L"elsewhere")));

  std::vector<BuildFolder> folders;
  std::vector<std::filesystem::path> skipped;
  ASSERT_HRESULT_SUCCEEDED(ListBuildFolders(app, &folders, &skipped));
  ASSERT_EQ(folders.size(), 5u);
  const struct {
    const wchar_t* name;
    const wchar_t* version;
    bool is1x;
    bool complete;
  } expected[] = {
      {L"1.1.0.0-67a1323e", L"1.1.0.0", true, true},
      {L"2.0.0.0-1a2b3c4d", L"2.0.0.0", false, true},
      {L"2.0.0.0-dddddddd", L"2.0.0.0", false, false},
      {L"2.0.0.0-deadbeef", L"2.0.0.0", false, false},
      {L"junk", L"", false, false},
  };
  for (size_t i = 0; i < folders.size(); ++i) {
    EXPECT_EQ(folders[i].name, expected[i].name);
    EXPECT_EQ(folders[i].version, expected[i].version) << i;
    EXPECT_EQ(folders[i].is1x, expected[i].is1x) << i;
    EXPECT_EQ(folders[i].complete, expected[i].complete) << i;
    EXPECT_EQ(folders[i].dir.native(), app + L"\\" + expected[i].name);
  }
  ASSERT_EQ(skipped.size(), 1u);
  EXPECT_EQ(skipped[0].native(), app + L"\\2.0.0.0-00000000");

  // No app folder at all, and an app folder that is itself a link.
  ASSERT_HRESULT_SUCCEEDED(ListBuildFolders(temp.File(L"none"), &folders, &skipped));
  EXPECT_TRUE(folders.empty());
  EXPECT_TRUE(skipped.empty());
  ASSERT_TRUE(MakeFolders(temp.File(L"linked")));
  ASSERT_TRUE(test::MakeJunction(temp.File(L"linked"), app));
  ASSERT_HRESULT_SUCCEEDED(ListBuildFolders(temp.File(L"linked"), &folders, &skipped));
  EXPECT_TRUE(folders.empty());
  ASSERT_EQ(skipped.size(), 1u);
  EXPECT_EQ(skipped[0].native(), temp.File(L"linked"));
}

// ---- 1.x leftovers --------------------------------------------------------------------------

TEST(LegacyState, AFake1xInstall) {
  test::TempDir temp(L"legacy");
  ASSERT_TRUE(temp.ok());
  test::ScopedRegistrySandbox sandbox;
  ASSERT_TRUE(sandbox.ok()) << HResultText(sandbox.error());
  const RegistryRoots roots{sandbox.user(), sandbox.machine()};
  const std::wstring user = temp.File(L"user");
  const std::wstring machine = temp.File(L"machine");
  const std::wstring script = std::wstring(kLegacyRootName) + L".ps1";
  const std::wstring v1Dll = std::wstring(kLegacyRootName) + L".dll";
  for (const std::wstring& file :
       {user + L"\\" + script, user + L"\\README.md", user + L"\\repair.log",
        user + L"\\bin\\" + v1Dll, user + L"\\licenses\\zlib.txt",
        user + L"\\app\\1.1.0.0-67a1323e\\" + v1Dll, user + L"\\app\\2.0.0.0-1a2b3c4d\\" + v1Dll,
        user + L"\\notes.txt", machine + L"\\app\\1.1.0.0-67a1323e\\" + v1Dll}) {
    ASSERT_TRUE(MakeFolders(std::filesystem::path(file).parent_path()));
    WriteBytes(file, "x");
  }
  WriteBytes(user + L"\\settings.ini", "[Thumbnails]\r\nZoom=130\r\n");
  ASSERT_EQ(
      RegSetKeyValueW(sandbox.user(), kRunOnceKey, kLegacyRunOnce, REG_SZ, L"cmd", sizeof(L"cmd")),
      ERROR_SUCCESS);
  ASSERT_HRESULT_SUCCEEDED(
      WriteClass(roots, Scope::kUser, user + L"\\app\\1.1.0.0-67a1323e\\" + v1Dll));
  const std::wstring machineDll = machine + L"\\app\\1.1.0.0-67a1323e\\" + v1Dll;
  ASSERT_EQ(RegSetKeyValueW(sandbox.machine(), (ClassKeyPath() + L"\\InprocServer32").c_str(),
                            nullptr, REG_SZ, machineDll.c_str(),
                            static_cast<DWORD>((machineDll.size() + 1) * sizeof(wchar_t))),
            ERROR_SUCCESS);

  LegacyState state;
  ASSERT_HRESULT_SUCCEEDED(ReadLegacyState(roots, user, machine, &state));
  EXPECT_TRUE(state.runOnce);
  EXPECT_FALSE(state.legacyTask);  // the task reader's to fill in
  EXPECT_TRUE(state.ini);
  EXPECT_EQ(state.iniZoom, 130);
  std::vector<std::wstring> files;
  for (const std::filesystem::path& file : state.files) files.push_back(file.native());
  const std::vector<std::wstring> expected = {user + L"\\app\\1.1.0.0-67a1323e",
                                              user + L"\\bin",
                                              user + L"\\licenses",
                                              user + L"\\" + script,
                                              user + L"\\README.md",
                                              user + L"\\repair.log",
                                              user + L"\\settings.ini"};
  EXPECT_EQ(files, expected);
  EXPECT_TRUE(state.userClassInV1);
  EXPECT_TRUE(state.machineClassInV1);
  EXPECT_TRUE(HasV1Leftovers(state));
}

TEST(LegacyState, NothingOf1x) {
  test::TempDir temp(L"legacy");
  ASSERT_TRUE(temp.ok());
  test::ScopedRegistrySandbox sandbox;
  ASSERT_TRUE(sandbox.ok()) << HResultText(sandbox.error());
  const RegistryRoots roots{sandbox.user(), sandbox.machine()};
  LegacyState state;
  ASSERT_HRESULT_SUCCEEDED(ReadLegacyState(roots, temp.File(L"none"), temp.File(L"none2"), &state));
  EXPECT_FALSE(HasV1Leftovers(state));
  EXPECT_FALSE(state.iniZoom);

  // A per-user folder that is a link is not looked into.
  ASSERT_TRUE(MakeFolders(temp.File(L"real\\bin")));
  WriteBytes(temp.File(L"real\\repair.log"), "x");
  ASSERT_TRUE(MakeFolders(temp.File(L"link")));
  ASSERT_TRUE(test::MakeJunction(temp.File(L"link"), temp.File(L"real")));
  UserFolderListing listing;
  ASSERT_HRESULT_SUCCEEDED(ListUserFolder(temp.File(L"link"), &listing));
  EXPECT_TRUE(listing.root.empty());
  ASSERT_HRESULT_SUCCEEDED(ListUserFolder(temp.File(L"real"), &listing));
  EXPECT_EQ(listing.root.size(), 2u);
}

}  // namespace
}  // namespace pdft
