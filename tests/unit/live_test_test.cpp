// The pure parts of the Explorer test (live_test.h): when it is skipped, the probe PDF's bytes,
// probe file names and their age, and the checks of a --test-file path. Nothing here extracts a
// thumbnail; writing and sweeping probe files is in the integration tests.

#include <windows.h>

#include "live_test.h"

#include <gtest/gtest.h>

#include <cwctype>
#include <string>

#include "project_config.h"

namespace pdft {
namespace {

FILETIME At(ULONGLONG ticks) {
  FILETIME time;
  time.dwLowDateTime = static_cast<DWORD>(ticks);
  time.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
  return time;
}

TEST(LiveTestSkip, Order) {
  LiveTestConditions conditions;
  conditions.effectiveIsOurs = true;
  EXPECT_EQ(DecideLiveTestSkip(conditions), LiveTestSkip::kNone);

  conditions.effectiveIsOurs = false;
  EXPECT_EQ(DecideLiveTestSkip(conditions), LiveTestSkip::kNotOurHandler);
  conditions.testFile = true;  // a test file shows what Explorer does with it, whoever handles it
  EXPECT_EQ(DecideLiveTestSkip(conditions), LiveTestSkip::kNone);

  // No class at all: the probe would only say so; a test file still runs.
  conditions = {};
  conditions.effectiveIsOurs = true;
  conditions.classRegistered = false;
  EXPECT_EQ(DecideLiveTestSkip(conditions), LiveTestSkip::kNoClass);
  conditions.effectiveIsOurs = false;
  EXPECT_EQ(DecideLiveTestSkip(conditions), LiveTestSkip::kNotOurHandler);
  conditions.testFile = true;
  EXPECT_EQ(DecideLiveTestSkip(conditions), LiveTestSkip::kNone);

  // Elevated or virtualized: skipped for the probe and for a test file alike.
  for (const bool testFile : {false, true}) {
    LiveTestConditions c;
    c.testFile = testFile;
    c.effectiveIsOurs = true;
    c.virtualized = true;
    EXPECT_EQ(DecideLiveTestSkip(c), LiveTestSkip::kVirtualized);
    c.elevated = true;
    EXPECT_EQ(DecideLiveTestSkip(c), LiveTestSkip::kElevated);
    c.requested = false;
    EXPECT_EQ(DecideLiveTestSkip(c), LiveTestSkip::kNotRequested);
  }
}

TEST(LiveTestFlags, OutOfProcessAsIn1x) {
  EXPECT_EQ(kLiveTestFlags, 0x424u);
  EXPECT_EQ(kLiveTestFlags & 0x100u, 0u);  // WTS_EXTRACTINPROC
  EXPECT_EQ(kProbeThumbnailSize, 96u);
  EXPECT_EQ(kTestFileThumbnailSize, 256u);
}

TEST(ProbePdf, BytesOf1x) {
  // What 1.x's Test-ExplorerExtraction wrote (ASCII, LF), byte for byte: 329 bytes.
  const std::string expected =
      "%PDF-1.4\n"
      "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n"
      "2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj\n"
      "3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 200] >>\nendobj\n"
      "xref\n0 4\n"
      "0000000000 65535 f \n"
      "0000000009 00000 n \n"
      "0000000058 00000 n \n"
      "0000000115 00000 n \n"
      "trailer\n<< /Size 4 /Root 1 0 R >>\nstartxref\n186\n%%EOF\n";
  const std::string bytes = ProbePdfBytes();
  EXPECT_EQ(bytes, expected);
  EXPECT_EQ(bytes.size(), 329u);
}

TEST(ProbePdf, OffsetsPointAtTheObjects) {
  const std::string pdf = ProbePdfBytes();
  // Each xref entry is 20 bytes; entry n gives object n's offset.
  const size_t xref = pdf.find("xref\n");
  ASSERT_NE(xref, std::string::npos);
  const size_t table = pdf.find("0000000000 65535 f \n", xref);
  ASSERT_NE(table, std::string::npos);
  for (int object = 1; object <= 3; ++object) {
    const std::string entry = pdf.substr(table + 20 * object, 20);
    ASSERT_EQ(entry.substr(10), " 00000 n \n");
    const size_t offset = std::stoul(entry.substr(0, 10));
    EXPECT_EQ(pdf.compare(offset, 8, std::to_string(object) + " 0 obj\n"), 0) << object;
  }
  const size_t startxref = pdf.find("startxref\n");
  ASSERT_NE(startxref, std::string::npos);
  EXPECT_EQ(std::stoul(pdf.substr(startxref + 10)), xref);
  EXPECT_TRUE(pdf.ends_with("%%EOF\n"));
  for (const char c : pdf) EXPECT_TRUE(c == '\n' || (c >= 0x20 && c < 0x7F));
}

// The probe name prefix of this build ("<Product>-probe-").
std::wstring Prefix() { return std::wstring(kProductName) + L"-probe-"; }

std::wstring Upper(std::wstring text) {
  for (wchar_t& c : text) c = static_cast<wchar_t>(towupper(c));
  return text;
}

TEST(ProbeNames, AsIn1x) {
  const GUID id = {0x0a1b2c3d, 0x4e5f, 0x6a7b, {0x8c, 0x9d, 0xae, 0xbf, 0xc0, 0xd1, 0xe2, 0xf3}};
  const std::wstring name = ProbeFileName(id);
  EXPECT_EQ(name, Prefix() + L"0a1b2c3d-4e5f-6a7b-8c9d-aebfc0d1e2f3.pdf");
  EXPECT_TRUE(IsProbeFileName(name));
  // 1.x's own probes: [guid]::NewGuid() is lower case too. Names compare ignoring case.
  EXPECT_TRUE(IsProbeFileName(Prefix() + L"3f2504e0-4f89-41d3-9a0c-0305e82c3301.pdf"));
  EXPECT_TRUE(IsProbeFileName(Upper(Prefix() + L"3f2504e0-4f89-41d3-9a0c-0305e82c3301.pdf")));
}

TEST(ProbeNames, OnlyThatExactPattern) {
  const std::wstring guid = L"3f2504e0-4f89-41d3-9a0c-0305e82c3301";
  ASSERT_TRUE(IsProbeFileName(Prefix() + guid + L".pdf"));
  const std::wstring bad[] = {
      L"",
      Prefix() + L".pdf",
      Prefix() + L"3f2504e0-4f89-41d3-9a0c-0305e82c330.pdf",     // one short
      Prefix() + L"3f2504e0-4f89-41d3-9a0c-0305e82c33011.pdf",   // one long
      Prefix() + L"{3f2504e0-4f89-41d3-9a0c-0305e82c3301}.pdf",  // braces
      Prefix() + L"3f2504e0-4f89-41d3-9a0c-0305e82c330g.pdf",    // not hex
      Prefix() + L"3f2504e04-f89-41d3-9a0c-0305e82c3301.pdf",    // dash moved
      Prefix() + guid + L".pdf.txt",
      Prefix() + guid + L".txt",
      Prefix() + guid,
      Prefix() + guid + L" .pdf",
      L"Other-probe-" + guid + L".pdf",
      L"x" + Prefix() + guid + L".pdf",
      std::wstring(kProductName) + L"-prob-" + guid + L".pdf",
      L"PDFTHU~1.PDF",
  };
  for (const std::wstring& name : bad) EXPECT_FALSE(IsProbeFileName(name)) << name;
}

TEST(ProbeNames, Staleness) {
  constexpr ULONGLONG kNow = 134'000'000'000'000'000ULL;
  constexpr ULONGLONG kMinute = 60ULL * 10'000'000;
  EXPECT_FALSE(IsStaleProbe(At(kNow), At(kNow)));
  EXPECT_FALSE(IsStaleProbe(At(kNow - 59 * kMinute), At(kNow)));
  EXPECT_FALSE(IsStaleProbe(At(kNow - kStaleProbeAge100ns), At(kNow)));
  EXPECT_TRUE(IsStaleProbe(At(kNow - kStaleProbeAge100ns - 1), At(kNow)));
  EXPECT_TRUE(IsStaleProbe(At(0), At(kNow)));
  EXPECT_FALSE(IsStaleProbe(At(kNow + 10 * kMinute), At(kNow)));  // from the future: not old
}

TEST(TestFilePath, Syntax) {
  EXPECT_EQ(TestFilePathProblem(L""), TestFileProblem::kEmpty);
  EXPECT_EQ(TestFilePathProblem(std::wstring(L"a.pdf\0b", 7)), TestFileProblem::kBadName);
  for (const wchar_t* device : {L"\\\\.\\NUL", L"\\\\.\\C:", L"\\\\.\\pipe\\x", L"\\\\?\\C:\\a.pdf",
                                L"\\\\?\\GLOBALROOT\\Device\\HarddiskVolume3\\a.pdf",
                                L"\\??\\C:\\a.pdf", L"//./COM1", L"//?/C:/a.pdf", L"\\\\./NUL"}) {
    EXPECT_EQ(TestFilePathProblem(device), TestFileProblem::kDevicePath) << device;
  }
  for (const wchar_t* fine : {L"a.pdf", L"D:\\Home\\a\\b.pdf", L"C:/a/b.pdf", L"..\\a.pdf",
                              L"\\\\server\\share\\a.pdf", L"\\a.pdf", L"\\\\.", L"\\\\.x\\y"}) {
    EXPECT_EQ(TestFilePathProblem(fine), TestFileProblem::kNone) << fine;
  }
}

TEST(TestFilePath, OpenedFile) {
  EXPECT_EQ(OpenedTestFileProblem(FILE_TYPE_DISK, FILE_ATTRIBUTE_ARCHIVE), TestFileProblem::kNone);
  EXPECT_EQ(OpenedTestFileProblem(FILE_TYPE_DISK, FILE_ATTRIBUTE_NORMAL), TestFileProblem::kNone);
  // A cloud placeholder or a file behind a link is still a file.
  EXPECT_EQ(OpenedTestFileProblem(FILE_TYPE_DISK, FILE_ATTRIBUTE_REPARSE_POINT | 0x400000),
            TestFileProblem::kNone);
  EXPECT_EQ(OpenedTestFileProblem(FILE_TYPE_DISK, FILE_ATTRIBUTE_DIRECTORY),
            TestFileProblem::kDirectory);
  EXPECT_EQ(OpenedTestFileProblem(FILE_TYPE_PIPE, 0), TestFileProblem::kNotDiskFile);
  EXPECT_EQ(OpenedTestFileProblem(FILE_TYPE_CHAR, 0), TestFileProblem::kNotDiskFile);
  EXPECT_EQ(OpenedTestFileProblem(FILE_TYPE_UNKNOWN, 0), TestFileProblem::kNotDiskFile);
}

}  // namespace
}  // namespace pdft
