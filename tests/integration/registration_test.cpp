// The registration (registration.h) against the registry, through RegistryRoots that point at a
// private key per test (ScopedRegistrySandbox): the class, the slots, their removal and pruning,
// class descriptions and the default app. Nothing here reads or writes the real Software\Classes:
// the Windows-wide association queries are replaced by a fake, except for one read-only call.

#include <windows.h>

#include "registration.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <string>

#include "child_process.h"
#include "pe_info.h"
#include "project_config.h"
#include "registry_sandbox.h"
#include "strings.h"

namespace pdft {
namespace {

const std::wstring kOurs = kHandlerClsidString;
const std::wstring kOther = L"{9C60DE1E-E5FC-40F4-A487-460851A8D915}";

#if defined(_M_ARM64)
constexpr USHORT kOwnMachine = IMAGE_FILE_MACHINE_ARM64;
#else
constexpr USHORT kOwnMachine = IMAGE_FILE_MACHINE_AMD64;
#endif

// Raw access to the sandbox, for setting up and checking what the code under test did.
void SetString(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value,
               DWORD type = REG_SZ) {
  ASSERT_EQ(RegSetKeyValueW(root, key.c_str(), name, type, value.c_str(),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))),
            ERROR_SUCCESS);
}

void SetDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD value) {
  ASSERT_EQ(RegSetKeyValueW(root, key.c_str(), name, REG_DWORD, &value, sizeof(value)),
            ERROR_SUCCESS);
}

void CreateKey(HKEY root, const std::wstring& key) {
  HKEY created = nullptr;
  ASSERT_EQ(
      RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &created, nullptr),
      ERROR_SUCCESS);
  RegCloseKey(created);
}

bool KeyExists(HKEY root, const std::wstring& key) {
  HKEY opened = nullptr;
  if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &opened) != ERROR_SUCCESS) return false;
  RegCloseKey(opened);
  return true;
}

// A REG_SZ value as stored, with its type checked.
std::optional<std::wstring> GetSz(HKEY root, const std::wstring& key, const wchar_t* name) {
  wchar_t buffer[1024] = {};
  DWORD size = sizeof(buffer);
  DWORD type = 0;
  if (RegGetValueW(root, key.c_str(), name, RRF_RT_ANY | RRF_NOEXPAND, &type, buffer, &size) !=
          ERROR_SUCCESS ||
      type != REG_SZ) {
    return std::nullopt;
  }
  return std::wstring(buffer);
}

class Registration : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sandbox_.ok()) << HResultText(sandbox_.error());
    roots_ = {sandbox_.user(), sandbox_.machine()};
  }

  std::wstring BuiltDll() const { return test::SelfFolder() + L"\\" + kHandlerDll; }

  test::ScopedRegistrySandbox sandbox_;
  RegistryRoots roots_;
};

TEST_F(Registration, SandboxRootsAreNotTheSystemOnes) {
  EXPECT_FALSE(UsesSystemRoots(roots_));
  // So nothing tells Explorer about changes made here.
  EXPECT_FALSE(NotifyAssocChanged(roots_));
}

TEST_F(Registration, WriteReadDeleteTheUserClass) {
  std::optional<ClassReg> reg;
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  EXPECT_FALSE(reg);

  ASSERT_HRESULT_SUCCEEDED(WriteClass(roots_, Scope::kUser, BuiltDll()));
  // Exactly what 1.x wrote, all REG_SZ.
  const std::wstring inproc = ClassKeyPath() + L"\\InprocServer32";
  EXPECT_EQ(GetSz(sandbox_.user(), ClassKeyPath(), nullptr), kHandlerClassName);
  EXPECT_EQ(GetSz(sandbox_.user(), inproc, nullptr), BuiltDll());
  EXPECT_EQ(GetSz(sandbox_.user(), inproc, L"ThreadingModel"), L"Apartment");
  EXPECT_FALSE(KeyExists(sandbox_.machine(), ClassKeyPath()));

  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  ASSERT_TRUE(reg);
  EXPECT_EQ(reg->name, kHandlerClassName);
  EXPECT_EQ(reg->dll, BuiltDll());
  EXPECT_EQ(reg->threading, L"Apartment");
  EXPECT_TRUE(reg->dllExists);
  EXPECT_TRUE(reg->pdfiumExists);
  EXPECT_EQ(reg->peMachine, kOwnMachine);
  EXPECT_EQ(reg->fileVersion, kFileVersion);
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg));
  EXPECT_FALSE(reg);

  ASSERT_HRESULT_SUCCEEDED(DeleteClass(roots_, Scope::kUser));
  EXPECT_FALSE(KeyExists(sandbox_.user(), ClassKeyPath()));
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  EXPECT_FALSE(reg);
  // Nothing left to delete is fine.
  EXPECT_HRESULT_SUCCEEDED(DeleteClass(roots_, Scope::kUser));
}

