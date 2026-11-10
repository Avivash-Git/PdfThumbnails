// The elevation boundary (elevation.h) without any elevation: RunElevated with launchers that
// never ask for administrator rights (they start this test program's "report" and "report-step"
// children normally, so the report pipe is tested end to end between real processes, and with it
// the parts a step did not complete as the asking side reads them back: this build's report, an
// older build's, one cut short and one naming a part this build cannot read), the reporter's
// refusal to write to a pipe its parent does not serve or its parent's user does not own, a pipe
// that is never read holding the reporter up for a few seconds at most, the asking side reading on
// past its cap and ending its reading when it gives up on a client that never stops writing, a
// report channel another process took, the token facts the report starts with and the ones the
// elevated side's entry check reads (this process's, and a standard user's token made from it,
// which the entry check must refuse even where it counts as elevated), and the installer's records
// (inno.h) in private registry keys. Nothing here calls the real launcher: a UAC prompt never
// appears.

#include <windows.h>

#include "elevation.h"

#include <gtest/gtest.h>
#include <sddl.h>
#include <wil/resource.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "child_process.h"
#include "inno.h"
#include "instance.h"
#include "medium_il.h"
#include "project_config.h"
#include "registry_roots.h"
#include "registry_sandbox.h"
#include "strings.h"

namespace pdft {
namespace {

using std::filesystem::path;

path SettingsExe() { return path(test::SelfFolder()) / kSettingsExe; }

// Makes this process's report pipe itself, as the asking side does but owned by |owner| (an SDDL
// SID: a SID string, or an alias such as "BA") and with |buffer| bytes of buffer; nothing reads
// from it. ERROR_INVALID_OWNER when this process's token may not make |owner| an owner.
HRESULT MakeOwnPipe(const std::wstring& owner, DWORD buffer, wil::unique_hfile* pipe) {
  std::wstring sid;
  if (const HRESULT hr = GetCurrentUserSid(&sid); FAILED(hr)) return hr;
  const std::wstring sddl = L"O:" + owner + L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;BA)";
  wil::unique_hlocal_security_descriptor descriptor;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            descriptor.put(), nullptr)) {
    return HRESULT_FROM_WIN32(GetLastError());
  }
  SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor.get(), FALSE};
  const HANDLE made =
      CreateNamedPipeW(ElevationPipeName(GetCurrentProcessId()).c_str(),
                       PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                       1, 0, buffer, 0, &attributes);
  if (made == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
  pipe->reset(made);
  return S_OK;
}

// Writes |bytes| to |pipe| (opened for overlapped I/O; |event| a manual-reset event for it) and
// waits up to |timeoutMs| for the pipe to take them; a write still pending then is cancelled.
// True when all of them went in.
bool WriteWithin(HANDLE pipe, HANDLE event, const std::string& bytes, DWORD timeoutMs) {
  OVERLAPPED overlapped = {};
  overlapped.hEvent = event;
  if (!WriteFile(pipe, bytes.data(), static_cast<DWORD>(bytes.size()), nullptr, &overlapped) &&
      GetLastError() != ERROR_IO_PENDING) {
    return false;
  }
  const bool inTime = WaitForSingleObject(event, timeoutMs) == WAIT_OBJECT_0;
  if (!inTime) CancelIoEx(pipe, &overlapped);
  DWORD written = 0;
  const bool ended = GetOverlappedResult(pipe, &overlapped, &written, TRUE) != FALSE;
  return inTime && ended && written == bytes.size();
}

// Starts this test program as "--pdft-child report <arguments>", the way the elevated process
// would be started but without elevation, waits for it, and returns its id and exit code.
HRESULT StartReportChild(const std::wstring& arguments, DWORD* processId, DWORD* exitCode) {
  return test::RunSelfChild(L"report " + arguments, processId, exitCode);
}

