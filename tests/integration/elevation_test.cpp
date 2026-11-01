// The elevation boundary (elevation.h) without any elevation: RunElevated with launchers that
// never ask for administrator rights (one starts this test program's "report" child normally,
// so the report pipe is tested end to end between real processes), the reporter's refusal to
// write to a pipe its parent does not serve, the token facts the report starts with and the ones
// the elevated side's entry check reads (this process's, and a standard user's token made from
// it, which the entry check must refuse even where it counts as elevated), and the installer's
// records (inno.h) in private registry keys. Nothing here calls the real launcher: a UAC prompt
// never appears.

#include <windows.h>

#include "elevation.h"

#include <gtest/gtest.h>
#include <wil/resource.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "child_process.h"
#include "inno.h"
#include "medium_il.h"
#include "project_config.h"
#include "registry_sandbox.h"

namespace pdft {
namespace {

using std::filesystem::path;

path SettingsExe() { return path(test::SelfFolder()) / kSettingsExe; }

// Starts this test program as "--pdft-child report <arguments>", the way the elevated process
// would be started but without elevation, waits for it, and returns its id and exit code.
HRESULT StartReportChild(const std::wstring& arguments, DWORD* processId, DWORD* exitCode) {
  std::wstring commandLine =
      test::QuoteArgument(test::SelfPath()) + L" --pdft-child report " + arguments;
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(test::SelfPath().c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  wil::unique_handle thread(process.hThread);
  wil::unique_handle handle(process.hProcess);
  if (WaitForSingleObject(handle.get(), 30000) != WAIT_OBJECT_0) {
    TerminateProcess(handle.get(), 1);
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
  *processId = process.dwProcessId;
  if (!GetExitCodeProcess(handle.get(), exitCode)) return HRESULT_FROM_WIN32(GetLastError());
  return S_OK;
}

TEST(RunElevated, CollectsItsChildsReport) {
  ElevationRequest seen;
  int calls = 0;
  ElevatedRun run;
  ASSERT_EQ(RunElevated(
                ElevatedVerb::kMachineRepairClass, SettingsExe(), nullptr,
                [&](const ElevationRequest& request, DWORD* processId, DWORD* exitCode) {
                  seen = request;
                  ++calls;
                  return StartReportChild(L"1 \"step one\" \"step two: caf\u00E9\"", processId,
                                          exitCode);
                },
                &run),
            S_OK);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(seen.exe, SettingsExe());
  EXPECT_EQ(seen.parameters, L"--elevated machine-repair-class");
  EXPECT_EQ(seen.owner, nullptr);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kRestartPending);
  EXPECT_EQ(run.exitCode, 1u);
  EXPECT_TRUE(run.reportChannel);
  EXPECT_EQ(run.report, (std::vector<std::wstring>{L"step one", L"step two: caf\u00E9"}));
}

TEST(RunElevated, AFailedStep) {
  ElevatedRun run;
  ASSERT_EQ(RunElevated(
                ElevatedVerb::kMachineUninstall, SettingsExe(), nullptr,
                [](const ElevationRequest&, DWORD* processId, DWORD* exitCode) {
                  return StartReportChild(L"0x80070020 \"copy failed\"", processId, exitCode);
                },
                &run),
            S_OK);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kFailed);
  EXPECT_EQ(run.failure, HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION));
  EXPECT_EQ(run.report, std::vector<std::wstring>{L"copy failed"});
}

TEST(RunElevated, ADeclinedPrompt) {
  ElevatedRun run;
  ASSERT_EQ(RunElevated(
                ElevatedVerb::kMachineUninstall, SettingsExe(), nullptr,
                [](const ElevationRequest&, DWORD*, DWORD*) {
                  return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                },
                &run),
            S_OK);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kCancelled);
  EXPECT_FALSE(run.reportChannel);
  EXPECT_TRUE(run.report.empty());
}