TEST_F(Registration, WriteClassRefusesTheMachineScopeAndOddPaths) {
  EXPECT_EQ(WriteClass(roots_, Scope::kMachine, BuiltDll()), E_INVALIDARG);
  EXPECT_EQ(WriteClass(roots_, Scope::kNone, BuiltDll()), E_INVALIDARG);
  EXPECT_EQ(DeleteClass(roots_, Scope::kMachine), E_INVALIDARG);
  EXPECT_EQ(WriteClass(roots_, Scope::kUser, std::wstring(kHandlerDll)), E_INVALIDARG);
  EXPECT_EQ(WriteClass(roots_, Scope::kUser, test::SelfFolder() + L"\\..\\x.dll"), E_INVALIDARG);
  EXPECT_FALSE(KeyExists(sandbox_.user(), ClassKeyPath()));
  EXPECT_FALSE(KeyExists(sandbox_.machine(), ClassKeyPath()));
}

TEST_F(Registration, ReadsTheMachineClassWithExpandedPaths) {
  const std::wstring inproc = ClassKeyPath() + L"\\InprocServer32";
  SetString(sandbox_.machine(), inproc, nullptr, L"%SystemRoot%\\no-such-folder\\x.dll",
            REG_EXPAND_SZ);
  SetString(sandbox_.machine(), inproc, L"ThreadingModel", L"Both");
  std::optional<ClassReg> reg;
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg));
  ASSERT_TRUE(reg);
  wchar_t windows[MAX_PATH] = {};
  ASSERT_NE(GetEnvironmentVariableW(L"SystemRoot", windows, MAX_PATH), 0u);
  EXPECT_EQ(reg->dll, std::wstring(windows) + L"\\no-such-folder\\x.dll");
  EXPECT_EQ(reg->threading, L"Both");
  EXPECT_EQ(reg->name, L"");
  EXPECT_FALSE(reg->dllExists);
  EXPECT_FALSE(reg->pdfiumExists);
  EXPECT_FALSE(reg->peMachine);
  EXPECT_EQ(reg->fileVersion, L"");
}

TEST_F(Registration, BrokenClassValues) {
  const std::wstring inproc = ClassKeyPath() + L"\\InprocServer32";
  std::optional<ClassReg> reg;
  // An empty path is no registration (1.x).
  SetString(sandbox_.user(), inproc, nullptr, L"");
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  EXPECT_FALSE(reg);
  // A path of the wrong type: registered, but broken.
  SetDword(sandbox_.user(), inproc, nullptr, 7);
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  ASSERT_TRUE(reg);
  EXPECT_EQ(reg->dll, L"");
  EXPECT_FALSE(reg->dllExists);
  // A relative path never counts as existing, even where the file is in the current folder.
  SetString(sandbox_.user(), inproc, nullptr, kHandlerDll);
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  ASSERT_TRUE(reg);
  EXPECT_EQ(reg->dll, kHandlerDll);
  EXPECT_FALSE(reg->dllExists);
  // A threading model of the wrong type reads as none.
  SetString(sandbox_.user(), inproc, nullptr, BuiltDll());
  SetDword(sandbox_.user(), inproc, L"ThreadingModel", 1);
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kUser, &reg));
  ASSERT_TRUE(reg);
  EXPECT_EQ(reg->threading, L"");
  EXPECT_TRUE(reg->dllExists);
}

TEST_F(Registration, FillEmptySlotsLeavesAnotherHandlerAlone) {
  SetString(sandbox_.user(), SlotKeyPath(1), nullptr, kOther);
  std::array<bool, kSlotCount> changed = {};
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kFillEmpty, &changed));
  EXPECT_TRUE(changed[0]);
  EXPECT_FALSE(changed[1]);
  SlotValues user;
  SlotValues machine;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &user, &machine));
  EXPECT_EQ(user[0], kOurs);
  EXPECT_EQ(user[1], kOther);
  EXPECT_FALSE(machine[0]);
  EXPECT_FALSE(machine[1]);
  // Again: nothing to do.
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kFillEmpty, &changed));
  EXPECT_FALSE(changed[0]);
  EXPECT_FALSE(changed[1]);
}

