// Build folder names and build ids (build_folder.h): the pure parts, and the formula against two
// known vectors. Hashes byte strings only; no files.

#include <windows.h>

#include "build_folder.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>

namespace pdft {
namespace {

Sha256Digest FromHex(std::string_view hex) {
  Sha256Digest digest = {};
  for (size_t i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<BYTE>(std::stoul(std::string(hex.substr(2 * i, 2)), nullptr, 16));
  }
  return digest;
}

Sha256Digest Sha(std::string_view text) {
  Sha256Digest digest = {};
  EXPECT_HRESULT_SUCCEEDED(Sha256Bytes(
      std::span<const BYTE>(reinterpret_cast<const BYTE*>(text.data()), text.size()), &digest));
  return digest;
}

// The two files of cmake/gates/CheckBuildId.cmake's fixed vector, as SHA-256 digests (computed
// independently with .NET's SHA256).
constexpr std::string_view kVectorDll =
    "21AA0E13DA4380F561E79BFCCE13C72B06417B238EC0891369CB221CD1AE5A24";
constexpr std::string_view kVectorPdfium =
    "DB386F6A4A6C68C2245D50AF6B4354AFE06B4C23C52266110A6A0B6436F6C946";

// The 1.1 release's two DLLs (C:\Program Files\PdfThumbnails\app\1.1.0.0-67a1323e on the
// developer's PC), whose folder 1.x named 1.1.0.0-67a1323e.
constexpr std::string_view kV11Dll =
    "C6F0E6CC2A39D30CA7FC65E5AF5114B63E22D6A7F64FC6096BE9AB4D6DC20777";
constexpr std::string_view kV11Pdfium =
    "4B0DEC92555D69437BEF0DF37BE630181AB3E83A6823CA264EDE355A4F2EF994";

TEST(BuildId, Sha256OfBytes) {
  EXPECT_EQ(Sha("build-id test vector: dll"), FromHex(kVectorDll));
  EXPECT_EQ(Sha("build-id test vector: pdfium"), FromHex(kVectorPdfium));
  EXPECT_EQ(Sha(""), FromHex("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"));
}

TEST(BuildId, HashInputIsBothDigestsInUpperCaseHex) {
  const std::string input = BuildIdHashInput(FromHex(kVectorDll), FromHex(kVectorPdfium));
  EXPECT_EQ(input, std::string(kVectorDll) + std::string(kVectorPdfium));
  EXPECT_EQ(input.size(), 128u);
}

TEST(BuildId, FolderNameIsVersionAndFourBytesInLowerCaseHex) {
  const Sha256Digest digest = FromHex(kVectorDll);
  EXPECT_EQ(BuildFolderName(L"2.0.0.0", digest), L"2.0.0.0-21aa0e13");
}

TEST(BuildId, KnownVectors) {
  std::wstring name;
  ASSERT_HRESULT_SUCCEEDED(
      BuildIdFromHashes(L"1.2.3.4", FromHex(kVectorDll), FromHex(kVectorPdfium), &name));
  EXPECT_EQ(name, L"1.2.3.4-ff226b2d");  // what cmake/BuildId.cmake gives for the same files
  ASSERT_HRESULT_SUCCEEDED(
      BuildIdFromHashes(L"1.1.0.0", FromHex(kV11Dll), FromHex(kV11Pdfium), &name));
  EXPECT_EQ(name, L"1.1.0.0-67a1323e");  // 1.x's own name for its release build
  // The order matters: the DLL first.
  ASSERT_HRESULT_SUCCEEDED(
      BuildIdFromHashes(L"1.1.0.0", FromHex(kV11Pdfium), FromHex(kV11Dll), &name));
  EXPECT_NE(name, L"1.1.0.0-67a1323e");
}

TEST(BuildId, RefusesVersionsThatCouldNotNameAFolder) {
  for (const wchar_t* version : {L"", L"..", L"1.1.0.0\\..", L"1..0", L".1", L"1.", L"1.2.3.4.5",
                                 L"123456", L"1.0 (beta)", L"a.b"}) {
    std::wstring name = L"x";
    EXPECT_EQ(BuildIdFromHashes(version, FromHex(kVectorDll), FromHex(kVectorPdfium), &name),
              HRESULT_FROM_WIN32(ERROR_INVALID_DATA))
        << ::testing::PrintToString(std::wstring(version));
    EXPECT_TRUE(name.empty());
  }
}

TEST(BuildId, IsBuildVersion) {
  for (const wchar_t* good : {L"1", L"2.0", L"1.1.0.0", L"2.0.0.0", L"10.0.26100.1", L"65535.0"}) {
    EXPECT_TRUE(IsBuildVersion(good)) << ::testing::PrintToString(std::wstring(good));
  }
  for (const wchar_t* bad :
       {L"", L".", L"1.", L".1", L"1..1", L"1.2.3.4.5", L"123456.0", L"1/2", L"1-2", L"\x0661.0"}) {
    EXPECT_FALSE(IsBuildVersion(bad)) << ::testing::PrintToString(std::wstring(bad));
  }
}

TEST(BuildId, ParseBuildFolderName) {
  const std::optional<BuildFolderNameParts> parts = ParseBuildFolderName(L"1.1.0.0-67a1323e");
  ASSERT_TRUE(parts);
  EXPECT_EQ(parts->version, L"1.1.0.0");
  EXPECT_EQ(parts->id, L"67a1323e");
  EXPECT_TRUE(ParseBuildFolderName(L"2.0.0.0-ABCDEF01"));  // folder names compare ignoring case
  for (const wchar_t* bad :
       {L"", L"2.0.0.0", L"2.0.0.0-", L"2.0.0.0-1234567", L"2.0.0.0-123456789", L"2.0.0.0-1234567g",
        L"-12345678", L"x-12345678", L"2.0-beta-12345678", L"app"}) {
    EXPECT_FALSE(ParseBuildFolderName(bad)) << ::testing::PrintToString(std::wstring(bad));
  }
}

TEST(BuildId, CompareFileVersions) {
  EXPECT_EQ(CompareFileVersions(L"2.0.0.0", L"2.0.0.0"), 0);
  EXPECT_EQ(CompareFileVersions(L"2.0", L"2.0.0.0"), 0);
  EXPECT_LT(CompareFileVersions(L"1.9", L"1.10"), 0);
  EXPECT_GT(CompareFileVersions(L"2.0.0.1", L"2.0.0.0"), 0);
  EXPECT_LT(CompareFileVersions(L"1.1.0.0", L"2.0.0.0"), 0);
  EXPECT_GT(CompareFileVersions(L"10.0", L"9.99"), 0);
  EXPECT_EQ(CompareFileVersions(L"", L"0.0"), 0);
  // Saturates instead of wrapping around.
  EXPECT_EQ(CompareFileVersions(L"99999999999999999999", L"99999999999999999998"), 0);
}

}  // namespace
}  // namespace pdft
