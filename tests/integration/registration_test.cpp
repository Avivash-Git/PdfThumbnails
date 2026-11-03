// The registration (registration.h) against the registry, through RegistryRoots that point at a
// private key per test (ScopedRegistrySandbox): the class, the slots, their removal and pruning,
// class descriptions and the default app. Nothing here reads or writes the real Software\Classes:
// the Windows-wide association queries are replaced by a fake, except for one read-only call.
//
// Also how keys are deleted (registry_roots.h), in the same private key: whole trees, never
// through a registry link (links are made inside the private key, to keys there), within the
// bounds of one call, and only through the handle of the key itself (a key that may not be deleted
// fails only when it is empty); and what the steps for all
// users do with that: a machine-wide class written into a fresh key, a class path stored to be
// expanded treated as foreign, a link in another user's hive left as it is, and the uninstaller's
// clean-up step gathering nothing for the user's part when elevated. Folders are made under
// %TEMP%; the files in them are empty stand-ins that never run.

#include <windows.h>

#include "registration.h"

#include <gtest/gtest.h>
#include <sddl.h>
#include <wil/resource.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "child_process.h"
#include "fake_probes.h"
#include "instance.h"
#include "migrate_v1.h"
#include "pe_info.h"
#include "project_config.h"
#include "registry_sandbox.h"
#include "strings.h"
#include "temp_dir.h"
#include "uninstall.h"

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

std::optional<DWORD> GetDword(HKEY root, const std::wstring& key, const wchar_t* name) {
  DWORD value = 0;
  DWORD size = sizeof(value);
  if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) !=
      ERROR_SUCCESS) {
    return std::nullopt;
  }
  return value;
}

// Creates |key| below |root| (with its parents) and opens it.
wil::unique_hkey CreateOpen(HKEY root, const std::wstring& key) {
  wil::unique_hkey created;
  EXPECT_EQ(RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                            created.put(), nullptr),
            ERROR_SUCCESS)
      << WideToUtf8Lossy(key);
  return created;
}

// Opens |key| below |root| as it is (a link itself, not the key it names), to look at it and to
// delete it.
wil::unique_hkey OpenItself(HKEY root, const std::wstring& key) {
  wil::unique_hkey opened;
  EXPECT_EQ(RegOpenKeyExW(root, key.c_str(), REG_OPTION_OPEN_LINK, KEY_READ | DELETE, opened.put()),
            ERROR_SUCCESS)
      << WideToUtf8Lossy(key);
  return opened;
}