TEST_F(Registration, EmptyAndWrongTypeSlotsCountAsEmpty) {
  SetString(sandbox_.user(), SlotKeyPath(0), nullptr, L"");
  SetDword(sandbox_.user(), SlotKeyPath(1), nullptr, 1);
  SlotValues user;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &user));
  EXPECT_EQ(user[0], L"");
  EXPECT_FALSE(user[1]);
  std::array<bool, kSlotCount> changed = {};
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kFillEmpty, &changed));
  EXPECT_TRUE(changed[0]);
  EXPECT_TRUE(changed[1]);
  EXPECT_EQ(GetSz(sandbox_.user(), SlotKeyPath(1), nullptr), kOurs);
}

TEST_F(Registration, TakeBackOverwritesAnotherHandler) {
  SetString(sandbox_.user(), SlotKeyPath(0), nullptr, kOther);
  SetString(sandbox_.user(), SlotKeyPath(1), nullptr, kOurs);
  std::array<bool, kSlotCount> changed = {};
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack, &changed));
  EXPECT_TRUE(changed[0]);
  EXPECT_FALSE(changed[1]);
  EXPECT_EQ(GetSz(sandbox_.user(), SlotKeyPath(0), nullptr), kOurs);
  EXPECT_EQ(GetSz(sandbox_.user(), SlotKeyPath(1), nullptr), kOurs);
}

TEST_F(Registration, MachineSlotsAreReadForInformation) {
  SetString(sandbox_.machine(), SlotKeyPath(0), nullptr, kOther);
  SlotValues user;
  SlotValues machine;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &user, &machine));
  EXPECT_EQ(machine[0], kOther);
  EXPECT_FALSE(machine[1]);
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &user, &machine));
  EXPECT_EQ(machine[0], kOther);  // never written
}

TEST_F(Registration, RemoveSlotsIfOursPrunesEmptyParents) {
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
  CreateKey(sandbox_.user(), L"Software\\Classes\\CLSID");  // a sibling that must stay
  std::array<bool, kSlotCount> removed = {};
  ASSERT_HRESULT_SUCCEEDED(RemoveSlotsIfOurs(roots_, &removed));
  EXPECT_TRUE(removed[0]);
  EXPECT_TRUE(removed[1]);
  for (const wchar_t* parent : kSlotParentKeys) {
    EXPECT_FALSE(KeyExists(sandbox_.user(), parent))
        << ::testing::PrintToString(std::wstring(parent));
  }
  EXPECT_TRUE(KeyExists(sandbox_.user(), L"Software\\Classes\\CLSID"));
  // Again, with nothing left: fine.
  ASSERT_HRESULT_SUCCEEDED(RemoveSlotsIfOurs(roots_, &removed));
  EXPECT_FALSE(removed[0]);
  EXPECT_FALSE(removed[1]);
}

TEST_F(Registration, RemoveSlotsIfOursKeepsOthersAndNonEmptyKeys) {
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
  // Slot 1 belongs to another handler; .pdf holds only a default value (an app's ProgID).
  SetString(sandbox_.user(), SlotKeyPath(1), nullptr, kOther);
  SetString(sandbox_.user(), L"Software\\Classes\\.pdf", nullptr, L"Some.Document");
  std::array<bool, kSlotCount> removed = {};
  ASSERT_HRESULT_SUCCEEDED(RemoveSlotsIfOurs(roots_, &removed));
  EXPECT_TRUE(removed[0]);
  EXPECT_FALSE(removed[1]);
  EXPECT_FALSE(KeyExists(sandbox_.user(), SlotKeyPath(0)));
  EXPECT_FALSE(KeyExists(sandbox_.user(), L"Software\\Classes\\.pdf\\ShellEx"));
  EXPECT_TRUE(KeyExists(sandbox_.user(), L"Software\\Classes\\.pdf"));  // its default value
  EXPECT_EQ(GetSz(sandbox_.user(), SlotKeyPath(1), nullptr), kOther);
  EXPECT_TRUE(KeyExists(sandbox_.user(), L"Software\\Classes\\SystemFileAssociations"));
}

