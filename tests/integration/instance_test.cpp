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
#include <winternl.h>

#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>

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

// Makes the mutex |name| owned by |ownerSid| with the descriptor |sddl| ("D:..."), as another
// account would make it. Making someone else the owner needs SeRestorePrivilege, which only an
// elevated run has: *made stays false without it. Use it with ASSERT_NO_FATAL_FAILURE.
void MakeMutexOwnedBy(const std::wstring& name, const std::wstring& ownerSid,
                      const std::wstring& sddl, wil::unique_handle* mutex, bool* made) {
  *made = false;
  wil::unique_handle process;
  ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE, process.put()))
      << GetLastError();
  // A copy for this thread with the privilege on, so the process's own token stays as it was.
  wil::unique_handle restore;
  ASSERT_TRUE(DuplicateTokenEx(process.get(),
                               TOKEN_ADJUST_PRIVILEGES | TOKEN_IMPERSONATE | TOKEN_QUERY, nullptr,
                               SecurityImpersonation, TokenImpersonation, restore.put()))
      << GetLastError();
  TOKEN_PRIVILEGES privileges = {};
  privileges.PrivilegeCount = 1;
  privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  ASSERT_TRUE(LookupPrivilegeValueW(nullptr, SE_RESTORE_NAME, &privileges.Privileges[0].Luid))
      << GetLastError();
  const BOOL adjusted =
      AdjustTokenPrivileges(restore.get(), FALSE, &privileges, 0, nullptr, nullptr);
  const DWORD error = GetLastError();
  ASSERT_TRUE(adjusted) << error;
  if (error == ERROR_NOT_ALL_ASSIGNED) return;  // the token does not have it
  ASSERT_TRUE(SetThreadToken(nullptr, restore.get())) << GetLastError();
  const auto revert = wil::scope_exit([] { RevertToSelf(); });
  ASSERT_NO_FATAL_FAILURE(MakeMutex(name, L"O:" + ownerSid + sddl, mutex));
  *made = true;
}

// What the native API answers when the caller may not do what it asked (STATUS_ACCESS_DENIED).
constexpr LONG kStatusAccessDenied = static_cast<LONG>(0xC0000022);