// The kernel's name of the key |key| ("\REGISTRY\USER\<SID>\..."): what a registry link names.
// No documented call gives it, so this asks ntdll's NtQueryKey (KeyNameInformation).
std::wstring KernelKeyName(HKEY key) {
  using NtQueryKeyFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
  constexpr int kKeyNameInformation = 3;
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto query = ntdll ? reinterpret_cast<NtQueryKeyFn>(
                                 reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryKey")))
                           : nullptr;
  if (!query) return {};
  std::vector<BYTE> buffer(4096);
  ULONG size = 0;
  if (query(key, kKeyNameInformation, buffer.data(), static_cast<ULONG>(buffer.size()), &size) <
          0 ||
      size < sizeof(ULONG)) {
    return {};
  }
  // KEY_NAME_INFORMATION: the name's length in bytes, then the name.
  ULONG length = 0;
  std::memcpy(&length, buffer.data(), sizeof(length));
  if (length > size - sizeof(ULONG)) return {};
  std::wstring name(length / sizeof(wchar_t), L'\0');
  std::memcpy(name.data(), buffer.data() + sizeof(ULONG), name.size() * sizeof(wchar_t));
  return name;
}

// A registry link: the key |name| below |parent|, leading to the key whose kernel name is
// |target| (KernelKeyName). Deleted itself when this goes: the sandbox's own clean-up would
// follow it.
class ScopedLink {
 public:
  ScopedLink(HKEY parent, const std::wstring& name, const std::wstring& target) {
    status_ = RegCreateKeyExW(parent, name.c_str(), 0, nullptr, REG_OPTION_CREATE_LINK,
                              KEY_ALL_ACCESS, nullptr, key_.put(), nullptr);
    if (status_ != ERROR_SUCCESS) return;
    // The target's name without a terminating null.
    status_ = RegSetValueExW(key_.get(), L"SymbolicLinkValue", 0, REG_LINK,
                             reinterpret_cast<const BYTE*>(target.c_str()),
                             static_cast<DWORD>(target.size() * sizeof(wchar_t)));
  }
  ScopedLink(const ScopedLink&) = delete;
  ScopedLink& operator=(const ScopedLink&) = delete;
  ~ScopedLink() {
    if (key_) (void)DeleteOpenRegistryKey(key_.get());
  }

  LSTATUS status() const { return status_; }

 private:
  wil::unique_hkey key_;
  LSTATUS status_ = ERROR_SUCCESS;
};

bool Exists(const std::filesystem::path& path) {
  return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// An empty file at |path|, in folders made for it: a stand-in that never runs.
bool MakeFile(const std::filesystem::path& path) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  wil::unique_hfile file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
  return static_cast<bool>(file);
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

// ---- The class for all users -----------------------------------------------------------------

TEST_F(Registration, TheMachineClassIsWrittenIntoAFreshKey) {
  test::TempDir temp(L"registration");
  ASSERT_TRUE(temp.ok());
  const std::filesystem::path machineRoot = std::filesystem::path(temp.path()) / L"machine";
  const std::filesystem::path build = machineRoot / L"app" / L"2.0.0.0-1a2b3c4d";
  ASSERT_TRUE(MakeFile(build / kHandlerDll));
  ASSERT_TRUE(MakeFile(build / kPdfiumDll));
  // What another tool left in the class key: another server, a TreatAs, values of its own.
  const std::wstring inproc = ClassKeyPath() + L"\\InprocServer32";
  SetString(sandbox_.machine(), ClassKeyPath(), L"AppID", kOther);
  SetString(sandbox_.machine(), ClassKeyPath() + L"\\TreatAs", nullptr, kOther);
  SetString(sandbox_.machine(), ClassKeyPath() + L"\\LocalServer32", nullptr, L"C:\\x\\x.exe");
  SetString(sandbox_.machine(), inproc, nullptr, L"C:\\x\\x.dll");
  SetString(sandbox_.machine(), inproc, L"Extra", L"x");
  // Another class next to it.
  const std::wstring otherClass = L"Software\\Classes\\CLSID\\" + kOther;
  SetString(sandbox_.machine(), otherClass, nullptr, L"Other");

  ASSERT_HRESULT_SUCCEEDED(WriteMachineClass(roots_, machineRoot, build / kHandlerDll));
  EXPECT_EQ(GetSz(sandbox_.machine(), ClassKeyPath(), nullptr), kHandlerClassName);
  EXPECT_EQ(GetSz(sandbox_.machine(), inproc, nullptr), (build / kHandlerDll).native());
  EXPECT_EQ(GetSz(sandbox_.machine(), inproc, L"ThreadingModel"), L"Apartment");
  EXPECT_FALSE(GetSz(sandbox_.machine(), ClassKeyPath(), L"AppID"));
  EXPECT_FALSE(GetSz(sandbox_.machine(), inproc, L"Extra"));
  EXPECT_FALSE(KeyExists(sandbox_.machine(), ClassKeyPath() + L"\\TreatAs"));
  EXPECT_FALSE(KeyExists(sandbox_.machine(), ClassKeyPath() + L"\\LocalServer32"));
  EXPECT_EQ(GetSz(sandbox_.machine(), otherClass, nullptr), L"Other");
}

TEST_F(Registration, AnExpandableMachineClassIsForeignToTheStepsForAllUsers) {
  const std::wstring inproc = ClassKeyPath() + L"\\InprocServer32";
  const std::wstring stored = L"%SystemRoot%\\no-such-folder\\x.dll";
  SetString(sandbox_.machine(), inproc, nullptr, stored, REG_EXPAND_SZ);
  std::optional<ClassReg> reg;
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg, ClassPathExpansion::kNever));
  ASSERT_TRUE(reg);
  EXPECT_EQ(reg->dll, stored);
  EXPECT_TRUE(reg->dllExpandable);
  EXPECT_FALSE(reg->dllExists);
  ASSERT_HRESULT_SUCCEEDED(ReadClass(roots_, Scope::kMachine, &reg));
  ASSERT_TRUE(reg);
  EXPECT_NE(reg->dll, stored);  // expanded
  EXPECT_TRUE(reg->dllExpandable);

  // Two 1.x builds for all users, and a class whose path expands to one of them: as the
  // environment decides that, every 1.x build stays.
  test::TempDir temp(L"registration");
  ASSERT_TRUE(temp.ok());
  const std::filesystem::path root = std::filesystem::path(temp.path()) / L"legacy";
  const std::filesystem::path used = root / L"app" / L"1.1.0.0-67a1323e";
  const std::filesystem::path other = root / L"app" / L"1.0.0.0-0badf00d";
  ASSERT_TRUE(MakeFile(used / kHandlerDll));
  ASSERT_TRUE(MakeFile(other / kHandlerDll));
  constexpr wchar_t kVariable[] = L"PDFT_TEST_LEGACY_ROOT";
  ASSERT_TRUE(SetEnvironmentVariableW(kVariable, root.c_str()));
  const auto unset =
      wil::scope_exit([&]() noexcept { SetEnvironmentVariableW(kVariable, nullptr); });
  SetString(sandbox_.machine(), inproc, nullptr,
            L"%" + std::wstring(kVariable) + L"%\\app\\1.1.0.0-67a1323e\\" + kHandlerDll,
            REG_EXPAND_SZ);
  MachineRemoval removal;
  ASSERT_HRESULT_SUCCEEDED(MigrateV1Machine(roots_, root, temp.path(), &removal));
  EXPECT_TRUE(removal.classForeign);
  EXPECT_FALSE(removal.classInV1);
  EXPECT_TRUE(removal.removed.empty());
  EXPECT_TRUE(Exists(used));
  EXPECT_TRUE(Exists(other));
  bool needed = true;
  ASSERT_HRESULT_SUCCEEDED(MachineV1RemovalNeeded(roots_, root, &needed));
  EXPECT_FALSE(needed);

  // The same path stored as it is names its build exactly: only the other one goes.
  SetString(sandbox_.machine(), inproc, nullptr, (used / kHandlerDll).native());
  ASSERT_HRESULT_SUCCEEDED(MigrateV1Machine(roots_, root, temp.path(), &removal));
  EXPECT_FALSE(removal.classForeign);
  EXPECT_TRUE(removal.classInV1);
  EXPECT_EQ(removal.removed, std::vector<std::wstring>{L"1.0.0.0-0badf00d"});
  EXPECT_TRUE(Exists(used));
  EXPECT_FALSE(Exists(other));
}