TEST_F(Registration, DescribeClsidReadsLikeClassesRoot) {
  const std::wstring key = L"Software\\Classes\\CLSID\\" + kOther;
  SetString(sandbox_.machine(), key, nullptr, L"Machine name");
  SetString(sandbox_.machine(), key + L"\\InprocServer32", nullptr, L"C:\\machine.dll");
  ClsidDescription description;
  ASSERT_HRESULT_SUCCEEDED(DescribeClsid(roots_, kOther, &description));
  EXPECT_EQ(description.name, L"Machine name");
  EXPECT_EQ(description.dll, L"C:\\machine.dll");
  // A per-user value wins, value by value.
  SetString(sandbox_.user(), key, nullptr, L"User name");
  ASSERT_HRESULT_SUCCEEDED(DescribeClsid(roots_, kOther, &description));
  EXPECT_EQ(description.name, L"User name");
  EXPECT_EQ(description.dll, L"C:\\machine.dll");
  // Unknown, and not a CLSID at all (never becomes part of a key path).
  ASSERT_HRESULT_SUCCEEDED(
      DescribeClsid(roots_, L"{00000000-0000-0000-0000-000000000001}", &description));
  EXPECT_FALSE(description.name);
  EXPECT_FALSE(description.dll);
  SetString(sandbox_.user(), L"Software\\Classes\\CLSID\\x\\y", nullptr, L"planted");
  ASSERT_HRESULT_SUCCEEDED(DescribeClsid(roots_, L"x\\y", &description));
  EXPECT_FALSE(description.name);
}

class FakeQueries final : public AssociationQueries {
 public:
  std::optional<std::wstring> handler;
  std::optional<std::wstring> progId;
  HRESULT ThumbnailHandler(std::optional<std::wstring>* clsid) const noexcept override {
    *clsid = handler;
    return S_OK;
  }
  HRESULT ProgId(std::optional<std::wstring>* id) const noexcept override {
    *id = progId;
    return S_OK;
  }
};

TEST_F(Registration, EffectiveHandlerComesFromTheQueries) {
  FakeQueries queries;
  std::optional<std::wstring> clsid = L"x";
  ASSERT_HRESULT_SUCCEEDED(QueryEffectiveHandler(queries, &clsid));
  EXPECT_FALSE(clsid);
  queries.handler = kOurs;
  ASSERT_HRESULT_SUCCEEDED(QueryEffectiveHandler(queries, &clsid));
  EXPECT_EQ(clsid, kOurs);
}

TEST_F(Registration, DefaultAppHandlerAndOverlay) {
  FakeQueries queries;
  DefaultApp app;
  ASSERT_HRESULT_SUCCEEDED(QueryDefaultApp(roots_, queries, &app));
  EXPECT_FALSE(app.progId);
  EXPECT_FALSE(app.handler);
  EXPECT_FALSE(app.overlay);

  queries.progId = L"Test.Document";
  ASSERT_HRESULT_SUCCEEDED(QueryDefaultApp(roots_, queries, &app));
  EXPECT_EQ(app.progId, L"Test.Document");
  EXPECT_FALSE(app.handler);
  EXPECT_FALSE(app.overlay);  // none: Windows draws the app's icon on thumbnails

  SetString(sandbox_.machine(),
            L"Software\\Classes\\Test.Document\\ShellEx\\" + std::wstring(kThumbSlotIidString),
            nullptr, kOther);
  SetString(sandbox_.user(), L"Software\\Classes\\Test.Document", L"TypeOverlay", L"");
  ASSERT_HRESULT_SUCCEEDED(QueryDefaultApp(roots_, queries, &app));
  EXPECT_EQ(app.handler, kOther);
  EXPECT_EQ(app.overlay, L"");  // present and empty: no overlay

  // A ProgID that cannot be a key name is reported, not looked up.
  queries.progId = L"..\\Test.Document";
  ASSERT_HRESULT_SUCCEEDED(QueryDefaultApp(roots_, queries, &app));
  EXPECT_EQ(app.progId, L"..\\Test.Document");
  EXPECT_FALSE(app.handler);
  EXPECT_FALSE(app.overlay);
}

TEST_F(Registration, WindowsAnswersTheRealQueries) {
  // Read-only: asks Windows what it resolves for .pdf in this process. The answers depend on the
  // PC (and on a container's registry view), so only that they come back is checked.
  std::optional<std::wstring> clsid;
  EXPECT_HRESULT_SUCCEEDED(QueryEffectiveHandler(SystemAssociationQueries(), &clsid));
  if (clsid) EXPECT_TRUE(IsGuidString(*clsid)) << WideToUtf8Lossy(*clsid);
  std::optional<std::wstring> progId;
  EXPECT_HRESULT_SUCCEEDED(SystemAssociationQueries().ProgId(&progId));
}

}  // namespace
}  // namespace pdft
