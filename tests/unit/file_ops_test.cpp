// The pure parts of file_ops.h: the RunOnce clean-up's value names and command lines, and how a
// security descriptor's rights for the broad groups (Everyone, Authenticated Users, Users,
// INTERACTIVE) are worked out, on descriptors made from SDDL.

#include <windows.h>

#include "file_ops.h"

#include <gtest/gtest.h>
#include <sddl.h>
#include <wil/resource.h>

#include <optional>
#include <string>

#include "project_config.h"

namespace pdft {
namespace {

const std::wstring kProduct = kProductName;
const std::filesystem::path kCmd = L"C:\\Windows\\System32\\cmd.exe";

TEST(RunOnceNames, OnePerFolder) {
  EXPECT_EQ(RunOnceValueName(0), kProduct + L"Cleanup");
  EXPECT_EQ(RunOnceValueName(1), kProduct + L"Cleanup2");
  EXPECT_EQ(RunOnceValueName(2), kProduct + L"Cleanup3");
  EXPECT_EQ(RunOnceValueName(10), kProduct + L"Cleanup11");
}

TEST(RunOnceNames, TheFamily) {
  EXPECT_TRUE(IsRunOnceRemovalName(kProduct + L"Cleanup"));
  EXPECT_TRUE(IsRunOnceRemovalName(kProduct + L"Cleanup2"));
  EXPECT_TRUE(IsRunOnceRemovalName(kProduct + L"cleanup17"));  // case does not matter
  EXPECT_TRUE(IsRunOnceRemovalName(kLegacyRunOnce));
  EXPECT_FALSE(IsRunOnceRemovalName(kProduct + L"CleanupX"));
  EXPECT_FALSE(IsRunOnceRemovalName(kProduct + L"Cleanup-2"));
  EXPECT_FALSE(IsRunOnceRemovalName(kProduct + L"Clean"));
  EXPECT_FALSE(IsRunOnceRemovalName(kProduct));
  EXPECT_FALSE(IsRunOnceRemovalName(L"OtherAppCleanup"));
  EXPECT_FALSE(IsRunOnceRemovalName(L""));
}

TEST(RunOnceCommand, TheLine) {
  std::wstring command;
  ASSERT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\Home\\pat\\Programs\\" + kProduct, &command), S_OK);
  EXPECT_EQ(command,
            L"\"C:\\Windows\\System32\\cmd.exe\" /d /c rd /s /q \"D:\\Home\\pat\\Programs\\" +
                kProduct + L"\"");
  // A trailing backslash would sit right before the closing quote.
  ASSERT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\Home\\pat\\x\\", &command), S_OK);
  EXPECT_EQ(command, L"\"C:\\Windows\\System32\\cmd.exe\" /d /c rd /s /q \"D:\\Home\\pat\\x\"");
  // Spaces and the characters cmd.exe treats specially outside quotes are fine inside them.
  ASSERT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\a b\\c&d (e)^f", &command), S_OK);
  EXPECT_EQ(command, L"\"C:\\Windows\\System32\\cmd.exe\" /d /c rd /s /q \"D:\\a b\\c&d (e)^f\"");
}

TEST(RunOnceCommand, RefusesWhatItCannotCarry) {
  std::wstring command = L"stale";
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\", &command), E_INVALIDARG);
  EXPECT_TRUE(command.empty());
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"relative\\dir", &command), E_INVALIDARG);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"\\\\server\\share\\dir", &command), E_INVALIDARG);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"\\\\?\\D:\\dir", &command), E_INVALIDARG);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\a\\..\\b", &command), E_INVALIDARG);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\a\\.\\b", &command), E_INVALIDARG);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:/a/b", &command), E_INVALIDARG);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\a\\b:stream", &command), E_INVALIDARG);
  const HRESULT badName = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\a\"b", &command), badName);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\%TEMP%", &command), badName);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\a!b!", &command), badName);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, L"D:\\a\tb", &command), badName);
  EXPECT_EQ(RunOnceRemovalCommand(L"cmd.exe", L"D:\\a", &command), E_INVALIDARG);
}

TEST(RunOnceCommand, ShorterThan260Characters) {
  const std::wstring prefix = L"\"" + kCmd.native() + L"\" /d /c rd /s /q \"";
  const auto dirOfLineLength = [&](size_t length) {
    const size_t dirLength = length - prefix.size() - 1;  // the closing quote
    return L"D:\\" + std::wstring(dirLength - 3, L'a');
  };
  std::wstring command;
  ASSERT_EQ(RunOnceRemovalCommand(kCmd, dirOfLineLength(kRunOnceMaxChars - 1), &command), S_OK);
  EXPECT_EQ(command.size(), kRunOnceMaxChars - 1);
  EXPECT_EQ(RunOnceRemovalCommand(kCmd, dirOfLineLength(kRunOnceMaxChars), &command),
            HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE));
  EXPECT_TRUE(command.empty());
}

// ---- the broad groups ----

