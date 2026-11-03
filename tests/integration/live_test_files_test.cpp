// The files around the Explorer test (live_test.h): writing the probe PDF, sweeping old probes
// (never through a link or a junction), and checking a --test-file path. No test here extracts a
// thumbnail: that would start a thumbnail surrogate and load whatever handler is registered on the
// PC, so the extraction is left to the check's own runs, and the code that gathers the state is
// tested with a fake (SystemProbes).

#include <windows.h>

#include <gtest/gtest.h>
#include <objbase.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "live_test.h"
#include "paths.h"
#include "project_config.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

std::string ReadAll(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

void Touch(const std::wstring& path) { std::ofstream(path, std::ios::binary) << "x"; }

// Sets the last write time of |path| to |ageHours| ago. |flags|: FILE_FLAG_BACKUP_SEMANTICS for a
// folder, FILE_FLAG_OPEN_REPARSE_POINT for a link itself rather than its target.
void Age(const std::wstring& path, int ageHours, DWORD flags = 0) {
  HANDLE handle = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, flags, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE) << GetLastError();
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER time;
  time.LowPart = now.dwLowDateTime;
  time.HighPart = now.dwHighDateTime;
  time.QuadPart -= static_cast<ULONGLONG>(ageHours) * 60 * 60 * 10'000'000;
  FILETIME then;
  then.dwLowDateTime = time.LowPart;
  then.dwHighDateTime = time.HighPart;
  EXPECT_TRUE(SetFileTime(handle, nullptr, nullptr, &then));
  CloseHandle(handle);
}

std::wstring ProbeName() {
  GUID id = {};
  EXPECT_HRESULT_SUCCEEDED(CoCreateGuid(&id));
  return ProbeFileName(id);
}

