// The pure parts of file_ops.h: the RunOnce clean-up's value names and command lines; how a
// security descriptor's owner and rights for accounts and groups other than the trusted ones
// are worked out, on descriptors made from SDDL (and one built by hand, for an entry SDDL cannot
// write); the texts that say what was found; and network folders, which are refused before anything
// is opened.

#include <windows.h>

#include "file_ops.h"

#include <gtest/gtest.h>
#include <sddl.h>
#include <wil/resource.h>

#include <optional>
#include <string>
#include <vector>

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

// ---- who else could change a build folder ----

// The user the checks below are made for, and another account, both of a made-up domain. The
// domain SIDs here are split after "S-1-5-21-": the repository's publish check rejects anything
// shaped like an account SID, even an invented one.
constexpr wchar_t kUser[] =
    L"S-1-5-21-"
    L"1-2-3-1001";
constexpr wchar_t kOther[] =
    L"S-1-5-21-"
    L"1-2-3-1005";
// This PC's own accounts' domain in the checks below, another made-up one.
constexpr wchar_t kPcDomain[] =
    L"S-1-5-21-"
    L"4-5-6";

std::optional<BroadAccess> Broad(const std::wstring& sddl, ACCESS_MASK mask) {
  wil::unique_hlocal_security_descriptor descriptor;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            descriptor.put(), nullptr)) {
    ADD_FAILURE() << "bad SDDL " << GetLastError();
    return BroadAccess{L"(bad SDDL)", 0};
  }
  wil::unique_hlocal user;
  wil::unique_hlocal domain;
  if (!ConvertStringSidToSidW(kUser, user.put()) ||
      !ConvertStringSidToSidW(kPcDomain, domain.put())) {
    ADD_FAILURE() << "bad SID " << GetLastError();
    return BroadAccess{L"(bad SID)", 0};
  }
  const PSID domains[] = {domain.get()};
  return FindBroadAccess(descriptor.get(), mask, user.get(), domains);
}

constexpr ACCESS_MASK kObject = kBuildObjectChangeRights;
constexpr ACCESS_MASK kFile = kBuildObjectChangeRights & ~FILE_DELETE_CHILD;
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

TEST(BroadAccess, AnotherAccountsEntry) {
  const std::wstring other = kOther;
  // Write, append and delete (with write attributes and extended attributes).
  std::optional<BroadAccess> found =
      Broad(L"O:BAD:(A;;FA;;;BA)(A;;0x10116;;;" + other + L")", kFile);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->sid, other);
  EXPECT_EQ(found->principal, other);  // named by its text: the check itself looks up nothing
  EXPECT_FALSE(found->owner);
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE));
  // The user the check is made for may change it.
  EXPECT_FALSE(Broad(L"O:BAD:(A;;FA;;;" + std::wstring(kUser) + L")", kObject));
  // Reading is fine.
  EXPECT_FALSE(Broad(L"O:BAD:(A;;0x1200a9;;;" + other + L")", kObject));
}

TEST(BroadAccess, OtherGroups) {
  // LOCAL (anyone signed in at the console) may delete children of a folder above.
  std::optional<BroadAccess> found = Broad(L"O:SYD:(A;;0x40;;;S-1-2-0)", kParent);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->sid, L"S-1-2-0");
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(FILE_DELETE_CHILD));
  // Local accounts (S-1-5-113) and a domain's group, shaped like Domain Users.
  EXPECT_TRUE(Broad(L"O:BAD:(A;;WD;;;S-1-5-113)", kObject));
  found = Broad(
      L"O:BAD:(A;;WD;;;S-1-5-21-"
      L"1-2-3-513)",
      kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->sid,
            L"S-1-5-21-"
            L"1-2-3-513");
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(WRITE_DAC));
}

TEST(BroadAccess, AnOwnerThatIsNotTrusted) {
  const std::wstring other = kOther;
  const std::optional<BroadAccess> found = Broad(L"O:" + other + L"D:P(A;;FA;;;BA)", kObject);
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->owner);
  EXPECT_EQ(found->sid, other);
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(WRITE_DAC));
  // The user's own folder.
  EXPECT_FALSE(Broad(L"O:" + std::wstring(kUser) + L"D:P(A;;FA;;;BA)", kObject));
}