TEST(RunElevated, OnlyTheSettingsExecutable) {
  int calls = 0;
  const ElevationLauncher counting = [&](const ElevationRequest&, DWORD*, DWORD*) {
    ++calls;
    return S_OK;
  };
  ElevatedRun run;
  EXPECT_EQ(RunElevated(ElevatedVerb::kMachineUninstall, path(test::SelfFolder()) / kCliExe,
                        nullptr, counting, &run),
            E_INVALIDARG);
  EXPECT_EQ(RunElevated(ElevatedVerb::kMachineUninstall, kSettingsExe, nullptr, counting, &run),
            E_INVALIDARG);
  EXPECT_EQ(RunElevated(ElevatedVerb::kMachineUninstall, SettingsExe(), nullptr, nullptr, &run),
            E_INVALIDARG);
  EXPECT_EQ(calls, 0);
}

// Someone else made the pipe first: RunElevated cannot listen, and the child sees that its
// parent does not serve the pipe and writes nothing to it.
TEST(RunElevated, SomeoneElsesPipeGetsNothing) {
  test::PipedChild squatter;
  ASSERT_EQ(squatter.Start(test::SelfPath(),
                           L"--pdft-child pipe-squat " + std::to_wstring(GetCurrentProcessId())),
            S_OK);
  ASSERT_EQ(squatter.ReadLine(), "ready");
  ElevatedRun run;
  ASSERT_EQ(RunElevated(
                ElevatedVerb::kMachineRepairClass, SettingsExe(), nullptr,
                [](const ElevationRequest&, DWORD* processId, DWORD* exitCode) {
                  return StartReportChild(L"0 \"secret\"", processId, exitCode);
                },
                &run),
            S_OK);
  EXPECT_EQ(run.outcome, ElevatedOutcome::kDone);
  EXPECT_FALSE(run.reportChannel);
  EXPECT_TRUE(run.report.empty());
  EXPECT_EQ(squatter.ReadLine(), "got=0");
  EXPECT_EQ(squatter.Wait(15000), 0u);
}

TEST(ElevatedReporterTest, NoPipeNoReport) {
  test::PipedChild child;
  ASSERT_EQ(child.Start(test::SelfPath(), L"--pdft-child report 0 \"nobody listens\""), S_OK);
  EXPECT_EQ(child.ReadLine(), "connected=0");
  EXPECT_EQ(child.Wait(10000), 0u);
}

// ---- the token facts an elevated step's report starts with ----