// RunElevated for |verb|, with a launcher that runs this test program's child "<arguments>".
ElevatedRun RunChild(ElevatedVerb verb, const std::wstring& arguments) {
  ElevatedRun run;
  EXPECT_EQ(RunElevated(
                verb, SettingsExe(), nullptr,
                [&](const ElevationRequest&, DWORD* processId, DWORD* exitCode) {
                  return test::RunSelfChild(arguments, processId, exitCode);
                },
                &run),
            S_OK);
  return run;
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
  EXPECT_EQ(run.reportServer, S_OK);
  EXPECT_FALSE(ReportChannelTaken(run));
  EXPECT_TRUE(run.reportChannel);
  EXPECT_EQ(run.report, (std::vector<std::wstring>{L"step one", L"step two: caf\u00E9"}));
  // Done, but without its end line: the report did not come back whole.
  EXPECT_FALSE(run.parts.ended);
  EXPECT_TRUE(ReportIncomplete(run));
  EXPECT_FALSE(PartsNotDone(run));
}

// ---- the parts a step did not complete, through the pipe ----

TEST(RunElevated, PartsNotDoneComeBack) {
  const ElevatedRun run =
      RunChild(ElevatedVerb::kMachineInstall,
               L"report-step machine-install 0 0 \"task-folder: 0x80070005\" \"old-builds: "
               L"0x80070057\"");
  EXPECT_EQ(run.outcome, ElevatedOutcome::kDone);
  EXPECT_TRUE(run.reportChannel);
  EXPECT_TRUE(run.parts.ended);
  EXPECT_EQ(run.parts.counted, 2u);
  EXPECT_FALSE(ReportIncomplete(run));
  EXPECT_EQ(NotDoneFailures(run, L"machine-install"),
            (std::vector<std::wstring>{L"machine-install task-folder: 0x80070005",
                                       L"machine-install old-builds: 0x80070057"}));
  ASSERT_FALSE(run.report.empty());
  EXPECT_EQ(run.report.back(), ElevatedEndLine(ElevatedVerb::kMachineInstall, S_OK, false, 2));
  EXPECT_EQ(run.report.front(), NotDoneLine(L"task-folder: 0x80070005"));
}

// A step that failed after naming its parts (as machine-uninstall does, ending with the first
// one's HRESULT): its parts stand for it, and where the failure started is said before the end.
TEST(RunElevated, AFailedStepNamesItsParts) {
  const std::wstring left = L"hive left: " + HResultText(kRegistryTreeLeftHr);
  const ElevatedRun run = RunChild(ElevatedVerb::kMachineUninstall,
                                   L"report-step machine-uninstall 0x80070005 0 "
                                   L"\"class: 0x80070005\" \"" +
                                       left + L"\"");
  EXPECT_EQ(run.outcome, ElevatedOutcome::kFailed);
  EXPECT_EQ(run.failure, E_ACCESSDENIED);
  EXPECT_TRUE(run.parts.ended);
  EXPECT_FALSE(NeedsStepFailure(run));
  EXPECT_EQ(NotDoneFailures(run, L"machine"),
            (std::vector<std::wstring>{L"machine class: 0x80070005", L"machine " + left}));
  EXPECT_NE(std::find(run.report.begin(), run.report.end(),
                      L"elevated machine-uninstall failed at a place not traced"),
            run.report.end());
  ASSERT_FALSE(run.report.empty());
  EXPECT_EQ(run.report.back(),
            ElevatedEndLine(ElevatedVerb::kMachineUninstall, E_ACCESSDENIED, false, 2));
}

// An older build's report: its end line has no count and it names no parts, so the exit code
// alone tells how the step ended, as it did.
TEST(RunElevated, AReportOfAnOlderBuild) {
  const ElevatedRun run =
      RunChild(ElevatedVerb::kMachineInstall,
               L"report 0 \"copied a build (4 files written)\" \"elevated machine-install end "
               L"hr=0x00000000\"");
  EXPECT_EQ(run.outcome, ElevatedOutcome::kDone);
  EXPECT_TRUE(run.parts.ended);
  EXPECT_FALSE(run.parts.counted);
  EXPECT_FALSE(ReportIncomplete(run));
  EXPECT_FALSE(PartsNotDone(run));
  EXPECT_TRUE(NotDoneFailures(run, L"machine-install").empty());
}