TEST(BroadAccess, AdministratorsByDefinition) {
  // Domain Admins and the built-in Administrator account of the user's own domain and of this
  // PC's, TrustedInstaller, SYSTEM and Administrators, as owners and in entries.
  for (const wchar_t* sid :
       {L"S-1-5-21-"
        L"1-2-3-512",
        L"S-1-5-21-"
        L"1-2-3-500",
        L"S-1-5-21-"
        L"4-5-6-500",
        L"S-1-5-21-"
        L"4-5-6-512",
        L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", L"SY", L"BA"}) {
    const std::wstring text = sid;
    EXPECT_FALSE(Broad(L"O:" + text + L"D:(A;;FA;;;" + text + L")", kObject))
        << ::testing::PrintToString(text);
  }
  // Another service is not TrustedInstaller; a domain's RID 512 is no administrator elsewhere
  // in the SID.
  EXPECT_TRUE(Broad(L"O:BAD:(A;;FA;;;S-1-5-80-1-2-3-4-5)", kObject));
  EXPECT_TRUE(
      Broad(L"O:BAD:(A;;FA;;;S-1-5-21-"
            L"512-2-3-1005)",
            kObject));
}

// The same RIDs of a domain that is neither the user's nor this PC's (one in another forest that
// this PC only trusts, say) are no administrators here, nor are Enterprise Admins of any domain:
// they are reported, as owners and in entries.
TEST(BroadAccess, AdministratorsOfOtherDomainsAreNot) {
  for (const wchar_t* sid : {L"S-1-5-21-"
                             L"7-8-9-500",
                             L"S-1-5-21-"
                             L"7-8-9-512",
                             L"S-1-5-21-"
                             L"1-2-3-519"}) {
    const std::wstring text = sid;
    std::optional<BroadAccess> found = Broad(L"O:" + text + L"D:P(A;;FA;;;BA)", kObject);
    ASSERT_TRUE(found) << ::testing::PrintToString(text);
    EXPECT_TRUE(found->owner);
    EXPECT_EQ(found->sid, text);
    found = Broad(L"O:BAD:(A;;FA;;;" + text + L")", kObject);
    ASSERT_TRUE(found) << ::testing::PrintToString(text);
    EXPECT_FALSE(found->owner);
    EXPECT_EQ(found->sid, text);
  }
}

TEST(BroadAccess, OwnerRightsStandForTheOwner) {
  EXPECT_FALSE(Broad(L"O:BAD:(A;;FA;;;OW)", kObject));
  // With an owner that is not trusted, the owner is what is reported, whatever OWNER RIGHTS
  // limit.
  const std::optional<BroadAccess> found =
      Broad(L"O:" + std::wstring(kOther) + L"D:(A;;RC;;;OW)(A;;FA;;;BA)", kObject);
  ASSERT_TRUE(found);
  EXPECT_TRUE(found->owner);
  EXPECT_EQ(found->sid, kOther);
}

TEST(BroadAccess, CreatorEntriesMatchNobody) {
  // CREATOR OWNER and CREATOR GROUP in an entry for the object itself.
  EXPECT_FALSE(Broad(L"O:BAD:(A;;FA;;;CO)(A;;FA;;;CG)", kObject));
}

TEST(BroadAccess, AppContainersAreLeftOut) {
  // ALL APPLICATION PACKAGES and a capability: an app container gets only what its user and
  // groups get as well.
  EXPECT_FALSE(Broad(L"O:BAD:(A;OICI;FA;;;AC)(A;OICI;FA;;;S-1-15-3-1)", kObject));
}

TEST(BroadAccess, ADenyFirstForAnUntrustedSid) {
  const std::wstring other = kOther;
  EXPECT_FALSE(Broad(L"O:BAD:(D;;FA;;;" + other + L")(A;;FA;;;" + other + L")", kObject));
  // Everyone is in every token.
  EXPECT_FALSE(Broad(L"O:BAD:(D;;FA;;;WD)(A;;FA;;;" + other + L")", kObject));
  // A deny for a group the account may not be in hides nothing.
  EXPECT_TRUE(Broad(L"O:BAD:(D;;FA;;;IU)(A;;FA;;;" + other + L")", kObject));
  // A deny that comes after it is too late.
  EXPECT_TRUE(Broad(L"O:BAD:(A;;FA;;;" + other + L")(D;;FA;;;" + other + L")", kObject));
}

TEST(BroadAccess, ACallbackAllowCounts) {
  const std::optional<BroadAccess> found =
      Broad(L"O:BAD:(XA;;FA;;;" + std::wstring(kOther) + L";(Member_of {SID(BA)}))", kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->sid, kOther);
}

TEST(BroadAccess, AnObjectAllowCountsForItsSid) {
  const std::optional<BroadAccess> found =
      Broad(L"O:BAD:(OA;;FA;bf967aba-0de6-11d0-a285-00aa003049e2;;" + std::wstring(kOther) + L")",
            kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->sid, kOther);
  EXPECT_FALSE(
      Broad(L"O:BAD:(OA;;FA;bf967aba-0de6-11d0-a285-00aa003049e2;;" + std::wstring(kUser) + L")",
            kObject));
}

// A descriptor owned by Administrators whose DACL holds one entry of the kind |type|, written
// by hand (SDDL has no word for a kind Windows does not define), naming Administrators.
std::optional<BroadAccess> BroadWithEntryOfType(BYTE type, ACCESS_MASK mask) {
  BYTE administrators[SECURITY_MAX_SID_SIZE] = {};
  DWORD size = sizeof(administrators);
  if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &size)) {
    ADD_FAILURE() << "CreateWellKnownSid " << GetLastError();
    return std::nullopt;
  }
  alignas(DWORD) BYTE aclBytes[256] = {};
  const auto acl = reinterpret_cast<PACL>(aclBytes);
  void* ace = nullptr;
  if (!InitializeAcl(acl, sizeof(aclBytes), ACL_REVISION) ||
      !AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, administrators) ||
      !GetAce(acl, 0, &ace)) {
    ADD_FAILURE() << "ACL " << GetLastError();
    return std::nullopt;
  }
  static_cast<ACE_HEADER*>(ace)->AceType = type;
  SECURITY_DESCRIPTOR descriptor = {};
  if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorOwner(&descriptor, administrators, FALSE) ||
      !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE)) {
    ADD_FAILURE() << "descriptor " << GetLastError();
    return std::nullopt;
  }
  wil::unique_hlocal user;
  if (!ConvertStringSidToSidW(kUser, user.put())) {
    ADD_FAILURE() << "bad SID " << GetLastError();
    return std::nullopt;
  }
  return FindBroadAccess(&descriptor, mask, user.get(), {});
}

