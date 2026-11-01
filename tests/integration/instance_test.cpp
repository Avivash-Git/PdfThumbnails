// Named objects across processes (instance.h). The other process is this test program again
// (main.cpp, "--pdft-child hold-lock"). Every name carries this run's suffix (main.cpp), so
// none of these tests touches the objects of a running app.

#include <windows.h>

#include "instance.h"

#include <aclapi.h>
#include <gtest/gtest.h>
#include <objbase.h>
#include <sddl.h>
#include <wil/resource.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>

#include "child_process.h"
#include "medium_il.h"
#include "project_config.h"

namespace pdft {
namespace {

std::wstring Sid() {
  std::wstring sid;
  EXPECT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  return sid;
}

// A new GUID as text ("{...}").
std::wstring NewGuidText() {
  GUID guid = {};
  EXPECT_HRESULT_SUCCEEDED(CoCreateGuid(&guid));
  wchar_t text[40] = {};
  StringFromGUID2(guid, text, ARRAYSIZE(text));
  return text;
}

// A name of this test's own: Local\<prefix>.Test.<new GUID>.
std::wstring TestMutexName() {
  return std::wstring(L"Local\\") + kObjectPrefix + L".Test." + NewGuidText();
}

// Makes the mutex |name| with the descriptor |sddl|, as another instance would; *mutex keeps it
// in existence. Use it with ASSERT_NO_FATAL_FAILURE.
void MakeMutex(const std::wstring& name, const std::wstring& sddl, wil::unique_handle* mutex) {
  PSECURITY_DESCRIPTOR raw = nullptr;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                   &raw, nullptr))
      << GetLastError();
  wil::unique_hlocal freeDescriptor(raw);
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), raw, FALSE};
  mutex->reset(CreateMutexW(&attributes, FALSE, name.c_str()));
  ASSERT_TRUE(*mutex) << GetLastError();
}

// Everything denied to the user |sid|, nothing allowed to anyone: not even Administrators may
// open it, so this holds in an elevated test run too.
std::wstring DenyingSddl(const std::wstring& sid) { return L"D:(D;;GA;;;" + sid + L")"; }

// The child's arguments: |mode| and this run's suffix for the named objects.
std::wstring ChildArguments(const wchar_t* mode) {
  return std::wstring(L"--pdft-child ") + mode + L" " + test::NamedObjectSuffix();
}

TEST(OperationLock, AnotherProcessHoldingItMakesAcquireBusy) {
  test::PipedChild child;
  ASSERT_HRESULT_SUCCEEDED(child.Start(test::SelfPath(), ChildArguments(L"hold-lock")));
  ASSERT_EQ(child.ReadLine(), "locked");

  std::optional<OperationLock> lock;
  EXPECT_EQ(OperationLock::Acquire(200, &lock), OperationLock::kBusy);
  EXPECT_FALSE(lock);

  child.CloseInput();  // the child releases the lock and ends
  EXPECT_EQ(child.Wait(30000), 0u);
  ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &lock));
  ASSERT_TRUE(lock);
  EXPECT_FALSE(lock->abandoned());
}

TEST(OperationLock, AHolderThatEndsWithoutReleasingCountsAsGone) {
  test::PipedChild child;
  ASSERT_HRESULT_SUCCEEDED(child.Start(test::SelfPath(), ChildArguments(L"hold-lock-and-exit")));
  ASSERT_EQ(child.ReadLine(), "locked");
  // A handle of our own keeps the mutex in existence (abandoned) after the child is gone.
  wil::unique_handle keep(OpenMutexW(SYNCHRONIZE, FALSE, OperationLockName(Sid()).c_str()));
  ASSERT_TRUE(keep) << GetLastError();
  child.CloseInput();  // the child ends without releasing the lock
  EXPECT_EQ(child.Wait(30000), 0u);
  std::optional<OperationLock> lock;
  ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &lock));
  ASSERT_TRUE(lock);
  EXPECT_TRUE(lock->abandoned());
}