// A report cut short: what came is read, and the run says that its end is missing.
TEST(RunElevated, AReportCutShort) {
  const ElevatedRun run =
      RunChild(ElevatedVerb::kMachineInstall, L"report 0 \"not done: old-builds: 0x80070057\"");
  EXPECT_EQ(run.outcome, ElevatedOutcome::kDone);
  EXPECT_FALSE(run.parts.ended);
  EXPECT_TRUE(ReportIncomplete(run));
  EXPECT_EQ(NotDoneFailures(run, L"machine-install"),
            std::vector<std::wstring>{L"machine-install old-builds: 0x80070057"});
}

// A part this build cannot read (a later build's name, say): counted, never dropped.
TEST(RunElevated, APartALaterBuildNames) {
  const ElevatedRun run = RunChild(ElevatedVerb::kMachineRepairClass,
                                   L"report-step machine-repair-class 0 0 \"Copy (trash): "
                                   L"0x80070005\"");
  EXPECT_EQ(run.outcome, ElevatedOutcome::kDone);
  EXPECT_TRUE(run.parts.ended);
  EXPECT_TRUE(run.parts.named.empty());
  EXPECT_EQ(run.parts.counted, 1u);
  EXPECT_EQ(NotDoneFailures(run, L"machine"),
            std::vector<std::wstring>{L"machine unnamed parts: 1 (see the step's report)"});
}

// The exit code of a step's end: 0, 1 with a restart pending, and the HRESULT of a failure.
TEST(RunElevated, TheEndOfAStep) {
  ElevatedRun run =
      RunChild(ElevatedVerb::kMachineRepairClass, L"report-step machine-repair-class 0 0");
  EXPECT_EQ(run.outcome, ElevatedOutcome::kDone);
  EXPECT_EQ(run.exitCode, 0u);
  EXPECT_EQ(run.report, std::vector<std::wstring>{
                            ElevatedEndLine(ElevatedVerb::kMachineRepairClass, S_OK, false, 0)});
  run = RunChild(ElevatedVerb::kMachineRepairClass, L"report-step machine-repair-class 0 1");
  EXPECT_EQ(run.outcome, ElevatedOutcome::kRestartPending);
  EXPECT_EQ(run.exitCode, 1u);
  EXPECT_TRUE(run.parts.ended);
  run =
      RunChild(ElevatedVerb::kMachineRepairClass, L"report-step machine-repair-class 0x80004005 1");
  EXPECT_EQ(run.outcome, ElevatedOutcome::kFailed);
  EXPECT_EQ(run.failure, E_FAIL);
  EXPECT_EQ(run.exitCode, static_cast<DWORD>(E_FAIL));
  ASSERT_FALSE(run.report.empty());
  EXPECT_EQ(run.report.back(), ElevatedEndLine(ElevatedVerb::kMachineRepairClass, E_FAIL, true, 0));
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

// Someone else made the pipe first: RunElevated cannot listen and records that the channel was
// taken, the child sees that its parent does not serve the pipe and writes nothing to it, and the
// step's exit code still decides how it ended.
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
  EXPECT_TRUE(ReportChannelTaken(run))
      << "making the pipe: 0x" << std::hex << static_cast<unsigned long>(run.reportServer);
  EXPECT_FALSE(run.reportChannel);
  EXPECT_TRUE(run.report.empty());
  EXPECT_TRUE(ReportIncomplete(run));
  EXPECT_EQ(squatter.ReadLine(), "got=0");
  EXPECT_EQ(squatter.Wait(15000), 0u);
}

TEST(ElevatedReporterTest, NoPipeNoReport) {
  test::PipedChild child;
  ASSERT_EQ(child.Start(test::SelfPath(), L"--pdft-child report 0 \"nobody listens\""), S_OK);
  EXPECT_EQ(child.ReadLine(), "connected=0");
  EXPECT_EQ(child.Wait(10000), 0u);
}