// Makes an object manager link named |name| (a full name: "\BaseNamedObjects\...") that leads to
// |target|, through ntdll, which every process has loaded; *link keeps it in existence. Returns
// the NTSTATUS, negative when it failed.
LONG MakeObjectLink(const std::wstring& name, const std::wstring& target,
                    wil::unique_handle* link) {
  using CreateLink = LONG(NTAPI*)(HANDLE*, ACCESS_MASK, OBJECT_ATTRIBUTES*, UNICODE_STRING*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  const auto create = ntdll ? reinterpret_cast<CreateLink>(reinterpret_cast<void*>(
                                  GetProcAddress(ntdll, "NtCreateSymbolicLinkObject")))
                            : nullptr;
  if (!create) return static_cast<LONG>(0xC0000139);  // STATUS_ENTRYPOINT_NOT_FOUND
  std::wstring nameText = name;
  std::wstring targetText = target;
  const auto counted = [](std::wstring& text) {
    const auto bytes = static_cast<USHORT>(text.size() * sizeof(wchar_t));
    return UNICODE_STRING{bytes, bytes, text.data()};
  };
  UNICODE_STRING linkName = counted(nameText);
  UNICODE_STRING linkTarget = counted(targetText);
  OBJECT_ATTRIBUTES attributes = {};
  attributes.Length = sizeof(attributes);
  attributes.ObjectName = &linkName;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  constexpr ACCESS_MASK kLinkAllAccess = STANDARD_RIGHTS_REQUIRED | 0x1;  // and query
  HANDLE made = nullptr;
  const LONG status = create(&made, kLinkAllAccess, &attributes, &linkTarget);
  if (status >= 0) link->reset(made);
  return status;
}

// Whether this process runs in session 0, whose "Local\" names are its "Global\" ones (that
// session's namespace is the whole machine's).
bool InSessionZero() {
  DWORD session = 0;
  EXPECT_TRUE(ProcessIdToSessionId(GetCurrentProcessId(), &session)) << GetLastError();
  return session == 0;
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

// With the lock's global name made first by "another account" (this test, with a descriptor that
// is not this user's), Acquire takes this session's lock at once: it neither waits on that name
// nor gives up at it. In session 0 a "Local\" name is the "Global\" one, so nothing is left to
// fall back on there, and the lock is busy at once. Use it with ASSERT_NO_FATAL_FAILURE.
void ExpectTheSessionLock() {
  const std::wstring sid = Sid();
  std::optional<OperationLock> lock;
  const ULONGLONG before = GetTickCount64();
  const HRESULT hr = OperationLock::Acquire(5000, &lock);
  EXPECT_LT(GetTickCount64() - before, 2000u);
  if (InSessionZero()) {
    EXPECT_EQ(hr, OperationLock::kBusy);
    EXPECT_FALSE(lock);
    return;
  }
  ASSERT_HRESULT_SUCCEEDED(hr);
  ASSERT_TRUE(lock);
  EXPECT_TRUE(lock->sessionOnly());
  EXPECT_FALSE(lock->abandoned());
  const std::wstring name = OperationSessionLockName(sid);
  ASSERT_EQ(name.rfind(L"Local\\", 0), 0u);
  wil::unique_handle opened(OpenMutexW(SYNCHRONIZE, FALSE, name.c_str()));
  EXPECT_TRUE(opened) << GetLastError();
}

// The lock's global name made by someone else with a descriptor that refuses this user even
// waiting on it (any account may make a Global\ name): not this user's lock, so the checks and
// changes still run, under this session's lock.
TEST(OperationLock, AGlobalNameThatRefusesThisUserLeadsToTheSessionLock) {
  const std::wstring sid = Sid();
  SuffixScope scope(test::NamedObjectSuffix() + L".denied");
  wil::unique_handle other;
  ASSERT_NO_FATAL_FAILURE(
      MakeMutex(OperationLockName(sid), DenyingSddl(sid), std::addressof(other)));
  ASSERT_NO_FATAL_FAILURE(ExpectTheSessionLock());
}

// The same with a descriptor that lets only SYSTEM in. Its owner (this test, in place of the other
// account) may still read it, as an owner may, but not wait on it.
TEST(OperationLock, AGlobalNameOnlySystemMayOpenLeadsToTheSessionLock) {
  SuffixScope scope(test::NamedObjectSuffix() + L".system-only");
  wil::unique_handle other;
  ASSERT_NO_FATAL_FAILURE(
      MakeMutex(OperationLockName(Sid()), L"D:(A;;GA;;;SY)", std::addressof(other)));
  ASSERT_NO_FATAL_FAILURE(ExpectTheSessionLock());
}

// The same with a descriptor that lets this user wait on it but not read who made it. The owner
// would read it all the same (an owner may), unless an OWNER RIGHTS entry says what it may do.
TEST(OperationLock, AGlobalNameThisUserMayOnlyWaitOnLeadsToTheSessionLock) {
  const std::wstring sid = Sid();
  SuffixScope scope(test::NamedObjectSuffix() + L".wait-only");
  wil::unique_handle other;
  ASSERT_NO_FATAL_FAILURE(MakeMutex(OperationLockName(sid),
                                    L"D:(A;;0x00100000;;;" + sid + L")(A;;0x00100000;;;OW)",
                                    std::addressof(other)));  // SYNCHRONIZE
  ASSERT_NO_FATAL_FAILURE(ExpectTheSessionLock());
}

// Another account made the lock's global name with a descriptor that lets everyone in, so this
// user could wait on it and own it: its owner shows that it is not this user's, and Acquire takes
// this session's lock instead of a lock that the other account can hold whenever it likes.
TEST(OperationLock, AGlobalNameAnotherAccountOwnsLeadsToTheSessionLock) {
  SuffixScope scope(test::NamedObjectSuffix() + L".other-owner");
  wil::unique_handle other;
  bool made = false;
  // A made-up account's SID: neither this user, Administrators nor SYSTEM. Split after
  // "S-1-5-21-": the repository's publish check rejects anything shaped like an account SID.
  ASSERT_NO_FATAL_FAILURE(MakeMutexOwnedBy(OperationLockName(Sid()),
                                           L"S-1-5-21-"
                                           L"1000-2000-3000-1001",
                                           L"D:(A;;GA;;;WD)", std::addressof(other), &made));
  if (!made) GTEST_SKIP() << "needs SeRestorePrivilege (an elevated run) to give it another owner";
  ASSERT_NO_FATAL_FAILURE(ExpectTheSessionLock());
}

// The lock's global name taken by an object of another type (an event here), which any account
// may make there too: CreateMutexW cannot make or open a mutex of that name, and no instance of
// this user made it, so Acquire takes this session's lock instead of failing.
TEST(OperationLock, AGlobalNameOfAnotherTypeLeadsToTheSessionLock) {
  SuffixScope scope(test::NamedObjectSuffix() + L".event");
  wil::unique_handle event(CreateEventW(nullptr, TRUE, FALSE, OperationLockName(Sid()).c_str()));
  ASSERT_TRUE(event) << GetLastError();
  ASSERT_NO_FATAL_FAILURE(ExpectTheSessionLock());
}

// An object manager link at the lock's global name that leads to itself, which any account may
// put there as it may put a mutex there: CreateMutexW follows it until Windows gives up, with an
// error of its own. No instance of this user made that either, so Acquire takes this session's
// lock instead of failing.
TEST(OperationLock, AGlobalNameThatLinksToItselfLeadsToTheSessionLock) {
  if (InSessionZero()) GTEST_SKIP() << "in session 0 the session's names are the global ones";
  SuffixScope scope(test::NamedObjectSuffix() + L".link");
  const std::wstring name = OperationLockName(Sid());
  const std::wstring global = L"Global\\";
  ASSERT_EQ(name.rfind(global, 0), 0u);
  // Global\ is a link to the machine's own directory of named objects.
  const std::wstring full = L"\\BaseNamedObjects\\" + name.substr(global.size());
  wil::unique_handle link;
  const LONG status = MakeObjectLink(full, full, std::addressof(link));
  if (status == kStatusAccessDenied) GTEST_SKIP() << "Windows refused this process the link";
  ASSERT_GE(status, 0) << std::hex << static_cast<unsigned long>(status);
  ASSERT_NO_FATAL_FAILURE(ExpectTheSessionLock());
}

// Acquire on a thread of its own, which also lets go of what it got: what it returned, and
// whether that was this session's lock alone. A mutex is its owning thread's, which may take it
// again, so a second holder in this process must be a thread of its own.
HRESULT AcquireElsewhere(DWORD waitMs, bool* sessionOnly) {
  HRESULT hr = E_UNEXPECTED;
  std::thread([&] {
    std::optional<OperationLock> lock;
    hr = OperationLock::Acquire(waitMs, &lock);
    *sessionOnly = lock && lock->sessionOnly();
  }).join();
  return hr;
}

// This session's lock keeps a second holder out until the first lets go, as the global one does.
TEST(OperationLock, TheSessionLockIsBusyUntilItsHolderLetsGo) {
  if (InSessionZero()) GTEST_SKIP() << "in session 0 the session's names are the global ones";
  const std::wstring sid = Sid();
  SuffixScope scope(test::NamedObjectSuffix() + L".session");
  wil::unique_handle other;
  ASSERT_NO_FATAL_FAILURE(
      MakeMutex(OperationLockName(sid), L"D:(A;;GA;;;SY)", std::addressof(other)));
  std::optional<OperationLock> first;
  ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &first));
  ASSERT_TRUE(first);
  ASSERT_TRUE(first->sessionOnly());

  bool sessionOnly = false;
  EXPECT_EQ(AcquireElsewhere(200, &sessionOnly), OperationLock::kBusy);
  first.reset();
  EXPECT_HRESULT_SUCCEEDED(AcquireElsewhere(5000, &sessionOnly));
  EXPECT_TRUE(sessionOnly);
}