// A primary token like this process's, labelled Low integrity.
HRESULT CreateLowToken(wil::unique_handle* token) {
  HANDLE medium = nullptr;
  HRESULT hr = test::CreateMediumTokenForSelf(test::MediumToken::kLabelOnly, &medium);
  if (FAILED(hr)) return hr;
  wil::unique_handle owned(medium);
  BYTE low[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(low);
  if (!CreateWellKnownSid(WinLowLabelSid, nullptr, low, &size)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  TOKEN_MANDATORY_LABEL label = {};
  label.Label.Attributes = SE_GROUP_INTEGRITY;
  label.Label.Sid = low;
  if (!SetTokenInformation(owned.get(), TokenIntegrityLevel, &label,
                           sizeof(label) + GetLengthSid(low))) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  *token = std::move(owned);
  return S_OK;
}

// Restores the run's own name suffix when it goes away.
class SuffixScope {
 public:
  explicit SuffixScope(const std::wstring& suffix) { SetObjectNameSuffixForTesting(suffix); }
  ~SuffixScope() { SetObjectNameSuffixForTesting(test::NamedObjectSuffix()); }
};

// A process of the same user below Medium integrity (a sandboxed part of a browser, say) must
// not be able to keep checks out: it cannot open the lock to wait on it (the label denies a
// lower process SYNCHRONIZE, which a mutex counts as execute access), and if it creates the
// lock's name before any of the app's processes, Acquire refuses to wait on that object instead
// of waiting for whoever made it.
TEST(OperationLock, ProcessesBelowMediumCannotHoldIt) {
  if (test::CurrentIntegrityRid() < SECURITY_MANDATORY_MEDIUM_RID) {
    GTEST_SKIP() << "needs a test process at Medium integrity or above";
  }
  const std::wstring sid = Sid();
  std::optional<OperationLock> held;
  ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &held));
  const std::wstring heldName = OperationLockName(sid);
  const std::wstring freshSuffix = test::NamedObjectSuffix() + L".squat";
  std::wstring freshName;
  {
    SuffixScope scope(freshSuffix);
    freshName = OperationLockName(sid);
  }

  wil::unique_handle low;
  ASSERT_HRESULT_SUCCEEDED(CreateLowToken(std::addressof(low)));
  test::PipedChild child;
  const HRESULT started = child.Start(test::SelfPath(),
                                      L"--pdft-child squat-lock " + test::QuoteArgument(heldName) +
                                          L" " + test::QuoteArgument(freshName),
                                      low.get());
  if (test::IsApplicationControlBlock(started)) {
    GTEST_SKIP() << "Application Control blocked the test program at Low integrity";
  }
  ASSERT_HRESULT_SUCCEEDED(started);
  EXPECT_EQ(child.ReadLine(), "il=0x1000");
  const std::string open = child.ReadLine();
  EXPECT_EQ(open.rfind("open=no", 0), 0u) << open;
  const std::string create = child.ReadLine();
  std::printf("[ low integrity ] %s, %s\n", open.c_str(), create.c_str());
  {
    SuffixScope scope(freshSuffix);
    std::optional<OperationLock> lock;
    const ULONGLONG before = GetTickCount64();
    const HRESULT hr = OperationLock::Acquire(5000, &lock);
    if (create == "create=yes") {
      // The name is the other process's object: refused at once, never waited on.
      EXPECT_EQ(hr, OperationLock::kTakenByLowerIntegrity);
      EXPECT_LT(GetTickCount64() - before, 2000u);
      EXPECT_FALSE(lock);
    } else {
      EXPECT_EQ(create.rfind("create=no", 0), 0u) << create;
      EXPECT_HRESULT_SUCCEEDED(hr);
      EXPECT_TRUE(lock);
    }
  }
  child.CloseInput();
  EXPECT_EQ(child.Wait(30000), 0u);
}

// The lock's name made by someone else with a descriptor that refuses this user even waiting on
// it: busy at once, as before, and never waited on.
TEST(OperationLock, ANameThatRefusesThisUserIsBusyAtOnce) {
  const std::wstring sid = Sid();
  SuffixScope scope(test::NamedObjectSuffix() + L".denied");
  wil::unique_handle other;
  ASSERT_NO_FATAL_FAILURE(
      MakeMutex(OperationLockName(sid), DenyingSddl(sid), std::addressof(other)));
  std::optional<OperationLock> lock;
  const ULONGLONG before = GetTickCount64();
  EXPECT_EQ(OperationLock::Acquire(5000, &lock), OperationLock::kBusy);
  EXPECT_LT(GetTickCount64() - before, 2000u);
  EXPECT_FALSE(lock);
}

TEST(OperationLock, LivesInTheGlobalNamespace) {
  std::optional<OperationLock> lock;
  ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &lock));
  const std::wstring name = OperationLockName(Sid());
  ASSERT_EQ(name.rfind(L"Global\\", 0), 0u);
  wil::unique_handle opened(OpenMutexW(SYNCHRONIZE, FALSE, name.c_str()));
  EXPECT_TRUE(opened) << GetLastError();
}