// |path| is there, itself: a link counts, wherever it leads.
bool Exists(const std::wstring& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsReparsePoint(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

TEST(ProbeFile, WrittenToTempWithThe1xBytes) {
  std::filesystem::path probe;
  ASSERT_HRESULT_SUCCEEDED(WriteProbePdf(&probe));
  const bool inTemp = [&] {
    wchar_t temp[MAX_PATH + 1] = {};
    const DWORD length = GetTempPathW(ARRAYSIZE(temp), temp);
    std::filesystem::path canonicalTemp;
    std::filesystem::path canonicalParent;
    return length > 0 && SUCCEEDED(Canonicalize(temp, &canonicalTemp)) &&
           SUCCEEDED(Canonicalize(probe.parent_path(), &canonicalParent)) &&
           IEquals(canonicalTemp.native(), canonicalParent.native());
  }();
  const std::string bytes = ReadAll(probe);
  const bool deleted = DeleteFileW(probe.c_str()) != FALSE;
  EXPECT_TRUE(inTemp) << probe.native();
  EXPECT_TRUE(IsProbeFileName(probe.filename().native())) << probe.native();
  EXPECT_EQ(bytes, ProbePdfBytes());
  EXPECT_TRUE(deleted);
}

TEST(ProbeFile, EveryProbeHasANewName) {
  std::filesystem::path first;
  std::filesystem::path second;
  ASSERT_HRESULT_SUCCEEDED(WriteProbePdf(&first));
  const HRESULT hr = WriteProbePdf(&second);
  DeleteFileW(first.c_str());
  if (SUCCEEDED(hr)) DeleteFileW(second.c_str());
  ASSERT_HRESULT_SUCCEEDED(hr);
  EXPECT_NE(first, second);
}

TEST(ProbeFile, SweepTakesOnlyOldProbes) {
  test::TempDir temp(L"probes");
  ASSERT_TRUE(temp.ok());
  const std::wstring old = temp.File(ProbeName());
  const std::wstring fresh = temp.File(ProbeName());
  const std::wstring oldButNotAProbe = temp.File(std::wstring(kProductName) + L"-probe-x.pdf");
  const std::wstring oldOther = temp.File(L"other.pdf");
  const std::wstring oldFolder = temp.File(ProbeName());
  Touch(old);
  Touch(fresh);
  Touch(oldButNotAProbe);
  Touch(oldOther);
  ASSERT_TRUE(CreateDirectoryW(oldFolder.c_str(), nullptr));
  Age(old, 2);
  Age(oldButNotAProbe, 2);
  Age(oldOther, 2);
  Age(oldFolder, 2, FILE_FLAG_BACKUP_SEMANTICS);

  int removed = -1;
  ASSERT_HRESULT_SUCCEEDED(SweepStaleProbes(temp.path(), &removed));
  EXPECT_EQ(removed, 1);
  EXPECT_FALSE(Exists(old));
  EXPECT_TRUE(Exists(fresh));
  EXPECT_TRUE(Exists(oldButNotAProbe));
  EXPECT_TRUE(Exists(oldOther));
  EXPECT_TRUE(Exists(oldFolder));

  // A folder that is not there is nothing to sweep.
  ASSERT_HRESULT_SUCCEEDED(SweepStaleProbes(temp.File(L"missing"), &removed));
  EXPECT_EQ(removed, 0);
}

// A folder that is a junction is not swept at all: nothing is deleted through it, not even a
// stale probe in the folder it leads to. The folder itself is swept as usual.
TEST(ProbeFile, SweepStopsAtAFolderThatIsAJunction) {
  test::TempDir temp(L"probes");
  ASSERT_TRUE(temp.ok());
  const std::wstring real = temp.File(L"real");
  ASSERT_TRUE(CreateDirectoryW(real.c_str(), nullptr));
  const std::wstring probe = real + L"\\" + ProbeName();
  Touch(probe);
  Age(probe, 2);
  const std::wstring link = temp.File(L"link");
  ASSERT_TRUE(CreateDirectoryW(link.c_str(), nullptr));
  ASSERT_TRUE(test::MakeJunction(link, real));

  int removed = -1;
  EXPECT_EQ(SweepStaleProbes(link, &removed), HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  EXPECT_EQ(removed, 0);
  EXPECT_TRUE(Exists(probe));

  ASSERT_HRESULT_SUCCEEDED(SweepStaleProbes(real, &removed));
  EXPECT_EQ(removed, 1);
  EXPECT_FALSE(Exists(probe));
}

// A junction with a probe's name is left alone, and so is the stale probe in the folder it leads
// to, however old the junction is.
TEST(ProbeFile, SweepLeavesAJunctionNamedLikeAProbe) {
  test::TempDir temp(L"probes");
  ASSERT_TRUE(temp.ok());
  const std::wstring target = temp.File(L"target");
  ASSERT_TRUE(CreateDirectoryW(target.c_str(), nullptr));
  const std::wstring behind = target + L"\\" + ProbeName();
  Touch(behind);
  Age(behind, 2);
  const std::wstring sweep = temp.File(L"sweep");
  ASSERT_TRUE(CreateDirectoryW(sweep.c_str(), nullptr));
  const std::wstring junction = sweep + L"\\" + ProbeName();
  ASSERT_TRUE(CreateDirectoryW(junction.c_str(), nullptr));
  ASSERT_TRUE(test::MakeJunction(junction, target));
  Age(junction, 2, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);

  int removed = -1;
  ASSERT_HRESULT_SUCCEEDED(SweepStaleProbes(sweep, &removed));
  EXPECT_EQ(removed, 0);
  EXPECT_TRUE(IsReparsePoint(junction));
  EXPECT_TRUE(Exists(behind));
}

// A file link with a probe's name is never deleted, nor the file it leads to, however old both
// are. Making the link needs the right to create symbolic links (elevated, as on CI, or Developer
// Mode); without it the test is skipped.
TEST(ProbeFile, SweepLeavesAFileLinkNamedLikeAProbe) {
  test::TempDir temp(L"probes");
  ASSERT_TRUE(temp.ok());
  const std::wstring target = temp.File(L"target.pdf");
  Touch(target);
  Age(target, 2);
  const std::wstring link = temp.File(ProbeName());
  if (!CreateSymbolicLinkW(link.c_str(), target.c_str(),
                           SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    const DWORD error = GetLastError();
    GTEST_SKIP() << "no symbolic link can be made here (error " << error << ")";
  }
  Age(link, 2, FILE_FLAG_OPEN_REPARSE_POINT);

  int removed = -1;
  ASSERT_HRESULT_SUCCEEDED(SweepStaleProbes(temp.path(), &removed));
  EXPECT_EQ(removed, 0);
  EXPECT_TRUE(IsReparsePoint(link));
  EXPECT_TRUE(Exists(target));
}

TEST(ProbeFile, RunRefusesSizesBeforeWritingAnything) {
  LiveTest result;
  EXPECT_EQ(RunLiveTest(0, nullptr, 1000, &result), E_INVALIDARG);
  EXPECT_EQ(RunLiveTest(2561, nullptr, 1000, &result), E_INVALIDARG);
}

TEST(TestFile, ARegularFileIsUsedByItsFinalPath) {
  test::TempDir temp(L"testfile");
  ASSERT_TRUE(temp.ok());
  const std::wstring real = temp.File(L"real");
  ASSERT_TRUE(CreateDirectoryW(real.c_str(), nullptr));
  const std::wstring file = real + L"\\a.pdf";
  Touch(file);

  std::filesystem::path resolved;
  TestFileProblem problem = TestFileProblem::kUnreadable;
  ASSERT_HRESULT_SUCCEEDED(ValidateTestFile(file, &resolved, &problem));
  EXPECT_EQ(problem, TestFileProblem::kNone);
  std::filesystem::path expected;
  ASSERT_HRESULT_SUCCEEDED(Canonicalize(file, &expected));
  EXPECT_EQ(resolved, expected);

  // Through a junction: the file it ends at.
  const std::wstring link = temp.File(L"link");
  ASSERT_TRUE(CreateDirectoryW(link.c_str(), nullptr));
  ASSERT_TRUE(test::MakeJunction(link, real));
  ASSERT_HRESULT_SUCCEEDED(ValidateTestFile(link + L"\\a.pdf", &resolved, &problem));
  EXPECT_EQ(problem, TestFileProblem::kNone);
  EXPECT_EQ(resolved, expected);
}

TEST(TestFile, WhatIsRefused) {
  test::TempDir temp(L"testfile");
  ASSERT_TRUE(temp.ok());
  std::filesystem::path resolved;
  TestFileProblem problem = TestFileProblem::kNone;

  EXPECT_HRESULT_FAILED(ValidateTestFile(temp.path(), &resolved, &problem));
  EXPECT_EQ(problem, TestFileProblem::kDirectory);
  EXPECT_TRUE(resolved.empty());

  EXPECT_HRESULT_FAILED(ValidateTestFile(temp.File(L"missing.pdf"), &resolved, &problem));
  EXPECT_EQ(problem, TestFileProblem::kNotFound);
  EXPECT_HRESULT_FAILED(ValidateTestFile(temp.File(L"missing\\a.pdf"), &resolved, &problem));
  EXPECT_EQ(problem, TestFileProblem::kNotFound);

  EXPECT_HRESULT_FAILED(ValidateTestFile(L"", &resolved, &problem));
  EXPECT_EQ(problem, TestFileProblem::kEmpty);

  EXPECT_HRESULT_FAILED(ValidateTestFile(L"\\\\.\\NUL", &resolved, &problem));
  EXPECT_EQ(problem, TestFileProblem::kDevicePath);
  EXPECT_HRESULT_FAILED(ValidateTestFile(L"\\\\.\\pipe\\anything", &resolved, &problem));
  EXPECT_EQ(problem, TestFileProblem::kDevicePath);

  // A reserved device name: a device path once made absolute, or a device when opened.
  EXPECT_HRESULT_FAILED(ValidateTestFile(L"NUL", &resolved, &problem));
  EXPECT_TRUE(problem == TestFileProblem::kDevicePath || problem == TestFileProblem::kNotDiskFile)
      << static_cast<int>(problem);
  EXPECT_TRUE(resolved.empty());
}

}  // namespace
}  // namespace pdft
