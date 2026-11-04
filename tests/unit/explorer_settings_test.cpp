// The pure parts of the Explorer settings, the Windows build string (explorer_settings.h) and
// PdfCraft's detection (pdfcraft.h). The registry and Windows Installer are read in the
// integration tests.

#include <windows.h>

#include "explorer_settings.h"

#include <gtest/gtest.h>

#include <cstring>
#include <optional>
#include <string>

#include "pdfcraft.h"

namespace pdft {
namespace {

RegistryValue Dword(DWORD number) {
  RegistryValue value;
  value.type = REG_DWORD;
  value.data.resize(sizeof(number));
  std::memcpy(value.data.data(), &number, sizeof(number));
  return value;
}

RegistryValue Text(const std::wstring& text) {
  RegistryValue value;
  value.type = REG_SZ;
  value.data.resize((text.size() + 1) * sizeof(wchar_t));
  std::memcpy(value.data.data(), text.c_str(), value.data.size());
  return value;
}

TEST(ExplorerSettings, IconsOnlyAs1xReadIt) {
  EXPECT_TRUE(IsIconsOnlyValue(Dword(1)));
  EXPECT_FALSE(IsIconsOnlyValue(Dword(0)));
  EXPECT_FALSE(IsIconsOnlyValue(Dword(2)));
  EXPECT_FALSE(IsIconsOnlyValue(Text(L"1")));
  EXPECT_FALSE(IsIconsOnlyValue(std::nullopt));
}

TEST(ExplorerSettings, PolicySwitch) {
  EXPECT_TRUE(IsPolicyOnValue(Dword(1)));
  EXPECT_TRUE(IsPolicyOnValue(Dword(2)));
  EXPECT_FALSE(IsPolicyOnValue(Dword(0)));
  EXPECT_FALSE(IsPolicyOnValue(Text(L"1")));
  EXPECT_FALSE(IsPolicyOnValue(std::nullopt));
  RegistryValue shortDword = Dword(1);
  shortDword.data.resize(1);
  EXPECT_FALSE(IsPolicyOnValue(shortDword));
}

TEST(AutoRestartShell, OnUnlessAZero) {
  // On by default: missing, another type, or any value but a REG_DWORD 0.
  EXPECT_TRUE(IsAutoRestartShellOn(std::nullopt));
  EXPECT_TRUE(IsAutoRestartShellOn(Dword(1)));
  EXPECT_TRUE(IsAutoRestartShellOn(Dword(2)));
  EXPECT_TRUE(IsAutoRestartShellOn(Text(L"0")));
  RegistryValue shortDword = Dword(0);
  shortDword.data.resize(1);
  EXPECT_TRUE(IsAutoRestartShellOn(shortDword));
  // Off only for a REG_DWORD 0.
  EXPECT_FALSE(IsAutoRestartShellOn(Dword(0)));
}

TEST(OsBuild, Format) {
  EXPECT_EQ(FormatOsBuild(L"26300", L"26300", 9818), L"26300.9818");
  EXPECT_EQ(FormatOsBuild(L"26300", std::nullopt, std::nullopt), L"26300");
  EXPECT_EQ(FormatOsBuild(L"26300", std::nullopt, 0), L"26300.0");
  // CurrentBuild missing or odd: CurrentBuildNumber.
  EXPECT_EQ(FormatOsBuild(std::nullopt, L"19045", 4170), L"19045.4170");
  EXPECT_EQ(FormatOsBuild(L"26300 (beta)", L"26300", 1), L"26300.1");
  // Nothing usable.
  EXPECT_EQ(FormatOsBuild(std::nullopt, std::nullopt, 1), L"");
  EXPECT_EQ(FormatOsBuild(L"", L"x", 1), L"");
  EXPECT_EQ(FormatOsBuild(L"12345678901", std::nullopt, 1), L"");
}

TEST(PdfCraftDetection, Names) {
  EXPECT_TRUE(IsPdfCraftName(L"PdfCraft"));
  EXPECT_TRUE(IsPdfCraftName(L"PDFCRAFT"));
  EXPECT_FALSE(IsPdfCraftName(L"PdfCraft 0.14.0"));
  EXPECT_FALSE(IsPdfCraftName(L"PdfCraf"));
  EXPECT_FALSE(IsPdfCraftName(L""));
}

TEST(PdfCraftDetection, VersionText) {
  EXPECT_EQ(CleanVersionText(L"0.14.0"), L"0.14.0");
  EXPECT_EQ(CleanVersionText(L"0.14\r\n.0\x7F"), L"0.14.0");
  EXPECT_EQ(CleanVersionText(std::wstring(100, L'9')).size(), kMaxPdfCraftVersionChars);
}

TEST(PdfCraftDetection, NewerWins) {
  PdfCraftInfo a;
  a.installed = true;
  a.version = L"0.14.0";
  a.productCode = L"{A}";
  PdfCraftInfo b = a;
  b.version = L"0.15.0";
  b.productCode = L"{B}";
  EXPECT_EQ(NewerPdfCraft(a, b).productCode, L"{B}");
  EXPECT_EQ(NewerPdfCraft(b, a).productCode, L"{B}");
  b.version = L"0.14";  // equal: the first stays
  EXPECT_EQ(NewerPdfCraft(a, b).productCode, L"{A}");
  b.version = L"";
  EXPECT_EQ(NewerPdfCraft(a, b).productCode, L"{A}");
  EXPECT_EQ(NewerPdfCraft(b, a).productCode, L"{A}");
}

}  // namespace
}  // namespace pdft
