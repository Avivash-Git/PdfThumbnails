// pdft_integration_tests: the app's core modules against the real system (COM, WMI, Task
// Scheduler, named objects, the process tree) and the two executables run as a script would.
// Writes nothing to the real profile except the canary's volatile probe key, which it deletes;
// the executables' writing command is started only where its writes are swallowed anyway.
//
// main() sets up COM exactly like the app (InitializeComSecurity, once), so the COM tests show
// what works under the app's own COM security. It also gives the app's named objects a suffix
// of this run's own (a new GUID), so the tests never take, or wait for, the objects of a running
// app or of another test run.
//
// "--pdft-child <mode>" makes the program the child process that some tests start: the test
// program starts itself, so no second locally built program has to be allowed to run.
//   hold-lock <suffix>           takes the operation lock (named with the parent's suffix),
//                                prints "locked", waits until its standard input closes,
//                                releases the lock and exits 0
//   hold-lock-and-exit <suffix>  the same, but ends without releasing the lock
//   ancestor <depth>             depth > 0: starts itself with depth - 1 (same standard output)
//                                and returns its exit code; depth 0: prints the packaged
//                                ancestor ("none")
//   squat-lock <held> <fresh>    run below Medium integrity: prints its integrity level, tries
//                                to open the mutex <held> to wait on it ("open=yes"/"open=no
//                                <error>"), then to create and own the mutex <fresh> ("create=
//                                yes"/"create=no <error>"), and keeps it until its standard
//                                input closes

#include <windows.h>

#include <gtest/gtest.h>
#include <objbase.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "child_process.h"
#include "com_security.h"
#include "context.h"
#include "instance.h"
#include "medium_il.h"
#include "strings.h"

namespace {

void Print(const std::string& text) {
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written,
            nullptr);
}

int HoldLock(bool release) {
  std::optional<pdft::OperationLock> lock;
  if (FAILED(pdft::OperationLock::Acquire(30000, &lock))) return 3;
  Print("locked\n");
  char buffer[64];
  DWORD read = 0;
  while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &read, nullptr) &&
         read > 0) {
  }
  if (!release) TerminateProcess(GetCurrentProcess(), 0);  // ends without releasing it
  lock.reset();
  return 0;
}

// What a process of the same user below Medium integrity can do to the operation lock.
int SquatLock(const wchar_t* held, const wchar_t* fresh) {
  char text[64];
  sprintf_s(text, "il=0x%04lX\n", pdft::test::CurrentIntegrityRid());
  Print(text);
  HANDLE opened = OpenMutexW(SYNCHRONIZE, FALSE, held);
  if (opened) {
    Print("open=yes\n");
    CloseHandle(opened);
  } else {
    sprintf_s(text, "open=no %lu\n", GetLastError());
    Print(text);
  }
  HANDLE created = CreateMutexW(nullptr, TRUE, fresh);
  const DWORD error = GetLastError();
  if (created && error != ERROR_ALREADY_EXISTS) {
    Print("create=yes\n");
  } else {
    sprintf_s(text, "create=no %lu\n", created ? ERROR_ALREADY_EXISTS : error);
    Print(text);
  }
  char buffer[64];
  DWORD read = 0;
  while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &read, nullptr) &&
         read > 0) {
  }
  if (created) CloseHandle(created);
  return 0;
}

int Ancestor(int depth) {
  if (depth > 0) {
    std::wstring commandLine = pdft::test::QuoteArgument(pdft::test::SelfPath()) +
                               L" --pdft-child ancestor " + std::to_wstring(depth - 1);
    STARTUPINFOW startup = {sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
      return 4;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 5;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
  }
  std::optional<std::wstring> package;
  if (FAILED(pdft::FindPackagedAncestor(&package))) return 6;
  Print(package ? pdft::WideToUtf8Lossy(*package) + "\n" : "none\n");
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc >= 3 && std::wstring(argv[1]) == L"--pdft-child") {
    const std::wstring mode = argv[2];
    if (mode == L"hold-lock" && argc == 4) {
      pdft::SetObjectNameSuffixForTesting(argv[3]);
      return HoldLock(true);
    }
    if (mode == L"hold-lock-and-exit" && argc == 4) {
      pdft::SetObjectNameSuffixForTesting(argv[3]);
      return HoldLock(false);
    }
    if (mode == L"ancestor" && argc == 4) return Ancestor(_wtoi(argv[3]));
    if (mode == L"squat-lock" && argc == 5) return SquatLock(argv[3], argv[4]);
    return 2;
  }

  GUID run = {};
  wchar_t runText[40] = {};
  if (FAILED(CoCreateGuid(&run)) || StringFromGUID2(run, runText, ARRAYSIZE(runText)) == 0) {
    std::printf("cannot make a GUID for this run's named objects\n");
    return 1;
  }
  pdft::test::NamedObjectSuffix() = std::wstring(L".test-") + runText;
  pdft::SetObjectNameSuffixForTesting(pdft::test::NamedObjectSuffix());

  const HRESULT com = pdft::InitializeComSecurity();
  if (FAILED(com)) {
    std::printf("InitializeComSecurity failed: 0x%08lX\n", static_cast<unsigned long>(com));
    return 1;
  }
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  CoUninitialize();
  return result;
}