// ---- Other users' hives, and the uninstaller's clean-up step ---------------------------------

TEST_F(Registration, TheStepForAllUsersLeavesALinkInAUsersHive) {
  test::TempDir temp(L"registration");
  ASSERT_TRUE(temp.ok());
  wil::unique_hkey users = CreateOpen(sandbox_.user(), L"Users");
  wil::unique_hkey hive = CreateOpen(users.get(),
                                     L"S-1-5-21-"
                                     L"1-2-3-1001");
  ASSERT_TRUE(users && hive);
  // In the user's hive: a key they may only read (a stand-in), the product key made a link to
  // it, and the handler's class.
  SetDword(hive.get(), L"Software\\Policies\\Rules", L"rule", 7);
  SetString(hive.get(), ClassKeyPath() + L"\\InprocServer32", nullptr, L"C:\\x\\x.dll");
  const std::wstring product = kRegKey;
  const size_t split = product.rfind(L'\\');
  ASSERT_NE(split, std::wstring::npos);
  wil::unique_hkey rules = OpenItself(hive.get(), L"Software\\Policies\\Rules");
  wil::unique_hkey parent = CreateOpen(hive.get(), product.substr(0, split));
  ScopedLink link(parent.get(), product.substr(split + 1), KernelKeyName(rules.get()));
  ASSERT_EQ(link.status(), ERROR_SUCCESS);
  ASSERT_EQ(GetDword(hive.get(), product, L"rule"), 7u);  // the link leads there

  MachineUninstallOptions options;
  options.machineRoot = std::filesystem::path(temp.path()) / L"machine";
  options.taskFolder.clear();
  options.users = users.get();
  options.retryMs = 0;
  MachineUninstallResult result;
  ASSERT_HRESULT_SUCCEEDED(UninstallForAllUsers(options, roots_, &result));
  EXPECT_EQ(result.hivesCleaned, 1u);  // the class went
  EXPECT_EQ(result.hivesLeft, 1u);
  EXPECT_EQ(result.firstError, kRegistryTreeLeftHr);
  EXPECT_EQ(result.failures,
            std::vector<std::wstring>{L"hive left: " + HResultText(kRegistryTreeLeftHr)});
  EXPECT_FALSE(KeyExists(hive.get(), ClassKeyPath()));
  EXPECT_EQ(GetDword(hive.get(), L"Software\\Policies\\Rules", L"rule"), 7u);
}