std::optional<BroadAccess> Broad(const wchar_t* sddl, ACCESS_MASK mask) {
  wil::unique_hlocal_security_descriptor descriptor;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, descriptor.put(),
                                                            nullptr)) {
    ADD_FAILURE() << "bad SDDL " << GetLastError();
    return BroadAccess{L"(bad SDDL)", 0};
  }
  return FindBroadAccess(descriptor.get(), mask);
}

constexpr ACCESS_MASK kObject = kBuildObjectChangeRights;
constexpr ACCESS_MASK kParent = kBuildParentChangeRights;

TEST(BroadAccess, AdministratorsAndSystemOnly) {
  EXPECT_FALSE(
      Broad(L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)(A;;0x1200a9;;;WD)", kObject));
  // Program Files and the profile, as Windows sets them up.
  EXPECT_FALSE(
      Broad(L"O:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;OICIIO;GA;;;CO)(A;;0x1200a9;;;BU)", kObject));
}

TEST(BroadAccess, EachGroupAlone) {
  std::optional<BroadAccess> found = Broad(L"O:BAD:(A;;FA;;;AU)", kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->principal, L"Authenticated Users");
  EXPECT_EQ(found->rights, kObject);

  found = Broad(L"O:BAD:(A;;0x2;;;BU)", kObject);  // FILE_WRITE_DATA: add a file
  ASSERT_TRUE(found);
  EXPECT_EQ(found->principal, L"Users");
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(FILE_WRITE_DATA));

  found = Broad(L"O:BAD:(A;;WD;;;IU)", kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->principal, L"INTERACTIVE");
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(WRITE_DAC));

  found = Broad(L"O:BAD:(A;;SD;;;WD)", kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->principal, L"Everyone");
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(DELETE));
}

TEST(BroadAccess, InheritOnlyEntriesDoNotCountForTheObject) {
  EXPECT_FALSE(Broad(L"O:BAD:(A;OICIIO;FA;;;AU)", kObject));
}

TEST(BroadAccess, GenericRightsAreMapped) {
  std::optional<BroadAccess> found = Broad(L"O:BAD:(A;;GW;;;WD)", kObject);
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->rights & FILE_WRITE_DATA);
  EXPECT_TRUE(found->rights & FILE_APPEND_DATA);
  EXPECT_FALSE(Broad(L"O:BAD:(A;;GRGX;;;WD)", kObject));
}

TEST(BroadAccess, EntriesInOrder) {
  // Everyone is in every group's token: denied first, nobody gets it.
  EXPECT_FALSE(Broad(L"O:BAD:(D;;FA;;;WD)(A;;FA;;;AU)", kObject));
  // Allowed first: a later deny changes nothing.
  EXPECT_TRUE(Broad(L"O:BAD:(A;;FA;;;AU)(D;;FA;;;AU)", kObject));
}

TEST(BroadAccess, ADenyForOneGroupDoesNotHideAnother) {
  // INTERACTIVE is denied, but a batch or service logon of any account is not interactive.
  const std::optional<BroadAccess> found = Broad(L"O:BAD:(D;;FA;;;IU)(A;;FA;;;AU)", kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->principal, L"Authenticated Users");
}

TEST(BroadAccess, AnOwnerAmongTheGroupsMayChangePermissions) {
  const std::optional<BroadAccess> found = Broad(L"O:AUD:(A;;FA;;;SY)", kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->principal, L"Authenticated Users");
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(WRITE_DAC));
}

TEST(BroadAccess, ANullDaclGrantsEverything) {
  const std::optional<BroadAccess> found = Broad(L"O:BAD:NO_ACCESS_CONTROL", kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->rights, kObject);
}

TEST(BroadAccess, FoldersAbove) {
  // C:\ lets Authenticated Users add folders (FILE_APPEND_DATA): harmless above a build folder,
  // not in it.
  EXPECT_FALSE(Broad(L"O:SYD:(A;;FA;;;SY)(A;;0x4;;;AU)(A;OICIIO;SDGRGWGX;;;AU)", kParent));
  EXPECT_TRUE(Broad(L"O:SYD:(A;;FA;;;SY)(A;;0x4;;;AU)", kObject));
  // Deleting children (FILE_DELETE_CHILD) is enough to swap the folder below.
  const std::optional<BroadAccess> found = Broad(L"O:SYD:(A;;0x40;;;BU)", kParent);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->principal, L"Users");
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(FILE_DELETE_CHILD));
}

TEST(BroadAccess, ConditionalEntries) {
  // An allow with a condition counts as if it held; a deny with one as if it did not.
  EXPECT_TRUE(Broad(L"O:BAD:(XA;;FA;;;AU;(Member_of {SID(BA)}))", kObject));
  EXPECT_TRUE(Broad(L"O:BAD:(XD;;FA;;;AU;(Member_of {SID(BA)}))(A;;FA;;;AU)", kObject));
}

}  // namespace
}  // namespace pdft
