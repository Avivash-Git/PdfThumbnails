// project_config.h, generated from cmake/ProjectInfo.cmake: every GUID struct is the GUID its
// string form names, the Windows constants spelled out there are the SDK's, and the private
// preview IID is one of a kind.

#include <windows.h>

#include "project_config.h"

#include <gtest/gtest.h>
#include <knownfolders.h>
#include <objbase.h>
#include <propsys.h>
#include <shlobj.h>
#include <thumbcache.h>

#include <cwchar>
#include <string>

namespace pdft::test {
namespace {

std::string Text(const GUID& guid) {
  wchar_t text[39] = {};
  StringFromGUID2(guid, text, 39);
  std::string out;
  for (const wchar_t* c = text; *c; ++c) out += static_cast<char>(*c);
  return out;
}

struct NamedGuid {
  const char* name;
  const wchar_t* text;
  GUID guid;
};

const NamedGuid kGuids[] = {
    {"kHandlerClsid", kHandlerClsidString, kHandlerClsid},
    {"kPreviewIid", kPreviewIidString, kPreviewIid},
    {"kPdfCraftUpgradeCode", kPdfCraftUpgradeCodeString, kPdfCraftUpgradeCode},
    {"kThumbSlotIid", kThumbSlotIidString, kThumbSlotIid},
    {"kSurrogateAppId", kSurrogateAppIdString, kSurrogateAppId},
};

TEST(ProjectConfig, EveryGuidMatchesItsString) {
  for (const NamedGuid& g : kGuids) {
    SCOPED_TRACE(g.name);
    // The registry form: 38 characters in braces.
    ASSERT_EQ(std::wcslen(g.text), 38u);
    EXPECT_EQ(g.text[0], L'{');
    EXPECT_EQ(g.text[37], L'}');
    // IIDFromString parses any GUID string and, unlike CLSIDFromString, never looks up a ProgID.
    GUID parsed = {};
    ASSERT_EQ(IIDFromString(g.text, &parsed), S_OK);
    EXPECT_TRUE(parsed == g.guid) << Text(parsed) << " vs " << Text(g.guid);
  }
}

// These are spelled out so the shipped code does not depend on uuid.lib knowing them; the test
// may link it and compare.
TEST(ProjectConfig, WindowsConstantsAreTheSdks) {
  EXPECT_TRUE(kIidInitializeWithStream == IID_IInitializeWithStream);
  EXPECT_TRUE(kThumbSlotIid == IID_IThumbnailProvider);
  EXPECT_TRUE(kFolderIdLocalAppData == FOLDERID_LocalAppData);
}

TEST(ProjectConfig, ThePreviewIidIsNoOtherGuid) {
  EXPECT_FALSE(kPreviewIid == GUID_NULL);
  EXPECT_FALSE(kPreviewIid == IID_IUnknown);
  EXPECT_FALSE(kPreviewIid == kIidInitializeWithStream);
  EXPECT_FALSE(kPreviewIid == kFolderIdLocalAppData);
  for (const NamedGuid& g : kGuids) {
    if (std::string(g.name) != "kPreviewIid") {
      EXPECT_FALSE(kPreviewIid == g.guid) << g.name;
    }
  }
}

}  // namespace
}  // namespace pdft::test