TEST_F(Registration, TheCleanupStepGathersNothingForTheUsersPartWhenElevated) {
  test::TempDir temp(L"registration");
  ASSERT_TRUE(temp.ok());
  ASSERT_HRESULT_SUCCEEDED(WriteSlots(roots_, SlotWrite::kTakeBack));
  ASSERT_HRESULT_SUCCEEDED(WriteClass(roots_, Scope::kUser, BuiltDll()));
  SetDword(sandbox_.user(), kRegKey, L"Zoom", 150);
  test::FakeProbes probes;
  probes.context.elevated = true;
  const std::filesystem::path base = temp.path();
  EnginePaths paths;
  paths.userRoot = base / L"user-root";
  paths.machineRoot = base / L"machine-root";
  paths.legacyUserRoot = paths.userRoot;
  paths.legacyMachineRoot = paths.machineRoot;
  paths.legacyIni = paths.userRoot / L"settings.ini";
  paths.ownExe = test::SelfPath();
  UninstallCleanupOptions options;
  options.paths = paths;
  options.dataDir = base / L"data";
  options.runMachinePart = false;

  UninstallCleanupResult result;
  ASSERT_HRESULT_SUCCEEDED(UninstallCleanup(options, roots_, probes, &result));
  for (const std::wstring& failure : result.failures) ADD_FAILURE() << WideToUtf8Lossy(failure);
  EXPECT_TRUE(result.userPart);
  EXPECT_FALSE(result.machinePart);
  // The user's keys went by their names...
  SlotValues slots;
  ASSERT_HRESULT_SUCCEEDED(ReadSlots(roots_, &slots));
  EXPECT_FALSE(slots[0]);
  EXPECT_FALSE(slots[1]);
  EXPECT_FALSE(KeyExists(sandbox_.user(), ClassKeyPath()));
  EXPECT_FALSE(KeyExists(sandbox_.user(), kRegKey));
  // ... with nothing gathered first: Windows was never asked about .pdf, no task was touched.
  EXPECT_EQ(probes.queries.calls, 0);
  EXPECT_TRUE(probes.applied.empty());
}

// ---- Deleting registry trees (registry_roots.h) ------------------------------------------------

class RegistryTree : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sandbox_.ok()) << HResultText(sandbox_.error());
    root_ = sandbox_.user();
  }

  // A key below the stand-in root with a value in it, and its parents.
  void Make(const std::wstring& key) { SetDword(root_, key, L"v", 1); }

  test::ScopedRegistrySandbox sandbox_;
  HKEY root_ = nullptr;
};

TEST_F(RegistryTree, AnOrdinaryTreeGoesWhole) {
  for (const wchar_t* key : {L"tree\\a\\b", L"tree\\c", L"keep"}) Make(key);
  RegistryTreeDeletion deletion;
  ASSERT_EQ(DeleteRegistryTree(root_, L"tree", RegistryTreeLimits{}, &deletion), S_OK);
  EXPECT_TRUE(deletion.existed);
  EXPECT_TRUE(deletion.deleted);
  EXPECT_EQ(deletion.keysDeleted, 4u);
  EXPECT_EQ(deletion.linksLeft, 0u);
  EXPECT_FALSE(deletion.boundReached);
  EXPECT_FALSE(KeyExists(root_, L"tree"));
  EXPECT_TRUE(KeyExists(root_, L"keep"));
  // Nothing there is not an error; and never the root itself.
  bool deleted = true;
  EXPECT_EQ(DeleteRegistryTree(root_, L"tree", &deleted), S_OK);
  EXPECT_FALSE(deleted);
  EXPECT_EQ(DeleteRegistryTree(root_, L""), E_INVALIDARG);
  EXPECT_TRUE(KeyExists(root_, L"keep"));
}

