// What the system tests stand on: when they may run at all, the real state of this PC that they
// change (and what is left of it afterwards), and starting the app's executables the two ways a
// person or an installer does: as the signed-in user (the test account, test_account.h), or as an
// administrator.

#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pdft::systest {

// The tests run only where this is "1" in the environment (the CI job sets it), so nobody runs
// them on their own PC by accident; a second opt-in says the PC is thrown away afterwards
// (account_rules.h kGitHubActionsEnv with kRunnerEnvironmentEnv, or kThrowawayVmEnv).
inline constexpr wchar_t kEnableEnv[] = L"PDFT_SYSTEM_TESTS";
// The staged runtime they install from (cmake --install <build> --prefix <folder> --component
// runtime): copied into a private folder first, which only the test account, this account,
// SYSTEM and Administrators can change. The copy is made with administrator rights, so the
// folder must be on a local drive, with no link, junction or mount point on the way to it or in
// it (the tests refuse it otherwise), and nobody but administrators should be able to change it.
inline constexpr wchar_t kRuntimeEnv[] = L"PDFT_SYSTEM_RUNTIME";
// The folder with 1.x's shipped files the migration tests plant; empty: system-fixture next to
// this program (the build copies them there, and the test bundle carries them).
inline constexpr wchar_t kFixtureEnv[] = L"PDFT_SYSTEM_FIXTURE";

// CTest's "skipped" exit code (SKIP_RETURN_CODE), and the one for a PC that has the product.
inline constexpr int kSkipCode = 77;
inline constexpr int kRefusedCode = 3;

// How long one command may take before it is ended (an elevation that waits for a prompt nobody
// can answer would otherwise hang the job).
inline constexpr DWORD kCommandTimeoutMs = 10 * 60 * 1000;

// The value of the environment variable |name|; empty when it is not set.
std::wstring EnvironmentValue(const wchar_t* name);

// Why the tests must not run in this process: not asked for (kEnableEnv and the second opt-in),
// not elevated, or a registry that may be virtualized or a packaged parent (writes would not
// reach the real hive). Call after COM is set up (the registry check reads through WMI).
std::optional<std::string> WhyNotHere();

// Where one user's own state lives: their HKEY_CURRENT_USER, their SID (their check task's name)
// and the install and data folders in their profile.
struct UserPlaces {
  std::string who;  // "this account", "the test account": for the lines that name what was found
  HKEY hive = HKEY_CURRENT_USER;
  std::wstring sid;
  std::filesystem::path userRoot;  // %LOCALAPPDATA%\Programs\<Product>
  std::filesystem::path dataDir;   // %LOCALAPPDATA%\<Product>
};

// The users whose state the tests look at: this process's own account (nothing may ever turn up
// there), and the test account and the standard account once they are there.
std::vector<UserPlaces> TestedUsers();

// Everything of 1.x or 2.x found on this PC, one line each: the classes, .pdf slots that name the
// handler, the product's settings key, RunOnce values of the clean-up family, the check task
// folder and each tested user's check task, 1.x's task, the install folders, the data folders, the
// Active Setup component and an installer's uninstall record. Trash folders next to the machine
// root are not listed: they wait for the restart (TrashFolders).
std::vector<std::string> Leftovers();

// The trash folders next to the machine-wide install folder.
std::vector<std::filesystem::path> TrashFolders();

// Whether |path| is queued for deletion at the next restart (PendingFileRenameOperations).
bool QueuedForRestart(const std::filesystem::path& path);

// Removes what Leftovers() lists and the trash folders, without the app: the safety net after a
// failed test, so the next one starts from a clean PC. Prints what it could not remove. Never
// touches an installer's record.
void ForceClean();

// Whether the Task Scheduler folder |folder| exists; std::nullopt when that cannot be told.
std::optional<bool> TaskFolderExists(std::wstring_view folder);

// Removes what the integration tests' task tests left in Task Scheduler: the folders and the
// root-folder tasks whose names start with "<product>-Test" (theirs: \<product>-Test-<pid>-<GUID>
// and \<product>-Test check <pid>-<GUID> ...), a folder with everything in it. One line for each
// ("task folder \<name>: removed", or "left" when it is still there); empty when nothing was
// left. A line saying so when Task Scheduler cannot be read.
std::vector<std::string> RemoveTaskTestLeftovers();

struct Outcome {
  HRESULT hr = E_FAIL;  // starting the program and waiting for it
  DWORD exitCode = 0;
  std::string out;  // standard output, as bytes
  std::string err;  // standard error
  std::string Output() const { return out + err; }
};

// Runs |exe| with |arguments| (already quoted) as the signed-in user: the test account with the
// limited token of its interactive logon. From there, an administrator step goes through UAC like
// a person's would. E_UNEXPECTED when there is no test account.
Outcome RunAsUser(const std::filesystem::path& exe, const std::wstring& arguments);

// Runs |exe| with this process's token (elevated), as an installer does.
Outcome RunAsAdministrator(const std::filesystem::path& exe, const std::wstring& arguments);

}  // namespace pdft::systest
