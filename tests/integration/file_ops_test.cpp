// File operations (file_ops.h) on folders this test makes under %TEMP%\pdft-tests: reparse point
// checks, the verified copy, removal that never follows links, files in use (held open by this
// test, or mapped as an image like a loaded DLL), the trash folder, the build-folder permission
// check on folders made with known permissions, and the RunOnce clean-up values in private
// registry keys (ScopedRegistrySandbox). Never queues a deletion at restart: unelevated that call
// is only checked to be refused, and the elevated path runs in the system tests.

#include <windows.h>

#include "file_ops.h"

#include <aclapi.h>
#include <gtest/gtest.h>
#include <sddl.h>
#include <wil/resource.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "build_folder.h"
#include "child_process.h"
#include "elevation.h"
#include "instance.h"
#include "legacy.h"
#include "paths.h"
#include "project_config.h"
#include "registry_sandbox.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

using std::filesystem::path;

bool WriteFileText(const path& file, const std::string& text) {
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL, nullptr));
  DWORD written = 0;
  return handle &&
         WriteFile(handle.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
         written == text.size();
}

std::string ReadFileText(const path& file) {
  wil::unique_hfile handle(CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  std::string text;
  char buffer[4096];
  DWORD read = 0;
  while (handle && ReadFile(handle.get(), buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    text.append(buffer, read);
  }
  return text;
}

bool Exists(const path& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool MakeDir(const path& p) { return CreateDirectoryW(p.c_str(), nullptr) != FALSE; }

// The names in |dir|.
std::vector<std::wstring> Listing(const path& dir) {
  std::vector<std::wstring> names;
  WIN32_FIND_DATAW entry = {};
  wil::unique_hfind find(FindFirstFileW((dir / L"*").c_str(), &entry));
  if (!find) return names;
  do {
    const std::wstring name = entry.cFileName;
    if (name != L"." && name != L"..") names.push_back(name);
  } while (FindNextFileW(find.get(), &entry));
  return names;
}

bool Elevated() {
  bool elevated = false;
  return SUCCEEDED(GetTokenElevation(&elevated)) && elevated;
}

// ---- reparse points ----

TEST(NoReparsePoints, PlainFoldersAndMissingParts) {
  test::TempDir dir(L"reparse");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(MakeDir(root / L"a"));
  EXPECT_EQ(CheckNoReparsePoints(root / L"a", root), S_OK);
  EXPECT_EQ(CheckNoReparsePoints(root / L"a" / L"b" / L"c", root), S_OK);  // not there yet
  EXPECT_EQ(CheckNoReparsePoints(root, root), S_OK);
}

TEST(NoReparsePoints, AJunctionOnTheWay) {
  test::TempDir dir(L"reparse");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(MakeDir(root / L"target"));
  ASSERT_TRUE(MakeDir(root / L"target" / L"sub"));
  ASSERT_TRUE(MakeDir(root / L"link"));
  ASSERT_TRUE(test::MakeJunction((root / L"link").native(), (root / L"target").native()));
  const HRESULT stopped = HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK);
  EXPECT_EQ(CheckNoReparsePoints(root / L"link", root), stopped);
  EXPECT_EQ(CheckNoReparsePoints(root / L"link" / L"sub", root), stopped);
  EXPECT_EQ(CheckNoReparsePoints(root / L"link" / L"missing" / L"x", root), stopped);
  EXPECT_EQ(CheckNoReparsePoints(root / L"target" / L"sub", root), S_OK);
}

TEST(NoReparsePoints, BadArguments) {
  test::TempDir dir(L"reparse");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  EXPECT_EQ(CheckNoReparsePoints(L"relative\\path", {}), E_INVALIDARG);
  EXPECT_EQ(CheckNoReparsePoints(root, L"relative"), E_INVALIDARG);
  // |stopAt| below |path|, or beside it.
  EXPECT_EQ(CheckNoReparsePoints(root, root / L"a"), E_INVALIDARG);
  EXPECT_EQ(CheckNoReparsePoints(root / L"a", root.parent_path() / L"elsewhere"), E_INVALIDARG);
}

// ---- the verified copy ----

TEST(CopyVerified, CopiesAndHashes) {
  test::TempDir dir(L"copy");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  const path from = root / L"source.bin";
  std::string content;
  for (int i = 0; i < 100000; ++i) content.push_back(static_cast<char>(i * 7));
  ASSERT_TRUE(WriteFileText(from, content));
  ASSERT_TRUE(MakeDir(root / L"to"));
  const path to = root / L"to" / L"copy.bin";

  Sha256Digest sha = {};
  ASSERT_EQ(CopyFileVerified(from, to, &sha), S_OK);
  EXPECT_EQ(ReadFileText(to), content);
  Sha256Digest expected = {};
  ASSERT_EQ(Sha256File(from, &expected), S_OK);
  EXPECT_EQ(sha, expected);
  // Only the copy is there: the temporary file was renamed, not left behind.
  EXPECT_EQ(Listing(root / L"to"), std::vector<std::wstring>{L"copy.bin"});

  // Again over the copy, with other bytes.
  ASSERT_TRUE(WriteFileText(from, "second"));
  ASSERT_EQ(CopyFileVerified(from, to, &sha), S_OK);
  EXPECT_EQ(ReadFileText(to), "second");
  EXPECT_EQ(Listing(root / L"to"), std::vector<std::wstring>{L"copy.bin"});
}

TEST(CopyVerified, RefusesASourceSomeoneIsWriting) {
  test::TempDir dir(L"copy");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  const path from = root / L"source.bin";
  ASSERT_TRUE(WriteFileText(from, "content"));
  ASSERT_TRUE(MakeDir(root / L"to"));
  wil::unique_hfile writer(CreateFileW(from.c_str(), GENERIC_WRITE,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(writer);
  Sha256Digest sha = {};
  sha.fill(0xAA);
  EXPECT_EQ(CopyFileVerified(from, root / L"to" / L"copy.bin", &sha),
            HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));
  EXPECT_EQ(sha, Sha256Digest{});
  EXPECT_TRUE(Listing(root / L"to").empty());
}

TEST(CopyVerified, RefusesFoldersAndLinkedTargets) {
  test::TempDir dir(L"copy");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(MakeDir(root / L"folder"));
  ASSERT_TRUE(MakeDir(root / L"to"));
  EXPECT_TRUE(FAILED(CopyFileVerified(root / L"folder", root / L"to" / L"copy.bin")));
  EXPECT_TRUE(Listing(root / L"to").empty());

  // The target folder is a junction: nothing is written through it.
  const path from = root / L"source.bin";
  ASSERT_TRUE(WriteFileText(from, "content"));
  ASSERT_TRUE(MakeDir(root / L"elsewhere"));
  ASSERT_TRUE(MakeDir(root / L"link"));
  ASSERT_TRUE(test::MakeJunction((root / L"link").native(), (root / L"elsewhere").native()));
  EXPECT_EQ(CopyFileVerified(from, root / L"link" / L"copy.bin"),
            HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  EXPECT_TRUE(Listing(root / L"elsewhere").empty());
  EXPECT_EQ(CopyFileVerified(from, L"relative.bin"), E_INVALIDARG);
}

TEST(CopyVerified, KeepsATargetInUse) {
  test::TempDir dir(L"copy");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  const path from = root / L"source.bin";
  const path to = root / L"target.bin";
  ASSERT_TRUE(WriteFileText(from, "new"));
  ASSERT_TRUE(WriteFileText(to, "old"));
  {
    // Open without delete sharing, like a running program's image.
    wil::unique_hfile user(CreateFileW(to.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    ASSERT_TRUE(user);
    EXPECT_TRUE(FAILED(CopyFileVerified(from, to)));
  }
  EXPECT_EQ(ReadFileText(to), "old");
  std::vector<std::wstring> names = Listing(root);
  std::sort(names.begin(), names.end());
  EXPECT_EQ(names, (std::vector<std::wstring>{L"source.bin", L"target.bin"}));
}

// ---- removal ----

TEST(RemoveTree, RemovesEverythingButFollowsNoLink) {
  test::TempDir dir(L"remove");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  const path tree = root / L"tree";
  ASSERT_TRUE(MakeDir(tree));
  ASSERT_TRUE(MakeDir(tree / L"sub"));
  ASSERT_TRUE(MakeDir(tree / L"sub" / L"deeper"));
  ASSERT_TRUE(WriteFileText(tree / L"a.txt", "a"));
  ASSERT_TRUE(WriteFileText(tree / L"sub" / L"deeper" / L"b.txt", "b"));
  ASSERT_TRUE(WriteFileText(tree / L"sub" / L"readonly.txt", "r"));
  ASSERT_TRUE(
      SetFileAttributesW((tree / L"sub" / L"readonly.txt").c_str(), FILE_ATTRIBUTE_READONLY));
  // A junction inside the tree, to a folder outside it.
  ASSERT_TRUE(MakeDir(root / L"outside"));
  ASSERT_TRUE(WriteFileText(root / L"outside" / L"keep.txt", "keep"));
  ASSERT_TRUE(MakeDir(tree / L"link"));
  ASSERT_TRUE(test::MakeJunction((tree / L"link").native(), (root / L"outside").native()));

  RemoveOutcome outcome;
  EXPECT_EQ(RemoveTreeOrTrash(tree, root, 0, &outcome), S_OK);
  EXPECT_TRUE(outcome.removed);
  EXPECT_FALSE(outcome.restartPending);
  EXPECT_TRUE(outcome.left.empty());
  EXPECT_TRUE(outcome.trash.empty());
  EXPECT_FALSE(Exists(tree));
  EXPECT_EQ(ReadFileText(root / L"outside" / L"keep.txt"), "keep");
  std::vector<std::wstring> names = Listing(root);
  EXPECT_EQ(names, std::vector<std::wstring>{L"outside"});  // no trash folder either
}

TEST(RemoveTree, NeverAJunctionItself) {
  test::TempDir dir(L"remove");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  ASSERT_TRUE(MakeDir(root / L"target"));
  ASSERT_TRUE(WriteFileText(root / L"target" / L"keep.txt", "keep"));
  ASSERT_TRUE(MakeDir(root / L"link"));
  ASSERT_TRUE(test::MakeJunction((root / L"link").native(), (root / L"target").native()));
  RemoveOutcome outcome;
  EXPECT_EQ(RemoveTreeOrTrash(root / L"link", root, 0, &outcome),
            HRESULT_FROM_WIN32(ERROR_STOPPED_ON_SYMLINK));
  EXPECT_FALSE(outcome.removed);
  EXPECT_EQ(ReadFileText(root / L"target" / L"keep.txt"), "keep");
  EXPECT_EQ(RemoveTreeNoFollow(root / L"target" / L"missing"), S_OK);
}

TEST(RemoveTree, MissingCountsAsRemoved) {
  test::TempDir dir(L"remove");
  ASSERT_TRUE(dir.ok());
  RemoveOutcome outcome;
  EXPECT_EQ(RemoveTreeOrTrash(path(dir.path()) / L"never-made", {}, 0, &outcome), S_OK);
  EXPECT_TRUE(outcome.removed);
  EXPECT_EQ(RemoveTreeOrTrash(L"relative", {}, 0, &outcome), E_INVALIDARG);
  EXPECT_EQ(RemoveTreeOrTrash(path(dir.path()).root_path(), {}, 0, &outcome), E_INVALIDARG);
}

// Unelevated, a file in use stays where it is, with everything else removed around it: the
// caller schedules its removal at the next sign-in. Elevated, it would go to the trash and be
// queued for deletion at the restart, which only the system tests do.
TEST(RemoveTree, AFileInUseStaysUnelevated) {
  if (Elevated()) GTEST_SKIP() << "elevated: the trash path belongs to the system tests";
  test::TempDir dir(L"remove");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  const path tree = root / L"tree";
  ASSERT_TRUE(MakeDir(tree));
  ASSERT_TRUE(MakeDir(tree / L"sub"));
  ASSERT_TRUE(WriteFileText(tree / L"free.txt", "free"));
  const path held = tree / L"sub" / L"held.txt";
  ASSERT_TRUE(WriteFileText(held, "held"));
  wil::unique_hfile user(CreateFileW(held.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
  ASSERT_TRUE(user);

  RemoveOutcome outcome;
  const ULONGLONG start = GetTickCount64();
  EXPECT_EQ(RemoveTreeOrTrash(tree, root, 600, &outcome), S_OK);
  EXPECT_GE(GetTickCount64() - start, 500u);  // it retried for the time given
  EXPECT_FALSE(outcome.removed);
  EXPECT_TRUE(outcome.restartPending);
  EXPECT_TRUE(outcome.trash.empty());
  ASSERT_EQ(outcome.left.size(), 1u);
  EXPECT_TRUE(IEquals(outcome.left[0].native(), held.native()));
  EXPECT_FALSE(Exists(tree / L"free.txt"));
  EXPECT_TRUE(Exists(held));
  EXPECT_EQ(Listing(root), std::vector<std::wstring>{L"tree"});  // no trash folder

  user.reset();
  EXPECT_EQ(RemoveTreeOrTrash(tree, root, 0, &outcome), S_OK);
  EXPECT_TRUE(outcome.removed);
  EXPECT_FALSE(outcome.restartPending);
}

// A DLL that a process has loaded cannot be deleted, but it can be renamed: that is what the
// trash is for. The test maps a copy of a Windows DLL as an image (as a resource: nothing in it
// runs) to hold it the way a loaded DLL is held.
TEST(Trash, MovesAnImageInUse) {
  test::TempDir dir(L"trash");
  ASSERT_TRUE(dir.ok());
  const path root = dir.path();
  wchar_t system[MAX_PATH] = {};
  ASSERT_NE(GetSystemDirectoryW(system, MAX_PATH), 0u);
  ASSERT_TRUE(MakeDir(root / L"build"));
  const path dll = root / L"build" / L"held.dll";
  ASSERT_TRUE(CopyFileW((path(system) / L"version.dll").c_str(), dll.c_str(), TRUE));
  HMODULE image = LoadLibraryExW(dll.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  ASSERT_NE(image, nullptr) << GetLastError();
  const auto unload = wil::scope_exit([image] { FreeLibrary(image); });

  EXPECT_FALSE(DeleteFileW(dll.c_str()));
  EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_ACCESS_DENIED));

  if (!Elevated()) {
    // Unelevated, removal leaves it in place.
    RemoveOutcome outcome;
    EXPECT_EQ(RemoveTreeOrTrash(root / L"build", root, 0, &outcome), S_OK);
    EXPECT_TRUE(outcome.restartPending);
    ASSERT_EQ(outcome.left.size(), 1u);
    EXPECT_TRUE(IEquals(outcome.left[0].native(), dll.native()));
  }

  path trash;
  ASSERT_EQ(MakeTrashFolder(root, &trash), S_OK);
  EXPECT_TRUE(IEquals(trash.parent_path().native(), root.native()));
  EXPECT_TRUE(IStartsWith(trash.filename().native(), std::wstring(kProductName) + L".trash-"));
  EXPECT_EQ(trash.filename().native().size(), std::wstring(kProductName).size() + 7 + 32);
  path moved;
  ASSERT_EQ(MoveIntoTrash(dll, trash, &moved), S_OK);
  EXPECT_FALSE(Exists(dll));
  EXPECT_TRUE(Exists(moved));
  EXPECT_TRUE(IEquals(moved.parent_path().native(), trash.native()));
  EXPECT_TRUE(IEquals(moved.extension().native(), L".dll"));
  // The folder it was in can go now.
  EXPECT_EQ(RemoveTreeNoFollow(root / L"build"), S_OK);
  EXPECT_EQ(MoveIntoTrash(root, trash), HRESULT_FROM_WIN32(ERROR_BAD_FILE_TYPE));
}

TEST(Trash, DeletingAtRestartNeedsAnAdministrator) {
  if (Elevated()) GTEST_SKIP() << "elevated: it would really queue the deletion";
  test::TempDir dir(L"trash");
  ASSERT_TRUE(dir.ok());
  const path file = path(dir.path()) / L"file.txt";
  ASSERT_TRUE(WriteFileText(file, "x"));
  EXPECT_EQ(ScheduleDeleteAtRestart(file), HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
  EXPECT_TRUE(Exists(file));
}

// ---- the clean-up at the next sign-in, in private keys ----

std::wstring CmdExe() {
  wchar_t system[MAX_PATH] = {};
  GetSystemDirectoryW(system, MAX_PATH);
  return (path(system) / L"cmd.exe").native();
}

std::optional<std::wstring> Value(HKEY root, const std::wstring& name) {
  std::optional<std::wstring> text;
  if (FAILED(ReadRegistryString(root, kRunOnceKey, name, &text))) return std::nullopt;
  return text;
}

TEST(RunOnceRemoval, OneValuePerFolderInTheSandbox) {
  test::ScopedRegistrySandbox sandbox;
  ASSERT_TRUE(sandbox.ok()) << sandbox.error();
  const RegistryRoots roots{sandbox.user(), sandbox.machine()};
  const std::vector<path> dirs = {L"D:\\Home\\pat\\Programs\\" + std::wstring(kProductName),
                                  L"D:\\Home\\pat\\Data\\" + std::wstring(kProductName)};
  std::vector<path> notScheduled = {L"stale"};
  ASSERT_EQ(ScheduleRunOnceRemoval(roots, dirs, &notScheduled), S_OK);
  EXPECT_TRUE(notScheduled.empty());
  std::wstring first;
  std::wstring second;
  ASSERT_EQ(RunOnceRemovalCommand(CmdExe(), dirs[0], &first), S_OK);
  ASSERT_EQ(RunOnceRemovalCommand(CmdExe(), dirs[1], &second), S_OK);
  EXPECT_EQ(Value(sandbox.user(), RunOnceValueName(0)), first);
  EXPECT_EQ(Value(sandbox.user(), RunOnceValueName(1)), second);
  // REG_SZ, written out: nothing is left for the sign-in to expand.
  std::optional<RegistryValue> raw;
  ASSERT_EQ(ReadRegistryValue(sandbox.user(), kRunOnceKey, RunOnceValueName(0), &raw), S_OK);
  ASSERT_TRUE(raw);
  EXPECT_EQ(raw->type, static_cast<DWORD>(REG_SZ));

  // Again: nothing new. A third folder takes the next name; one that cannot be carried is
  // reported and not written.
  const std::vector<path> more = {dirs[1], L"D:\\Home\\pat\\Third", L"D:\\%TEMP%\\x"};
  ASSERT_EQ(ScheduleRunOnceRemoval(roots, more, &notScheduled), S_FALSE);
  ASSERT_EQ(notScheduled.size(), 1u);
  EXPECT_EQ(notScheduled[0], more[2]);
  EXPECT_TRUE(Value(sandbox.user(), RunOnceValueName(2)).has_value());
  EXPECT_FALSE(Value(sandbox.user(), RunOnceValueName(3)).has_value());
  // Nothing went to the machine-wide stand-in.
  bool exists = true;
  ASSERT_EQ(RegistryKeyExists(sandbox.machine(), kRunOnceKey, &exists), S_OK);
  EXPECT_FALSE(exists);
}

TEST(RunOnceRemoval, DeletingTheFamilyKeepsEverythingElse) {
  test::ScopedRegistrySandbox sandbox;
  ASSERT_TRUE(sandbox.ok()) << sandbox.error();
  const RegistryRoots roots{sandbox.user(), sandbox.machine()};
  size_t deleted = 99;
  ASSERT_EQ(DeleteRunOnceRemovals(roots, &deleted), S_OK);  // no key at all
  EXPECT_EQ(deleted, 0u);

  const std::wstring product = kProductName;
  for (const std::wstring& name :
       {std::wstring(kLegacyRunOnce), product + L"Cleanup2", product + L"Cleanup7",
        product + L"CleanupX", std::wstring(L"OtherApp"), product + L"Cleanup"}) {
    ASSERT_EQ(WriteRegistryString(roots.user, kRunOnceKey, name, L"x"), S_OK);
  }
  ASSERT_EQ(DeleteRunOnceRemovals(roots, &deleted), S_OK);
  // 1.x's name and <Product>Cleanup are one value while the product keeps its 1.x name.
  const size_t expected = IEquals(kLegacyRunOnce, product + L"Cleanup") ? 3u : 4u;
  EXPECT_EQ(deleted, expected);
  EXPECT_FALSE(Value(roots.user, kLegacyRunOnce).has_value());
  EXPECT_FALSE(Value(roots.user, product + L"Cleanup2").has_value());
  EXPECT_FALSE(Value(roots.user, product + L"Cleanup7").has_value());
  EXPECT_TRUE(Value(roots.user, product + L"CleanupX").has_value());
  EXPECT_TRUE(Value(roots.user, L"OtherApp").has_value());
}

// ---- who else could change a build folder ----

// Gives |object| exactly the DACL in |sddl| (protected: nothing inherited) plus full control for
// this user, so the test can always clean up after itself.
void SetDacl(const path& object, const std::wstring& sddl) {
  std::wstring sid;
  ASSERT_EQ(GetCurrentUserSid(&sid), S_OK);
  const std::wstring full = L"D:P(A;OICI;FA;;;" + sid + L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)" + sddl;
  wil::unique_hlocal_security_descriptor descriptor;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(full.c_str(), SDDL_REVISION_1,
                                                                   descriptor.put(), nullptr))
      << GetLastError();
  BOOL present = FALSE;
  BOOL defaulted = FALSE;
  PACL dacl = nullptr;
  ASSERT_TRUE(GetSecurityDescriptorDacl(descriptor.get(), &present, &dacl, &defaulted));
  ASSERT_EQ(SetNamedSecurityInfoW(const_cast<wchar_t*>(object.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, dacl, nullptr),
            static_cast<DWORD>(ERROR_SUCCESS));
}

class BuildFolderAccess : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(dir_.ok());
    parent_ = path(dir_.path()) / L"parent";
    folder_ = parent_ / L"build";
    ASSERT_TRUE(MakeDir(parent_));
    ASSERT_TRUE(MakeDir(folder_));
    ASSERT_TRUE(WriteFileText(folder_ / L"a.exe", "a"));
    ASSERT_TRUE(WriteFileText(folder_ / L"b.dll", "b"));
    SetDacl(parent_, L"");
    SetDacl(folder_, L"");
    SetDacl(folder_ / L"a.exe", L"");
    SetDacl(folder_ / L"b.dll", L"");
  }

  // The check, skipped when the only finding is a folder above the test's own (the machine's
  // setup, which the test does not control).
  std::optional<UnsafeAccess> Check(const path& folder) {
    std::optional<UnsafeAccess> unsafe;
    const std::vector<std::wstring> binaries = {L"a.exe", L"b.dll"};
    EXPECT_EQ(FindUnsafeBuildFolderAccess(folder, binaries, &unsafe), S_OK);
    return unsafe;
  }

  bool OutsideTheTest(const std::optional<UnsafeAccess>& unsafe) const {
    return unsafe && !IsUnderLexically(unsafe->object, dir_.path()) &&
           !IEquals(unsafe->object.native(), dir_.path());
  }

  test::TempDir dir_{L"acl"};
  path parent_;
  path folder_;
};

TEST_F(BuildFolderAccess, OnlyTheUserAndAdministrators) {
  const std::optional<UnsafeAccess> unsafe = Check(folder_);
  if (OutsideTheTest(unsafe)) {
    GTEST_SKIP() << "a folder above the test's own lets others change it: "
                 << ::testing::PrintToString(unsafe->object.native());
  }
  EXPECT_FALSE(unsafe) << ::testing::PrintToString(unsafe->object.native());
}

TEST_F(BuildFolderAccess, AuthenticatedUsersMayModifyTheFolder) {
  SetDacl(folder_, L"(A;OICI;0x1301bf;;;AU)");  // Modify
  const std::optional<UnsafeAccess> unsafe = Check(folder_);
  ASSERT_TRUE(unsafe);
  path canonical;
  ASSERT_EQ(Canonicalize(folder_, &canonical), S_OK);
  EXPECT_TRUE(IEquals(unsafe->object.native(), canonical.native()));
  EXPECT_EQ(unsafe->access.principal, L"Authenticated Users");
  EXPECT_TRUE(unsafe->access.rights & FILE_WRITE_DATA);
}

TEST_F(BuildFolderAccess, UsersMayChangeOneBinarysPermissions) {
  SetDacl(folder_ / L"b.dll", L"(A;;WD;;;BU)");
  const std::optional<UnsafeAccess> unsafe = Check(folder_);
  ASSERT_TRUE(unsafe);
  EXPECT_TRUE(IEquals(unsafe->object.filename().native(), L"b.dll"));
  EXPECT_EQ(unsafe->access.principal, L"Users");
  EXPECT_EQ(unsafe->access.rights, static_cast<ACCESS_MASK>(WRITE_DAC));
}

TEST_F(BuildFolderAccess, InteractiveMayDeleteChildrenOfTheFolderAbove) {
  SetDacl(parent_, L"(A;;0x40;;;IU)");  // FILE_DELETE_CHILD, this folder only
  const std::optional<UnsafeAccess> unsafe = Check(folder_);
  ASSERT_TRUE(unsafe);
  EXPECT_TRUE(IEquals(unsafe->object.filename().native(), L"parent"));
  EXPECT_EQ(unsafe->access.principal, L"INTERACTIVE");
  EXPECT_EQ(unsafe->access.rights, static_cast<ACCESS_MASK>(FILE_DELETE_CHILD));
}

TEST_F(BuildFolderAccess, AddingFoldersAboveIsHarmless) {
  SetDacl(parent_, L"(A;;0x4;;;AU)");  // FILE_ADD_SUBDIRECTORY, like C:\ itself
  const std::optional<UnsafeAccess> unsafe = Check(folder_);
  if (OutsideTheTest(unsafe)) GTEST_SKIP() << "a folder above the test's own is open to others";
  EXPECT_FALSE(unsafe);
}

TEST_F(BuildFolderAccess, ThroughAJunctionTheRealFolderIsJudged) {
  SetDacl(folder_, L"(A;;FA;;;WD)");
  const path link = path(dir_.path()) / L"link";
  ASSERT_TRUE(MakeDir(link));
  ASSERT_TRUE(test::MakeJunction(link.native(), folder_.native()));
  const std::optional<UnsafeAccess> unsafe = Check(link);
  ASSERT_TRUE(unsafe);
  EXPECT_TRUE(IEquals(unsafe->object.filename().native(), L"build"));
  EXPECT_EQ(unsafe->access.principal, L"Everyone");
}

TEST_F(BuildFolderAccess, AMissingBinaryFails) {
  std::optional<UnsafeAccess> unsafe;
  const std::vector<std::wstring> binaries = {L"a.exe", L"missing.dll"};
  EXPECT_EQ(FindUnsafeBuildFolderAccess(folder_, binaries, &unsafe),
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND));
  const std::vector<std::wstring> escaping = {L"..\\a.exe"};  // in the folder above
  ASSERT_TRUE(WriteFileText(parent_ / L"a.exe", "a"));
  EXPECT_EQ(FindUnsafeBuildFolderAccess(folder_, escaping, &unsafe), E_INVALIDARG);
}

// The folder this test program and the executables were built into: inside the user's profile,
// it must pass, so a developer install from it is allowed. Printed everywhere; asserted only
// off CI, where the checkout's drive is set up by the runner image.
TEST(BuildFolderAccessHere, TheBuildFolderPasses) {
  std::optional<UnsafeAccess> unsafe;
  const std::vector<std::wstring> binaries = {kSettingsExe, kCliExe};
  ASSERT_EQ(FindUnsafeBuildFolderAccess(test::SelfFolder(), binaries, &unsafe), S_OK);
  if (unsafe) {
    std::printf("build folder check: %ls may be changed by %ls (0x%08lX)\n", unsafe->object.c_str(),
                unsafe->access.principal.c_str(), unsafe->access.rights);
  } else {
    std::printf("build folder check: only the user and administrators can change it\n");
  }
  wchar_t ci[8] = {};
  if (GetEnvironmentVariableW(L"GITHUB_ACTIONS", ci, ARRAYSIZE(ci)) > 0) {
    GTEST_SKIP() << "on CI the result is only printed";
  }
  EXPECT_FALSE(unsafe);
}

}  // namespace
}  // namespace pdft