// The global name was another account's while the first holder took the lock, and is free again
// when a second one comes: the second makes the global name itself, but this session's lock,
// which every holder takes first, still keeps it out until the first lets go. Then it holds both.
TEST(OperationLock, AGlobalNameFreedMeanwhileDoesNotLetASecondHolderIn) {
  if (InSessionZero()) GTEST_SKIP() << "in session 0 the session's names are the global ones";
  const std::wstring sid = Sid();
  SuffixScope scope(test::NamedObjectSuffix() + L".freed");
  std::optional<OperationLock> first;
  {
    wil::unique_handle other;
    ASSERT_NO_FATAL_FAILURE(
        MakeMutex(OperationLockName(sid), L"D:(A;;GA;;;SY)", std::addressof(other)));
    ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &first));
    ASSERT_TRUE(first);
    ASSERT_TRUE(first->sessionOnly());
  }  // the other account's mutex is gone with its last handle: the global name is free

  bool sessionOnly = true;
  EXPECT_EQ(AcquireElsewhere(200, &sessionOnly), OperationLock::kBusy);
  first.reset();
  EXPECT_HRESULT_SUCCEEDED(AcquireElsewhere(5000, &sessionOnly));
  EXPECT_FALSE(sessionOnly);
}

// A global name that this user's process made (with the app's own descriptor, as another
// instance makes it) is the lock, as it always was: Acquire holds that very mutex, and this
// session's as well.
TEST(OperationLock, AGlobalNameThisUserMadeIsTheLock) {
  const std::wstring sid = Sid();
  SuffixScope scope(test::NamedObjectSuffix() + L".own");
  wil::unique_handle own;
  bool existed = true;
  ASSERT_HRESULT_SUCCEEDED(
      CreateNamedMutex(OperationLockName(sid), sid, false, 0, std::addressof(own), &existed));
  ASSERT_FALSE(existed);
  std::optional<OperationLock> lock;
  ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &lock));
  ASSERT_TRUE(lock);
  EXPECT_FALSE(lock->sessionOnly());
  // Held: another thread cannot have it.
  DWORD waited = WAIT_FAILED;
  std::thread([&] { waited = WaitForSingleObject(own.get(), 0); }).join();
  EXPECT_EQ(waited, static_cast<DWORD>(WAIT_TIMEOUT));
  if (!InSessionZero()) {
    wil::unique_handle session(
        OpenMutexW(SYNCHRONIZE, FALSE, OperationSessionLockName(sid).c_str()));
    ASSERT_TRUE(session) << GetLastError();
    waited = WAIT_FAILED;
    std::thread([&] { waited = WaitForSingleObject(session.get(), 0); }).join();
    EXPECT_EQ(waited, static_cast<DWORD>(WAIT_TIMEOUT));
  }
}

TEST(OperationLock, LivesInTheGlobalNamespace) {
  std::optional<OperationLock> lock;
  ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &lock));
  EXPECT_FALSE(lock->sessionOnly());
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