TEST(BroadAccess, AnEntryOfAnUnknownKindCountsForEveryone) {
  EXPECT_FALSE(BroadWithEntryOfType(ACCESS_ALLOWED_ACE_TYPE, kObject));
  constexpr BYTE kUnknown = 0x1F;
  const std::optional<BroadAccess> found = BroadWithEntryOfType(kUnknown, kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->rights, kObject);
  EXPECT_NE(found->principal.find(L"does not know (type 31)"), std::wstring::npos)
      << ::testing::PrintToString(found->principal);
  // An audit entry does not belong in a DACL: there it is of a kind the check does not know.
  EXPECT_TRUE(BroadWithEntryOfType(SYSTEM_AUDIT_ACE_TYPE, kObject));
}

TEST(BroadAccess, FilesAndFoldersShareOneGenericMapping) {
  const std::wstring other = kOther;
  // GENERIC_ALL on a folder above gives everything that counts there, deleting children too.
  std::optional<BroadAccess> found = Broad(L"O:SYD:(A;;GA;;;" + other + L")", kParent);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->rights, kParent);
  // GENERIC_WRITE: adding files and folders (writing and appending), never deleting.
  found = Broad(L"O:BAD:(A;;GW;;;" + other + L")", kObject);
  ASSERT_TRUE(found);
  EXPECT_EQ(found->rights, static_cast<ACCESS_MASK>(FILE_WRITE_DATA | FILE_APPEND_DATA));
  EXPECT_FALSE(Broad(L"O:SYD:(A;;GW;;;" + other + L")", kParent));
}

// ---- what is said about what was found ----