TEST_F(RegistryTree, ALinkInTheTreeStaysAndSoDoesItsTarget) {
  SetDword(root_, L"target", L"rule", 7);
  Make(L"target\\inner");
  Make(L"tree\\a");
  wil::unique_hkey target = OpenItself(root_, L"target");
  wil::unique_hkey tree = CreateOpen(root_, L"tree");
  ScopedLink link(tree.get(), L"link", KernelKeyName(target.get()));
  ASSERT_EQ(link.status(), ERROR_SUCCESS);
  ASSERT_EQ(GetDword(root_, L"tree\\link", L"rule"), 7u);  // the link leads there

  RegistryTreeDeletion deletion;
  EXPECT_EQ(DeleteRegistryTree(root_, L"tree", RegistryTreeLimits{}, &deletion),
            kRegistryTreeLeftHr);
  EXPECT_TRUE(deletion.existed);
  EXPECT_FALSE(deletion.deleted);
  EXPECT_EQ(deletion.linksLeft, 1u);
  EXPECT_FALSE(deletion.boundReached);
  EXPECT_EQ(deletion.keysDeleted, 1u);  // tree\a
  EXPECT_FALSE(KeyExists(root_, L"tree\\a"));
  EXPECT_TRUE(KeyExists(root_, L"tree"));  // above the link
  EXPECT_EQ(GetDword(root_, L"target", L"rule"), 7u);
  EXPECT_TRUE(KeyExists(root_, L"target\\inner"));
}

TEST_F(RegistryTree, ALinkAtTheTopStaysAndSoDoesItsTarget) {
  SetDword(root_, L"target", L"rule", 7);
  Make(L"target\\inner");
  wil::unique_hkey target = OpenItself(root_, L"target");
  ScopedLink link(root_, L"top", KernelKeyName(target.get()));
  ASSERT_EQ(link.status(), ERROR_SUCCESS);
  ASSERT_EQ(GetDword(root_, L"top", L"rule"), 7u);

  RegistryTreeDeletion deletion;
  EXPECT_EQ(DeleteRegistryTree(root_, L"top", RegistryTreeLimits{}, &deletion),
            kRegistryTreeLeftHr);
  EXPECT_TRUE(deletion.existed);
  EXPECT_FALSE(deletion.deleted);
  EXPECT_EQ(deletion.linksLeft, 1u);
  EXPECT_EQ(deletion.keysDeleted, 0u);
  bool deleted = true;
  EXPECT_EQ(DeleteRegistryTree(root_, L"top", &deleted), kRegistryTreeLeftHr);
  EXPECT_FALSE(deleted);
  EXPECT_EQ(GetDword(root_, L"target", L"rule"), 7u);
  EXPECT_TRUE(KeyExists(root_, L"target\\inner"));
}

TEST_F(RegistryTree, AnEmptyKeyIsNeverDeletedThroughALink) {
  wil::unique_hkey target = CreateOpen(root_, L"empty");
  ScopedLink link(root_, L"link", KernelKeyName(target.get()));
  ASSERT_EQ(link.status(), ERROR_SUCCESS);
  bool deleted = true;
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryKeyIfEmpty(root_, L"link", &deleted));
  EXPECT_FALSE(deleted);
  EXPECT_TRUE(KeyExists(root_, L"empty"));
  // The empty key itself goes.
  ASSERT_HRESULT_SUCCEEDED(DeleteRegistryKeyIfEmpty(root_, L"empty", &deleted));
  EXPECT_TRUE(deleted);
  EXPECT_FALSE(KeyExists(root_, L"empty"));
}

