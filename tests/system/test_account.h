// The users the system tests act as: local accounts of their own, made for one run and deleted at
// its end, also after failures.
//
// A filtered copy of the runner's own token is not what a person signed in as an administrator
// has. The CI runner is the built-in Administrator, whose token has no linked token; a filtered
// copy of it still counts as elevated, so runas started the "elevated" step with that copy
// unchanged instead of going through UAC. The test account is an ordinary member of
// Administrators, logged on interactively: with UAC on, that logon gives its limited token
// (elevation type 3) with a linked full token, so its runas goes through UAC (the AppInfo
// service) and, with ConsentPromptBehaviorAdmin=0, elevates into the linked token without a
// prompt.
//
// The standard account is a second one, made and logged on the same way but never added to
// Administrators (NetUserAdd makes it a member of Users only): its token is not elevated and has
// no linked token (elevation type 1), as a person's who signs in to a standard account. The task
// tests run as it, and so does a standard user's own --setup-user (system_test.cpp).
//
// They are made only where WhyNoTestAccount (account_rules.h) allows it, which needs, besides
// PDFT_SYSTEM_TESTS=1, GITHUB_ACTIONS=true with RUNNER_ENVIRONMENT=github-hosted or
// PDFT_SYSTEM_TESTS_THROWAWAY_VM=1: never on a PC someone uses. Each password is made in memory,
// handed to NetUserAdd and LogonUserW only, and wiped once the account is logged on; it is never
// printed or written anywhere. The privileges this needs (backup and restore for a hive, debug
// to end an account's programs) are enabled only while they are used, so the installer's
// commands the tests run with this process's token have them as a real installer has them:
// disabled.

#pragma once

#include <windows.h>

#include <userenv.h>
#include <wil/resource.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "elevation.h"
#include "system_env.h"

namespace pdft::systest {

class TestAccount {
 public:
  // The test account (an ordinary member of Administrators), or the standard account.
  enum class Kind { kAdministrator, kStandard };

  TestAccount(const TestAccount&) = delete;
  TestAccount& operator=(const TestAccount&) = delete;
  // Ends what still runs as the account, takes its entries out of the permissions of the window
  // station, the desktop and this session's named objects again, unloads its profile, logs it off
  // (closes its tokens), deletes the account and its profile, and prints what it did (its name and
  // SID, never the password).
  ~TestAccount();

  // Makes the account where WhyNoTestAccountHere allows it, after removing the test accounts
  // earlier runs could not delete (IsTestAccount, with their profiles; never one this process
  // made and still has): a random name, a random password, for kAdministrator a member of
  // Administrators (the group named by its well-known SID), logged on interactively
  // (LogonUserW), its profile loaded (LoadUserProfileW) and its environment built
  // (CreateEnvironmentBlock); lets it use this process's window station and desktop, and this
  // session's named objects (namedObjects()). nullptr with |error| set when a step failed (never
  // with the password), also when its token is not what |kind| needs; what was made by then is
  // removed again.
  static std::unique_ptr<TestAccount> Create(Kind kind, std::string* error);

  Kind kind() const { return kind_; }
  // How the lines about it name it: "test account" or "standard account".
  const char* noun() const;
  const std::wstring& name() const { return name_; }
  const std::wstring& sid() const { return sid_; }
  // The token of its interactive logon (for a member of Administrators, the limited one).
  HANDLE token() const { return token_.get(); }
  // The facts of that token and of its linked (full) token; a standard account's token has no
  // linked token, and its linked facts are the defaults.
  const TokenFacts& tokenFacts() const { return facts_; }
  const TokenFacts& linkedFacts() const { return linked_; }
  // Its HKEY_CURRENT_USER: the hive LoadUserProfileW loaded (HKEY_USERS\<SID>).
  HKEY hive() const { return static_cast<HKEY>(profile_.hProfile); }
  const std::filesystem::path& profileDir() const { return profileDir_; }
  // <its %LOCALAPPDATA%>\Temp: where the tests put what the account must read.
  const std::filesystem::path& tempDir() const { return tempDir_; }
  // Whether its programs may make named objects as Windows makes them for a program: S_OK, or why
  // not. That needs this session's \Sessions\<id>\BaseNamedObjects (every "Local\" name is made
  // there, and every name, "Global\" ones too, is looked up through it) and \BaseNamedObjects
  // (where "Global\" names are made). The session's own sign-in has the first;
  // CreateProcessWithTokenW grants nothing there, and without it none of the app's named objects
  // can be made, the operation lock included. Create grants it when it is missing, except in
  // session 0, whose directory is the whole machine's; it never changes \BaseNamedObjects.
  HRESULT namedObjects() const { return namedObjects_; }
  // Its hive, SID and the product's per-user install and data folders in its profile.
  UserPlaces Places() const;

  // Runs |exe| with |arguments| (already quoted) as the account, with token(), its
  // environment and its profile folder as the working folder (CreateProcessWithTokenW), standard
  // output and error each into a pipe, standard input from the null device, a hidden console of
  // its own; ended after |timeoutMs| (hr is then HRESULT_FROM_WIN32(ERROR_TIMEOUT)).
  Outcome Run(const std::filesystem::path& exe, const std::wstring& arguments,
              DWORD timeoutMs) const;

 private:
  TestAccount() = default;

  // What Create does after the account exists; the destructor removes what it made.
  HRESULT LogOnAndLoad(std::wstring* password, std::string* error);
  void AllowDesktop();
  void RestoreDesktop() noexcept;
  void AllowNamedObjects();
  void RestoreNamedObjects() noexcept;
  void EndItsProcesses() noexcept;

  Kind kind_ = Kind::kAdministrator;
  std::wstring name_;
  std::wstring sid_;
  std::vector<BYTE> sidBytes_;
  bool created_ = false;
  wil::unique_handle token_;
  TokenFacts facts_;
  TokenFacts linked_;
  PROFILEINFOW profile_ = {};
  bool profileLoaded_ = false;
  wil::unique_environment_block environment_;
  std::filesystem::path profileDir_;
  std::filesystem::path userRoot_;
  std::filesystem::path dataDir_;
  std::filesystem::path tempDir_;
  // This process's window station and desktop, and whether the account's entry was added to
  // their permissions (to take away again).
  wil::unique_hwinsta station_;
  bool stationGranted_ = false;
  wil::unique_hdesk desktop_;
  bool desktopGranted_ = false;
  // This session's named objects directory and the SID whose entry was added to its permissions
  // (the account's or its logon's), to take away again; both empty when nothing was added there.
  wil::unique_handle namedObjectsDirectory_;
  std::vector<BYTE> namedObjectsGrantee_;
  HRESULT namedObjects_ = E_FAIL;
};

// The test account of this run (main.cpp sets it for the time the tests run); nullptr when there
// is none.
TestAccount* CurrentTestAccount() noexcept;
void SetCurrentTestAccount(TestAccount* account) noexcept;

// The standard account of this run (main.cpp sets it for the time the tests run); nullptr when
// there is none (a "[ system ] the standard account" line says why).
TestAccount* CurrentStandardAccount() noexcept;
void SetCurrentStandardAccount(TestAccount* account) noexcept;

// Why the test accounts must not be made in this process: WhyNoTestAccount with this process's
// facts (the environment, its token, the registry check, Leftovers()). Call after COM is set up.
std::optional<std::string> WhyNoTestAccountHere();

}  // namespace pdft::systest