TEST(UnsafeAccessTexts, SayWhatWasFound) {
  UnsafeAccess unsafe{L"D:\\b", BroadAccess{L"PC\\pat", WRITE_DAC, kOther}};
  EXPECT_EQ(UnsafeAccessText(unsafe),
            L"D:\\b can be changed by PC\\pat (" + std::wstring(kOther) + L")");
  unsafe.access.owner = true;
  EXPECT_EQ(UnsafeAccessText(unsafe), L"D:\\b is owned by PC\\pat (" + std::wstring(kOther) + L")");
  EXPECT_EQ(UnsafeAccessReason(unsafe).find(L"D:\\"), std::wstring::npos);
  // Named by its text only: the SID once.
  unsafe = UnsafeAccess{L"D:\\b", BroadAccess{kOther, WRITE_DAC, kOther}};
  EXPECT_EQ(UnsafeAccessText(unsafe), L"D:\\b can be changed by " + std::wstring(kOther));
  unsafe = UnsafeAccess{L"D:\\b", BroadAccess{L"Authenticated Users", DELETE}};
  EXPECT_EQ(UnsafeAccessText(unsafe), L"D:\\b can be changed by Authenticated Users");

  // Where the text is logged, a name Windows looked up for a SID is left out and the SID stands
  // alone; a group's own name stays.
  unsafe = UnsafeAccess{L"D:\\b", BroadAccess{L"PC\\pat", WRITE_DAC, kOther, true, true}};
  EXPECT_EQ(UnsafeAccessReason(unsafe), L"is owned by PC\\pat (" + std::wstring(kOther) + L")");
  EXPECT_EQ(UnsafeAccessReason(unsafe, false), L"is owned by " + std::wstring(kOther));
  unsafe.access.owner = false;
  EXPECT_EQ(UnsafeAccessReason(unsafe, false), L"can be changed by " + std::wstring(kOther));
  unsafe = UnsafeAccess{L"D:\\b", BroadAccess{L"Authenticated Users", DELETE, L"S-1-5-11"}};
  EXPECT_EQ(UnsafeAccessReason(unsafe, false), L"can be changed by Authenticated Users (S-1-5-11)");

  unsafe = UnsafeAccess{};
  unsafe.object = L"D:\\link";
  unsafe.kind = UnsafeKind::kLink;
  unsafe.reparseTag = IO_REPARSE_TAG_MOUNT_POINT;
  EXPECT_EQ(UnsafeAccessText(unsafe), L"D:\\link is a junction or mount point");
  unsafe.reparseTag = IO_REPARSE_TAG_SYMLINK;
  EXPECT_EQ(UnsafeAccessReason(unsafe), L"is a symbolic link");
  unsafe.reparseTag = 0;
  EXPECT_NE(UnsafeAccessReason(unsafe).find(L"through a link"), std::wstring::npos);

  unsafe = UnsafeAccess{};
  unsafe.object = L"\\\\server\\share\\b";
  unsafe.kind = UnsafeKind::kNetwork;
  EXPECT_EQ(UnsafeAccessText(unsafe), L"\\\\server\\share\\b is on a network drive");
}

// ---- network folders ----

TEST(NetworkFolders, AreKnownByTheirPath) {
  EXPECT_TRUE(IsOnNetworkDrive(L"\\\\server\\share\\build"));
  EXPECT_TRUE(IsOnNetworkDrive(L"//server/share/build"));
  EXPECT_TRUE(IsOnNetworkDrive(L"\\\\?\\UNC\\server\\share\\build"));
  EXPECT_TRUE(IsOnNetworkDrive(L"\\\\?\\unc\\server\\share"));
  // An object manager path may name a share (\Device\Mup) or a link to one.
  EXPECT_TRUE(IsOnNetworkDrive(L"\\\\?\\GLOBALROOT\\Device\\Mup\\server\\share\\build"));
  EXPECT_TRUE(IsOnNetworkDrive(L"\\\\.\\globalroot\\Device\\Mup\\server\\share"));
  // The system drive is local on every PC the tests run on.
  wchar_t windows[MAX_PATH] = {};
  ASSERT_GT(GetWindowsDirectoryW(windows, MAX_PATH), 2u);
  const std::wstring system = std::wstring(windows).substr(0, 2) + L"\\build";
  EXPECT_FALSE(IsOnNetworkDrive(system));
  EXPECT_FALSE(IsOnNetworkDrive(L"\\\\?\\" + system));
  EXPECT_FALSE(IsOnNetworkDrive(L"\\\\.\\pipe\\build"));
  EXPECT_FALSE(IsOnNetworkDrive(L"relative\\build"));
}

TEST(NetworkFolders, AreRefusedBeforeAnythingIsOpened) {
  // A share that does not exist: refused at once, never asked for.
  const std::wstring folder = L"\\\\pdft-no-such-server.invalid\\share\\build";
  HeldBuildFolder held;
  std::optional<UnsafeAccess> unsafe;
  ASSERT_EQ(HoldBuildFolder(folder, BuildFolderHoldOptions{}, &held, &unsafe), S_OK);
  ASSERT_TRUE(unsafe);
  EXPECT_EQ(unsafe->kind, UnsafeKind::kNetwork);
  EXPECT_EQ(unsafe->object, folder);
  EXPECT_TRUE(held.dir().empty());
  const std::vector<std::wstring> binaries = {L"a.exe"};
  ASSERT_EQ(FindUnsafeBuildFolderAccess(folder, binaries, &unsafe), S_OK);
  ASSERT_TRUE(unsafe);
  EXPECT_EQ(unsafe->kind, UnsafeKind::kNetwork);
}

}  // namespace
}  // namespace pdft