// The parent made the pipe, but another than its user owns it: refused, as another account's
// pipe would be (no other account can make the user the owner of its pipe). The same pipe owned
// by the user is written to. Making Administrators an owner takes an elevated token.
TEST(ElevatedReporterTest, OnlyAPipeItsParentsUserOwns) {
  wil::unique_hfile pipe;
  const HRESULT made = MakeOwnPipe(L"BA", 64 * 1024, std::addressof(pipe));
  if (made == HRESULT_FROM_WIN32(ERROR_INVALID_OWNER)) {
    GTEST_SKIP() << "this process's token may not make Administrators the owner of a pipe (it is "
                    "not elevated), and no owner but its own user is open to it";
  }
  ASSERT_HRESULT_SUCCEEDED(made);
  {
    test::PipedChild child;
    ASSERT_EQ(child.Start(test::SelfPath(), L"--pdft-child report 0 \"secret\""), S_OK);
    EXPECT_EQ(child.ReadLine(), "connected=0");
    EXPECT_EQ(child.Wait(10000), 0u);
  }

  pipe.reset();
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  ASSERT_HRESULT_SUCCEEDED(MakeOwnPipe(sid, 64 * 1024, std::addressof(pipe)));
  test::PipedChild child;
  ASSERT_EQ(child.Start(test::SelfPath(), L"--pdft-child report 0 \"step one\""), S_OK);
  EXPECT_EQ(child.ReadLine(), "connected=1");
  EXPECT_EQ(child.Wait(10000), 0u);
}

// A parent that never reads (a pipe without a buffer takes next to nothing until it is read)
// holds the step up for about kReportWriteTimeoutMs at most: the reporter cancels the write,
// gives up on the rest, and the step ends with its own exit code.
TEST(ElevatedReporterTest, AParentThatNeverReadsDoesNotHoldItUp) {
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  wil::unique_hfile pipe;
  ASSERT_HRESULT_SUCCEEDED(MakeOwnPipe(sid, 0, std::addressof(pipe)));
  // More than any buffer the pipe could round its size up to.
  std::wstring lines;
  for (int i = 0; i < 20; ++i) lines += L" " + std::wstring(1000, L'x');
  const ULONGLONG start = GetTickCount64();
  test::PipedChild child;
  ASSERT_EQ(child.Start(test::SelfPath(), L"--pdft-child report 3" + lines), S_OK);
  EXPECT_EQ(child.ReadLine(), "connected=1");
  const DWORD budget = kReportWriteTimeoutMs + 15000;
  EXPECT_EQ(child.Wait(budget), 3u);
  const ULONGLONG elapsed = GetTickCount64() - start;
  std::printf("the step ended after %llu ms; the limit per line is %lu ms\n", elapsed,
              static_cast<unsigned long>(kReportWriteTimeoutMs));
  EXPECT_LT(elapsed, static_cast<ULONGLONG>(budget));
}

// ---- the asking side's end ----

// Past its cap the asking side reads on and drops what comes, so that a step with a lot to say
// never waits for room in the pipe; it keeps the first kMaxReportBytes bytes. This process is
// the pipe's client here.
TEST(ElevationReportServerTest, ReadsOnPastItsCap) {
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  std::unique_ptr<ElevationReportServer> server;
  ASSERT_EQ(ElevationReportServer::Create(sid, &server), S_OK);
  wil::unique_hfile client(CreateFileW(ElevationPipeName(GetCurrentProcessId()).c_str(),
                                       GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_OVERLAPPED, nullptr));
  ASSERT_TRUE(client);
  wil::unique_event_nothrow written;
  ASSERT_HRESULT_SUCCEEDED(written.create(wil::EventOptions::ManualReset));
  const std::string line = std::string(1000, 'x') + "\n";
  // Four times the cap: far more than the pipe's buffer holds once nothing is read.
  const size_t count = 4 * kMaxReportBytes / line.size();
  for (size_t i = 0; i < count; ++i) {
    ASSERT_TRUE(WriteWithin(client.get(), written.get(), line, 10000))
        << "line " << i << " of " << count << " was not taken";
  }
  client.reset();
  bool fromClient = false;
  const std::vector<std::wstring> lines = server->Finish(GetCurrentProcessId(), &fromClient);
  EXPECT_TRUE(fromClient);
  // The whole lines of the first kMaxReportBytes bytes, and the start of the next one.
  ASSERT_EQ(lines.size(), kMaxReportBytes / line.size() + 1);
  EXPECT_EQ(lines.front(), std::wstring(1000, L'x'));
  EXPECT_EQ(lines.back(), std::wstring(kMaxReportBytes % line.size(), L'x'));
}