TEST(NamedObjects, SingleInstanceSeesTheFirst) {
  const std::wstring sid = Sid();
  wil::unique_handle first;
  bool isFirst = false;
  ASSERT_HRESULT_SUCCEEDED(TakeSingleInstance(sid, std::addressof(first), &isFirst));
  EXPECT_TRUE(isFirst);
  wil::unique_handle second;
  bool secondIsFirst = true;
  ASSERT_HRESULT_SUCCEEDED(TakeSingleInstance(sid, std::addressof(second), &secondIsFirst));
  EXPECT_FALSE(secondIsFirst);
}

// Another instance made the mutex with a descriptor that does not give this process full access
// (here it may only wait on it, so the fallback open can be checked as well): opening it fully
// is refused, and that refusal still means "it exists".
TEST(NamedObjects, AccessDeniedMeansHeldByAnother) {
  const std::wstring sid = Sid();
  const std::wstring name = TestMutexName();
  // SYNCHRONIZE for this user only. Administrators are not named, so this holds elevated too,
  // and the creator's implicit owner rights do not include MUTEX_ALL_ACCESS.
  PSECURITY_DESCRIPTOR raw = nullptr;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      (L"D:(A;;0x00100000;;;" + sid + L")").c_str(), SDDL_REVISION_1, &raw, nullptr))
      << GetLastError();
  wil::unique_hlocal freeDescriptor(raw);
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), raw, FALSE};
  wil::unique_handle other(CreateMutexW(&attributes, FALSE, name.c_str()));
  ASSERT_TRUE(other) << GetLastError();

  // With a fallback access, the handle that can only wait keeps the mutex alive.
  wil::unique_handle waitOnly;
  bool existed = false;
  ASSERT_HRESULT_SUCCEEDED(
      CreateNamedMutex(name, sid, false, SYNCHRONIZE, std::addressof(waitOnly), &existed));
  EXPECT_TRUE(existed);
  EXPECT_TRUE(waitOnly);

  // Without one there is no handle: what OperationLock::Acquire reports as busy.
  wil::unique_handle none;
  existed = false;
  ASSERT_HRESULT_SUCCEEDED(CreateNamedMutex(name, sid, false, 0, std::addressof(none), &existed));
  EXPECT_TRUE(existed);
  EXPECT_FALSE(none);
}

// Another instance made the mutex with a descriptor that refuses this user even waiting on it:
// opening it is refused too, but a probe in its namespace shows that this process may create
// objects there, so the mutex exists and someone else has it, with a fallback access or without.
TEST(NamedObjects, AMutexThatRefusesEvenWaitingIsHeldByAnother) {
  const std::wstring sid = Sid();
  const std::wstring name = TestMutexName();
  wil::unique_handle other;
  ASSERT_NO_FATAL_FAILURE(MakeMutex(name, DenyingSddl(sid), std::addressof(other)));
  for (const DWORD fallback : {DWORD{SYNCHRONIZE}, DWORD{0}}) {
    wil::unique_handle mutex;
    bool existed = false;
    EXPECT_HRESULT_SUCCEEDED(
        CreateNamedMutex(name, sid, false, fallback, std::addressof(mutex), &existed))
        << fallback;
    EXPECT_TRUE(existed) << fallback;
    EXPECT_FALSE(mutex) << fallback;
  }
}