// A key this process may read but not delete (its permissions are someone else's choice): one
// with a value in it would stay anyway, so that is no failure; an empty one is refused, as
// deleting it would be.
TEST_F(RegistryTree, AKeyThatMayNotBeDeletedFailsOnlyWhenEmpty) {
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  wil::unique_hlocal_security_descriptor readOnly;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      (L"D:P(A;;KR;;;" + sid + L")").c_str(), SDDL_REVISION_1, readOnly.put(), nullptr))
      << GetLastError();
  wil::unique_hlocal_security_descriptor full;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      (L"D:P(A;;KA;;;" + sid + L")(A;;KA;;;BA)").c_str(), SDDL_REVISION_1, full.put(), nullptr))
      << GetLastError();
  for (const bool empty : {false, true}) {
    const std::wstring name = empty ? L"empty" : L"full";
    if (!empty) Make(name);
    wil::unique_hkey key = CreateOpen(root_, name);
    ASSERT_TRUE(key);
    ASSERT_EQ(RegSetKeySecurity(key.get(), DACL_SECURITY_INFORMATION, readOnly.get()),
              ERROR_SUCCESS);
    // The handle opened before keeps its rights: the key gets them back, so the sandbox can go.
    const auto restore = wil::scope_exit(
        [&] { RegSetKeySecurity(key.get(), DACL_SECURITY_INFORMATION, full.get()); });
    bool deleted = true;
    EXPECT_EQ(DeleteRegistryKeyIfEmpty(root_, name, &deleted),
              empty ? HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) : S_OK)
        << empty;
    EXPECT_FALSE(deleted) << empty;
    EXPECT_TRUE(KeyExists(root_, name)) << empty;
  }
}

TEST_F(RegistryTree, ADeepTreeStopsAtTheBound) {
  RegistryTreeLimits limits;
  limits.maxDepth = 3;
  Make(L"tree\\1\\2\\3");
  RegistryTreeDeletion deletion;
  ASSERT_EQ(DeleteRegistryTree(root_, L"tree", limits, &deletion), S_OK);  // 3 levels below
  EXPECT_EQ(deletion.keysDeleted, 4u);
  Make(L"tree\\1\\2\\3\\4");
  EXPECT_EQ(DeleteRegistryTree(root_, L"tree", limits, &deletion), kRegistryTreeLeftHr);
  EXPECT_TRUE(deletion.boundReached);
  EXPECT_FALSE(deletion.deleted);
  EXPECT_EQ(deletion.keysDeleted, 0u);
  EXPECT_TRUE(KeyExists(root_, L"tree\\1\\2\\3\\4"));
  // The default bound is the same rule, a few levels further down.
  ASSERT_EQ(DeleteRegistryTree(root_, L"tree", RegistryTreeLimits{}, &deletion), S_OK);
  std::wstring deep = L"tree";
  for (DWORD level = 0; level <= RegistryTreeLimits{}.maxDepth; ++level) deep += L"\\k";
  Make(deep);
  EXPECT_EQ(DeleteRegistryTree(root_, L"tree", RegistryTreeLimits{}, &deletion),
            kRegistryTreeLeftHr);
  EXPECT_TRUE(deletion.boundReached);
  EXPECT_TRUE(KeyExists(root_, deep));
}

TEST_F(RegistryTree, AWideTreeStopsAtTheBound) {
  RegistryTreeLimits limits;
  limits.maxKeys = 5;  // the key itself and four below it
  for (int i = 0; i < 10; ++i) Make(L"tree\\" + std::to_wstring(i));
  RegistryTreeDeletion deletion;
  EXPECT_EQ(DeleteRegistryTree(root_, L"tree", limits, &deletion), kRegistryTreeLeftHr);
  EXPECT_TRUE(deletion.boundReached);
  EXPECT_EQ(deletion.keysDeleted, 4u);
  EXPECT_TRUE(KeyExists(root_, L"tree"));
  // Each call has bounds of its own: later calls delete the rest.
  EXPECT_EQ(DeleteRegistryTree(root_, L"tree", limits, &deletion), kRegistryTreeLeftHr);
  EXPECT_EQ(DeleteRegistryTree(root_, L"tree", limits, &deletion), S_OK);
  EXPECT_EQ(deletion.keysDeleted, 3u);
  EXPECT_FALSE(KeyExists(root_, L"tree"));
}

TEST_F(RegistryTree, ATreeStopsWhenItsTimeIsUp) {
  RegistryTreeLimits limits;
  limits.maxMs = 0;
  Make(L"tree\\a");
  RegistryTreeDeletion deletion;
  EXPECT_EQ(DeleteRegistryTree(root_, L"tree", limits, &deletion), kRegistryTreeLeftHr);
  EXPECT_TRUE(deletion.boundReached);
  EXPECT_EQ(deletion.keysDeleted, 0u);
  EXPECT_TRUE(KeyExists(root_, L"tree\\a"));
}