TEST(TokenFactsTest, ThisProcessAndAStandardUsersToken) {
  TokenFacts facts;
  ASSERT_HRESULT_SUCCEEDED(ReadTokenFacts(GetCurrentProcessToken(), &facts));
  std::printf("this process: %ls\n", TokenFactsText(facts).c_str());
  bool elevated = false;
  ASSERT_HRESULT_SUCCEEDED(GetTokenElevation(&elevated));
  EXPECT_EQ(facts.elevated, elevated);
  EXPECT_EQ(facts.integrityRid, test::CurrentIntegrityRid());
  BYTE administrators[SECURITY_MAX_SID_SIZE];
  DWORD size = sizeof(administrators);
  ASSERT_TRUE(CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrators, &size));
  BOOL member = FALSE;
  ASSERT_TRUE(CheckTokenMembership(nullptr, administrators, &member));
  EXPECT_EQ(facts.administrators == TokenFacts::Group::kEnabled, member != FALSE);

  // What the elevated side's entry check reads of the same token agrees, and it lets this
  // process through only with administrator rights: elevated, Administrators enabled, High.
  ElevatedEntryFacts entry;
  ASSERT_HRESULT_SUCCEEDED(ReadEntryTokenFacts(nullptr, &entry));
  EXPECT_EQ(entry.tokenElevated, elevated);
  EXPECT_EQ(entry.administratorsEnabled, member != FALSE);
  EXPECT_EQ(entry.integrityRid, facts.integrityRid);
  wil::unique_handle processToken;
  ASSERT_TRUE(
      OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, processToken.put()));
  ElevatedEntryFacts own;
  ASSERT_HRESULT_SUCCEEDED(ReadEntryTokenFacts(processToken.get(), &own));
  EXPECT_EQ(own.tokenElevated, entry.tokenElevated);
  EXPECT_EQ(own.administratorsEnabled, entry.administratorsEnabled);
  EXPECT_EQ(own.integrityRid, entry.integrityRid);
  entry.verb = ElevatedVerb::kMachineRepairClass;
  entry.runsFromMachineRoot = true;
  const bool rights =
      elevated && member && facts.integrityRid >= static_cast<DWORD>(SECURITY_MANDATORY_HIGH_RID);
  EXPECT_EQ(CheckElevatedEntry(entry), rights ? S_OK : kNoAdministratorRightsHr);
  std::printf("this process's entry check: %ls\n",
              rights ? L"passes" : ElevatedRefusalText(FindElevatedRefusal(entry)).data());

  // The token the system tests ran their commands with before they had an account of their own:
  // Administrators deny-only (or absent, for an account outside the group), Medium. Whether it
  // counts as elevated depends on the token it is made from: made from a limited token (a split
  // administrator's) it does not. Made from a token without a linked one (elevation type 1: the
  // built-in Administrator, as on CI) it does, and runas then starts the "elevated" step with it
  // unchanged: the line printed says which, and the entry check refuses it either way.
  const TOKEN_ELEVATION_TYPE sourceType = facts.elevationType;
  wil::unique_handle standard;
  ASSERT_HRESULT_SUCCEEDED(
      test::CreateMediumTokenForSelf(test::MediumToken::kFiltered, standard.put()));
  ASSERT_HRESULT_SUCCEEDED(ReadTokenFacts(standard.get(), &facts));
  std::printf("standard user's: %ls\n", TokenFactsText(facts).c_str());
  if (sourceType == TokenElevationTypeLimited) EXPECT_FALSE(facts.elevated);
  EXPECT_EQ(facts.integrityRid, static_cast<DWORD>(SECURITY_MANDATORY_MEDIUM_RID));
  EXPECT_NE(facts.administrators, TokenFacts::Group::kEnabled);

  ElevatedEntryFacts filtered;
  ASSERT_HRESULT_SUCCEEDED(ReadEntryTokenFacts(standard.get(), &filtered));
  EXPECT_EQ(filtered.tokenElevated, facts.elevated);
  EXPECT_FALSE(filtered.administratorsEnabled);
  EXPECT_EQ(filtered.integrityRid, static_cast<DWORD>(SECURITY_MANDATORY_MEDIUM_RID));
  filtered.verb = ElevatedVerb::kMachineRepairClass;
  filtered.runsFromMachineRoot = true;
  const ElevatedRefusal refusal = FindElevatedRefusal(filtered);
  EXPECT_EQ(refusal, filtered.tokenElevated ? ElevatedRefusal::kAdministratorsNotEnabled
                                            : ElevatedRefusal::kNotElevated);
  EXPECT_EQ(CheckElevatedEntry(filtered), kNoAdministratorRightsHr);
  std::printf("standard user's entry check: refused, %ls\n", ElevatedRefusalText(refusal).data());
}

// ---- the installer's records, in private keys ----

TEST(InnoRecords, MachineFirstThenUser) {
  test::ScopedRegistrySandbox sandbox;
  ASSERT_TRUE(sandbox.ok()) << sandbox.error();
  const RegistryRoots roots{sandbox.user(), sandbox.machine()};
  std::optional<InnoInstall> install;
  ASSERT_EQ(FindInnoInstall(roots, &install), S_OK);
  EXPECT_FALSE(install);

  const std::wstring key = InnoUninstallKey();
  ASSERT_EQ(WriteRegistryString(roots.user, key, L"UninstallString",
                                L"\"D:\\Home\\pat\\Programs\\x\\unins000.exe\""),
            S_OK);
  ASSERT_EQ(FindInnoInstall(roots, &install), S_OK);
  ASSERT_TRUE(install);
  EXPECT_EQ(install->scope, Scope::kUser);
  EXPECT_EQ(install->uninstallString, L"\"D:\\Home\\pat\\Programs\\x\\unins000.exe\"");

  // The key alone counts, with or without its value.
  HKEY created = nullptr;
  ASSERT_EQ(RegCreateKeyExW(roots.machine, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                            &created, nullptr),
            ERROR_SUCCESS);
  RegCloseKey(created);
  ASSERT_EQ(FindInnoInstall(roots, &install), S_OK);
  ASSERT_TRUE(install);
  EXPECT_EQ(install->scope, Scope::kMachine);
  EXPECT_TRUE(install->uninstallString.empty());
}

}  // namespace
}  // namespace pdft