// A client that never stops writing cannot hold the asking side up: once Finish stops waiting
// for the rest (a few seconds), the reading ends however fast the bytes come, and what is left is
// dropped. This process is the pipe's client here, with several writes queued at all times, so
// every read finds bytes waiting and none of them has to wait.
TEST(ElevationReportServerTest, FinishEndsAClientThatNeverStopsWriting) {
  std::wstring sid;
  ASSERT_HRESULT_SUCCEEDED(GetCurrentUserSid(&sid));
  std::unique_ptr<ElevationReportServer> server;
  ASSERT_EQ(ElevationReportServer::Create(sid, &server), S_OK);
  wil::unique_hfile client(CreateFileW(ElevationPipeName(GetCurrentProcessId()).c_str(),
                                       GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_OVERLAPPED, nullptr));
  ASSERT_TRUE(client);
  constexpr DWORD kQueued = 4;
  const std::string chunk(64 * 1024, 'x');
  std::array<wil::unique_event_nothrow, kQueued> ended;
  std::array<OVERLAPPED, kQueued> writes = {};
  std::array<bool, kQueued> pending = {};
  for (wil::unique_event_nothrow& event : ended) {
    ASSERT_HRESULT_SUCCEEDED(event.create(wil::EventOptions::ManualReset));
  }
  // Queues write |i| (again); it stays pending until the pipe has room for it.
  const auto queue = [&](DWORD i) {
    writes[i] = OVERLAPPED{};
    writes[i].hEvent = ended[i].get();
    pending[i] = WriteFile(client.get(), chunk.data(), static_cast<DWORD>(chunk.size()), nullptr,
                           &writes[i]) ||
                 GetLastError() == ERROR_IO_PENDING;
    if (!pending[i]) ended[i].ResetEvent();
  };
  for (DWORD i = 0; i < kQueued; ++i) queue(i);
  std::atomic<bool> stopWriting = false;
  std::thread writer([&] {
    HANDLE waits[kQueued] = {};
    for (DWORD i = 0; i < kQueued; ++i) waits[i] = ended[i].get();
    while (!stopWriting) {
      const DWORD waited = WaitForMultipleObjects(kQueued, waits, FALSE, 100);
      if (waited == WAIT_TIMEOUT) continue;
      if (waited >= WAIT_OBJECT_0 + kQueued) break;
      const DWORD i = waited - WAIT_OBJECT_0;
      DWORD written = 0;
      const bool wrote = GetOverlappedResult(client.get(), &writes[i], &written, FALSE) != FALSE;
      pending[i] = false;
      if (wrote) {
        queue(i);
      } else {
        ended[i].ResetEvent();  // the server's end went away: nothing more goes in there
      }
    }
  });

  const ULONGLONG start = GetTickCount64();
  bool fromClient = false;
  const std::vector<std::wstring> lines = server->Finish(GetCurrentProcessId(), &fromClient);
  const ULONGLONG elapsed = GetTickCount64() - start;
  // The server's end goes, which ends the writes still queued, and then the writer.
  server.reset();
  stopWriting = true;
  writer.join();
  CancelIoEx(client.get(), nullptr);
  for (DWORD i = 0; i < kQueued; ++i) {
    DWORD unused = 0;
    if (pending[i]) GetOverlappedResult(client.get(), &writes[i], &unused, TRUE);
  }
  std::printf("Finish returned after %llu ms while the client kept writing\n", elapsed);
  EXPECT_TRUE(fromClient);
  EXPECT_FALSE(lines.empty());
  // Finish waits a few seconds for the rest; the reading must end soon after that.
  EXPECT_LT(elapsed, 15000u);
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

// ---- where this executable runs from ----

// The test program is not the settings executable in the machine root, wherever the build is: the
// settings window would not start again for another account from such a place.
TEST(MachineRootCheckTest, TheTestProgramIsNotTheInstallForAllUsers) {
  const MachineRootCheck check = CheckMachineRoot();
  EXPECT_FALSE(check.runsFromMachineRoot);
  EXPECT_FALSE(IsMachineInstall(check));
  std::printf("machine root's reparse point check: 0x%08lX\n",
              static_cast<unsigned long>(check.reparsePoints));
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