// A private namespace of this test's own that lets this user look names up in it but not create
// anything there, which is what a session's named objects are to a program that another account
// runs in that session: Windows refuses the creation and the name is not there, so nobody has
// the mutex and nothing would ever let go of it. Not "another instance has it".
//
// This covers a refused creation, a name not found and a refused probe. When kernel32 cannot
// open the session's directory at all, opening the name is refused too; no test here can make
// that happen without changing the session's own permissions, so only the classification's
// table (unit tests) and the system tests' runs cover it. The tests of a mutex whose descriptor
// refuses this user (the two before this one) pass with the earlier "refused means busy" code
// too: they guard that answer.
TEST(NamedObjects, ANamespaceThatRefusesCreatingIsNotHeldByAnother) {
  const std::wstring sid = Sid();
  PSID user = nullptr;
  ASSERT_TRUE(ConvertStringSidToSidW(sid.c_str(), &user)) << GetLastError();
  wil::unique_hlocal freeUser(user);
  // The boundary: a name of this run's own, with this user in it (its creator must be).
  const std::wstring unique = L"pdft-test-" + NewGuidText();
  HANDLE boundary = CreateBoundaryDescriptorW(unique.c_str(), 0);
  ASSERT_NE(boundary, nullptr) << GetLastError();
  const auto deleteBoundary = wil::scope_exit([&boundary] { DeleteBoundaryDescriptor(boundary); });
  ASSERT_TRUE(AddSIDToBoundaryDescriptor(&boundary, user)) << GetLastError();
  // DIRECTORY_QUERY | DIRECTORY_TRAVERSE for this user, nothing for anyone else.
  PSECURITY_DESCRIPTOR raw = nullptr;
  ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
      (L"D:(A;;0x3;;;" + sid + L")").c_str(), SDDL_REVISION_1, &raw, nullptr))
      << GetLastError();
  wil::unique_hlocal freeDescriptor(raw);
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), raw, FALSE};
  HANDLE closed = CreatePrivateNamespaceW(&attributes, boundary, unique.c_str());
  ASSERT_NE(closed, nullptr) << GetLastError();
  const auto destroy =
      wil::scope_exit([closed] { ClosePrivateNamespace(closed, PRIVATE_NAMESPACE_FLAG_DESTROY); });

  const std::wstring name = unique + L"\\" + kObjectPrefix + L".Test";
  for (const DWORD fallback : {DWORD{SYNCHRONIZE}, DWORD{0}}) {
    wil::unique_handle mutex;
    bool existed = true;
    EXPECT_EQ(CreateNamedMutex(name, sid, false, fallback, std::addressof(mutex), &existed),
              kNamedObjectsRefusedHr)
        << fallback;
    EXPECT_FALSE(existed) << fallback;
    EXPECT_FALSE(mutex) << fallback;
  }
}

TEST(NamedObjects, DescriptorGrantsTheUserAndCarriesAMediumLabel) {
  const std::wstring sid = Sid();
  const std::wstring name = TestMutexName();
  wil::unique_handle mutex;
  bool existed = true;
  ASSERT_HRESULT_SUCCEEDED(CreateNamedMutex(name, sid, false, 0, std::addressof(mutex), &existed));
  ASSERT_TRUE(mutex);
  EXPECT_FALSE(existed);

  PSID owner = nullptr;
  PACL dacl = nullptr;
  PACL sacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  ASSERT_EQ(GetSecurityInfo(mutex.get(), SE_KERNEL_OBJECT,
                            DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, &owner, nullptr,
                            &dacl, &sacl, &descriptor),
            static_cast<DWORD>(ERROR_SUCCESS));
  wil::unique_hlocal freeDescriptor(descriptor);
  PSID user = nullptr;
  ASSERT_TRUE(ConvertStringSidToSidW(sid.c_str(), &user));
  wil::unique_hlocal freeUser(user);

  // The user has every mutex right (GA maps to MUTEX_ALL_ACCESS).
  ACCESS_MASK userRights = 0;
  for (DWORD i = 0; dacl && i < dacl->AceCount; ++i) {
    ACE_HEADER* ace = nullptr;
    if (GetAce(dacl, i, reinterpret_cast<void**>(&ace)) &&
        ace->AceType == ACCESS_ALLOWED_ACE_TYPE &&
        EqualSid(&reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart, user)) {
      userRights |= reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace)->Mask;
    }
  }
  EXPECT_EQ(userRights & MUTEX_ALL_ACCESS, static_cast<ACCESS_MASK>(MUTEX_ALL_ACCESS));

  // Labelled Medium, no-write-up, no-read-up and no-execute-up: a Medium and a High process of
  // the user can both open it, a lower one cannot open it at all.
  bool medium = false;
  for (DWORD i = 0; sacl && i < sacl->AceCount; ++i) {
    ACE_HEADER* ace = nullptr;
    if (GetAce(sacl, i, reinterpret_cast<void**>(&ace)) &&
        ace->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
      auto* label = reinterpret_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
      PSID labelSid = &label->SidStart;
      constexpr ACCESS_MASK kAll = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP |
                                   SYSTEM_MANDATORY_LABEL_NO_READ_UP |
                                   SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP;
      medium = *GetSidSubAuthority(labelSid, 0) == SECURITY_MANDATORY_MEDIUM_RID &&
               (label->Mask & kAll) == kAll;
    }
  }
  EXPECT_TRUE(medium);
}

TEST(NamedObjects, InstallerMutexesAreVisible) {
  AppMutexes mutexes;
  ASSERT_HRESULT_SUCCEEDED(mutexes.Hold(Sid()));
  for (bool global : {false, true}) {
    wil::unique_handle opened(OpenMutexW(SYNCHRONIZE, FALSE, AppMutexName(global).c_str()));
    EXPECT_TRUE(opened) << global << " " << GetLastError();
  }
}

}  // namespace
}  // namespace pdft
