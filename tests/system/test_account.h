// The user the system tests act as: a local account of their own, made for one run and deleted at
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
// It is made only where WhyNoTestAccount (account_rules.h) allows it, which needs, besides
// PDFT_SYSTEM_TESTS=1, GITHUB_ACTIONS=true with RUNNER_ENVIRONMENT=github-hosted or
// PDFT_SYSTEM_TESTS_THROWAWAY_VM=1: never on a PC someone uses. Its password is made in memory,
// handed to NetUserAdd and LogonUserW only, and wiped once the account is logged on; it is never
// printed or written anywhere. The privileges this needs (backup and restore for its hive, debug
// to end its programs) are enabled only while they are used, so the installer's commands the
// tests run with this process's token have them as a real installer has them: disabled.

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
  TestAccount(const TestAccount&) = delete;
  TestAccount& operator=(const TestAccount&) = delete;
  // Puts the window station's and desktop's permissions back, ends what still runs as the
  // account, unloads its profile, logs it off (closes its tokens), deletes the account and its
  // profile, and prints what it did (its name and SID, never the password).
  ~TestAccount();

  // Makes the account where WhyNoTestAccountHere allows it, after removing the test accounts
  // earlier runs could not delete (IsTestAccount, with their profiles): a random name, a random
  // password, a
  // member of Administrators (the group named by its well-known SID), logged on interactively
  // (LogonUserW), its profile loaded (LoadUserProfileW) and its environment built
  // (CreateEnvironmentBlock); lets it use this process's window station and desktop. nullptr
  // with |error| set when a step failed (never with the password); what was made by then is
  // removed again.
  static std::unique_ptr<TestAccount> Create(std::string* error);

  const std::wstring& name() const { return name_; }
  const std::wstring& sid() const { return sid_; }
  // The token of its interactive logon: the limited one.
  HANDLE token() const { return token_.get(); }
  // The facts of that token and of its linked (full) token.
  const TokenFacts& limitedFacts() const { return limited_; }
  const TokenFacts& linkedFacts() const { return linked_; }
  // Its HKEY_CURRENT_USER: the hive LoadUserProfileW loaded (HKEY_USERS\<SID>).
  HKEY hive() const { return static_cast<HKEY>(profile_.hProfile); }
  const std::filesystem::path& profileDir() const { return profileDir_; }
  // <its %LOCALAPPDATA%>\Temp: where the tests put what the account must read.
  const std::filesystem::path& tempDir() const { return tempDir_; }
  // Its hive, SID and the product's per-user install and data folders in its profile.
  UserPlaces Places() const;

  // Runs |exe| with |arguments| (already quoted) as the account, with its limited token, its
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
  void EndItsProcesses() noexcept;

  std::wstring name_;
  std::wstring sid_;
  std::vector<BYTE> sidBytes_;
  bool created_ = false;
  wil::unique_handle token_;
  TokenFacts limited_;
  TokenFacts linked_;
  PROFILEINFOW profile_ = {};
  bool profileLoaded_ = false;
  wil::unique_environment_block environment_;
  std::filesystem::path profileDir_;
  std::filesystem::path userRoot_;
  std::filesystem::path dataDir_;
  std::filesystem::path tempDir_;
  // This process's window station and desktop with the permissions they had, to put back.
  wil::unique_hwinsta station_;
  wil::unique_hlocal_security_descriptor stationWas_;
  wil::unique_hdesk desktop_;
  wil::unique_hlocal_security_descriptor desktopWas_;
};

// The test account of this run (main.cpp sets it for the time the tests run); nullptr when there
// is none.
TestAccount* CurrentTestAccount() noexcept;
void SetCurrentTestAccount(TestAccount* account) noexcept;

// Why the test account must not be made in this process: WhyNoTestAccount with this process's
// facts (the environment, its token, the registry check, Leftovers()). Call after COM is set up.
std::optional<std::string> WhyNoTestAccountHere();

}  // namespace pdft::systest
