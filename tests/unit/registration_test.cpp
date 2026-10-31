// The pure parts of the registration (registration.h): key paths, slot decisions, CLSID checks.
// The registry itself is exercised by the integration tests, in a private key.

#include <windows.h>

#include "registration.h"

#include <gtest/gtest.h>

#include <cwctype>
#include <iterator>
#include <optional>
#include <string>

#include "project_config.h"

namespace pdft {
namespace {

const std::wstring kOurs = kHandlerClsidString;
const std::wstring kOther = L"{9C60DE1E-E5FC-40F4-A487-460851A8D915}";

TEST(Registration, KeyPaths) {
  EXPECT_EQ(ClassKeyPath(), std::wstring(L"Software\\Classes\\CLSID\\") + kHandlerClsidString);
  EXPECT_EQ(SlotKeyPath(0),
            std::wstring(L"Software\\Classes\\.pdf\\ShellEx\\") + kThumbSlotIidString);
  EXPECT_EQ(SlotKeyPath(1), std::wstring(L"Software\\Classes\\SystemFileAssociations\\.pdf\\"
                                         L"ShellEx\\") +
                                kThumbSlotIidString);
  EXPECT_EQ(SlotKeyPath(2), L"");
  EXPECT_EQ(SlotName(0), L".pdf");
  EXPECT_EQ(SlotName(1), L"SystemFileAssociations\\.pdf");
  EXPECT_EQ(SlotName(2), L"");
}

TEST(Registration, ParentKeysArePrunedInTheOrderOf1x) {
  // Deepest first, so a parent is looked at only after its child had its chance to go.
  const std::wstring expected[] = {
      L"Software\\Classes\\.pdf\\ShellEx",
      L"Software\\Classes\\.pdf",
      L"Software\\Classes\\SystemFileAssociations\\.pdf\\ShellEx",
      L"Software\\Classes\\SystemFileAssociations\\.pdf",
      L"Software\\Classes\\SystemFileAssociations",
  };
  ASSERT_EQ(std::size(kSlotParentKeys), std::size(expected));
  for (size_t i = 0; i < std::size(expected); ++i) EXPECT_EQ(kSlotParentKeys[i], expected[i]);
  // Every slot key lies below the first or the third.
  EXPECT_EQ(SlotKeyPath(0).rfind(kSlotParentKeys[0], 0), 0u);
  EXPECT_EQ(SlotKeyPath(1).rfind(kSlotParentKeys[2], 0), 0u);
}

TEST(Registration, OurClsidIgnoresCase) {
  EXPECT_TRUE(IsOurClsid(kOurs));
  std::wstring lower = kOurs;
  for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
  EXPECT_TRUE(IsOurClsid(lower));
  EXPECT_FALSE(IsOurClsid(kOther));
  EXPECT_FALSE(IsOurClsid(L""));
  EXPECT_FALSE(IsOurClsid(kOurs.substr(1, 36)));  // without braces
}

TEST(Registration, SlotWriteDecision) {
  struct Row {
    std::optional<std::wstring> current;
    bool fill;      // SlotWrite::kFillEmpty writes
    bool takeBack;  // SlotWrite::kTakeBack writes
  };
  const Row rows[] = {
      {std::nullopt, true, true},                                 // missing
      {L"", true, true},                                          // empty
      {kOurs, false, false},                                      // already ours
      {L"{0c3534c4-5b7a-48ab-828d-1cd660dc447f}", false, false},  // ours, other case
      {kOther, false, true},  // another handler: the owner's choice, unless taken back
      {L"not a clsid", false, true},
  };
  for (const Row& row : rows) {
    EXPECT_EQ(SlotNeedsWrite(row.current, SlotWrite::kFillEmpty), row.fill)
        << ::testing::PrintToString(row.current.value_or(L"<missing>"));
    EXPECT_EQ(SlotNeedsWrite(row.current, SlotWrite::kTakeBack), row.takeBack)
        << ::testing::PrintToString(row.current.value_or(L"<missing>"));
  }
  EXPECT_TRUE(SlotIsEmpty(std::nullopt));
  EXPECT_TRUE(SlotIsEmpty(std::wstring()));
  EXPECT_FALSE(SlotIsEmpty(kOther));
}

TEST(Registration, GuidStrings) {
  EXPECT_TRUE(IsGuidString(kOurs));
  EXPECT_TRUE(IsGuidString(kOther));
  EXPECT_TRUE(IsGuidString(L"{e357fccd-a995-4576-b01f-234630154e96}"));
  for (const wchar_t* bad :
       {L"", L"{}", L"0C3534C4-5B7A-48AB-828D-1CD660DC447F",
        L"{0C3534C4-5B7A-48AB-828D-1CD660DC447F", L"{0C3534C4-5B7A-48AB-828D-1CD660DC447G}",
        L"{0C3534C4x5B7A-48AB-828D-1CD660DC447F}", L"{0C3534C4-5B7A-48AB-828D-1CD660DC447F}\\..\\x",
        L"{..\\..\\..\\Software\\Microsoft\\Windows\\Run}"}) {
    EXPECT_FALSE(IsGuidString(bad)) << ::testing::PrintToString(std::wstring(bad));
  }
}

}  // namespace
}  // namespace pdft
