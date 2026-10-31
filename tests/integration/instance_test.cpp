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

#include <memory>
#include <optional>
#include <string>

#include "child_process.h"
#include "project_config.h"

namespace pdft {
namespace {

std::wstring Sid() {
  std::wstring sid;
  EXPECT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  return sid;
}

// A name of this test's own: Local\<prefix>.Test.<new GUID>.
std::wstring TestMutexName() {
  GUID guid = {};
  EXPECT_HRESULT_SUCCEEDED(CoCreateGuid(&guid));
  wchar_t text[40] = {};
  StringFromGUID2(guid, text, ARRAYSIZE(text));
  return std::wstring(L"Local\\") + kObjectPrefix + L".Test." + text;
}

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

  // Labelled Medium, no-write-up: a Medium and a High process of the user can both open it.
  bool medium = false;
  for (DWORD i = 0; sacl && i < sacl->AceCount; ++i) {
    ACE_HEADER* ace = nullptr;
    if (GetAce(sacl, i, reinterpret_cast<void**>(&ace)) &&
        ace->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
      auto* label = reinterpret_cast<SYSTEM_MANDATORY_LABEL_ACE*>(ace);
      PSID labelSid = &label->SidStart;
      medium = *GetSidSubAuthority(labelSid, 0) == SECURITY_MANDATORY_MEDIUM_RID &&
               (label->Mask & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) != 0;
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