TEST_F(RegistryTree, AnOpenKeyIsDeletedItselfAndNeverAnotherOne) {
  // The handle's own key: not its parent, not a sibling.
  Make(L"parent\\child");
  Make(L"parent\\sibling");
  wil::unique_hkey child = OpenItself(root_, L"parent\\child");
  ASSERT_EQ(DeleteOpenRegistryKey(child.get()), S_OK);
  EXPECT_FALSE(KeyExists(root_, L"parent\\child"));
  EXPECT_TRUE(KeyExists(root_, L"parent\\sibling"));
  // A key made at the same path since is another key: it stays.
  wil::unique_hkey old = OpenItself(root_, L"parent\\sibling");
  ASSERT_EQ(RegDeleteKeyW(root_, L"parent\\sibling"), ERROR_SUCCESS);
  Make(L"parent\\sibling");
  EXPECT_EQ(DeleteOpenRegistryKey(old.get()), S_OK);  // gone already
  EXPECT_TRUE(KeyExists(root_, L"parent\\sibling"));
  // Never while it has subkeys.
  wil::unique_hkey parent = OpenItself(root_, L"parent");
  EXPECT_HRESULT_FAILED(DeleteOpenRegistryKey(parent.get()));
  EXPECT_TRUE(KeyExists(root_, L"parent\\sibling"));
  // A link opened as itself: the link goes, the key it leads to stays.
  SetDword(root_, L"target", L"rule", 7);
  wil::unique_hkey target = OpenItself(root_, L"target");
  ScopedLink link(root_, L"link", KernelKeyName(target.get()));
  ASSERT_EQ(link.status(), ERROR_SUCCESS);
  wil::unique_hkey itself = OpenItself(root_, L"link");
  ASSERT_EQ(DeleteOpenRegistryKey(itself.get()), S_OK);
  EXPECT_EQ(GetDword(root_, L"target", L"rule"), 7u);
  wil::unique_hkey gone;
  EXPECT_EQ(RegOpenKeyExW(root_, L"link", REG_OPTION_OPEN_LINK, KEY_READ, gone.put()),
            ERROR_FILE_NOT_FOUND);
}

// What RegDeleteKeyExW does with an empty name on an open handle, which DeleteOpenRegistryKey
// does not use: on an ordinary key it deletes that key. On a link opened as itself it opens the
// key again by that empty name; which key then goes is printed, not judged.
TEST_F(RegistryTree, RegDeleteKeyExWithAnEmptyName) {
  Make(L"parent\\child");
  wil::unique_hkey child = OpenItself(root_, L"parent\\child");
  ASSERT_EQ(RegDeleteKeyExW(child.get(), L"", KEY_WOW64_64KEY, 0), ERROR_SUCCESS);
  EXPECT_FALSE(KeyExists(root_, L"parent\\child"));
  EXPECT_TRUE(KeyExists(root_, L"parent"));

  SetDword(root_, L"target", L"rule", 7);
  wil::unique_hkey target = OpenItself(root_, L"target");
  ScopedLink link(root_, L"link", KernelKeyName(target.get()));
  ASSERT_EQ(link.status(), ERROR_SUCCESS);
  wil::unique_hkey itself = OpenItself(root_, L"link");
  const LSTATUS status = RegDeleteKeyExW(itself.get(), L"", KEY_WOW64_64KEY, 0);
  wil::unique_hkey linkLeft;
  const bool linkGone = RegOpenKeyExW(root_, L"link", REG_OPTION_OPEN_LINK, KEY_READ,
                                      linkLeft.put()) == ERROR_FILE_NOT_FOUND;
  const bool targetGone = !GetDword(root_, L"target", L"rule");
  std::printf(
      "[ registry ] RegDeleteKeyExW(link opened as itself, \"\") = %ld: link %s, target %s\n",
      static_cast<long>(status), linkGone ? "deleted" : "kept", targetGone ? "deleted" : "kept");
}

}  // namespace
}  // namespace pdft
