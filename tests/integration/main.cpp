// pdft_integration_tests: the app's core modules against the real system (COM, WMI, Task
// Scheduler, named objects, the process tree) and the two executables run as a script would.
// Writes nothing to the real profile except the canary's volatile probe key, which it deletes;
// the executables' writing command is started only where its writes are swallowed anyway. In
// Task Scheduler it registers test tasks of its own (tasks_test.cpp) and deletes them again.
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
//   apply-task <folder> <name> <fallback name> <program>
//                                registers the check task with both triggers through
//                                ApplyTaskOptions, as whatever token it runs with (the tests
//                                start it with a standard user's token), at <folder>\<name> or
//                                \<fallback name>, running <program>; prints
//                                "apply-task hr=0x<HRESULT> elevated=<0|1>"
//   make-folder <folder>         makes the top-level Task Scheduler folder <folder> with the
//                                root folder's permissions inherited, as whatever token it runs
//                                with (the tests start it with a standard user's token, so that
//                                user owns it); prints "make-folder hr=0x<HRESULT>"
//   hold-file <path>             opens the file <path> for reading, sharing only reading (so it
//                                can be neither deleted nor renamed), prints "held", and keeps
//                                it open until its standard input closes
//   parent                       prints this process's parent process id (GetParentProcessId)
//                                as "parent=<id>", or "parent=none"
//   report <exit code> <line>... connects to its parent's elevation report pipe the way the
//                                elevated process does (ElevatedReporter), prints "connected=1"
//                                or "connected=0", writes each <line> to the pipe and exits with
//                                <exit code> (decimal, or hexadecimal with 0x)
//   pipe-squat <process id>      creates the elevation report pipe named after <process id>, as
//                                someone else would, prints "ready", waits up to 10 s for a
//                                client and prints "got=<bytes it sent>" or "got=none"

#include <windows.h>

#include <gtest/gtest.h>
#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "child_process.h"
#include "com_security.h"
#include "context.h"
#include "elevation.h"
#include "instance.h"
#include "medium_il.h"
#include "processes.h"
#include "strings.h"
#include "tasks.h"

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

int ApplyTask(const wchar_t* folder, const wchar_t* name, const wchar_t* fallbackName,
              const wchar_t* program) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 3;
  pdft::ProcessContext context;
  HRESULT hr = pdft::GetProcessContext(pdft::ContextProbe::kNone, &context);
  if (SUCCEEDED(hr)) {
    const pdft::TaskLocation location{folder, name, L"\\", fallbackName, context.userSid};
    hr =
        pdft::ApplyTaskOptions(location, pdft::OptionsFromCheckboxes(true, true), program, context);
  }
  char text[64];
  sprintf_s(text, "apply-task hr=0x%08lX elevated=%d\n", static_cast<unsigned long>(hr),
            context.elevated ? 1 : 0);
  Print(text);
  CoUninitialize();
  return 0;
}

int MakeFolder(const wchar_t* folder) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 3;
  HRESULT hr = S_OK;
  {
    wil::com_ptr_nothrow<ITaskService> service;
    hr = CoCreateInstance(__uuidof(TaskScheduler), nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(service.put()));
    VARIANT empty;
    VariantInit(&empty);
    if (SUCCEEDED(hr)) hr = service->Connect(empty, empty, empty, empty);
    wil::com_ptr_nothrow<ITaskFolder> root;
    const wil::unique_bstr rootPath = wil::make_bstr_nothrow(L"\\");
    const wil::unique_bstr path = wil::make_bstr_nothrow(folder);
    if (SUCCEEDED(hr) && (!rootPath || !path)) hr = E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = service->GetFolder(rootPath.get(), root.put());
    wil::com_ptr_nothrow<ITaskFolder> made;
    if (SUCCEEDED(hr)) hr = root->CreateFolder(path.get(), empty, made.put());
  }
  char text[48];
  sprintf_s(text, "make-folder hr=0x%08lX\n", static_cast<unsigned long>(hr));
  Print(text);
  CoUninitialize();
  return 0;
}

void WaitForInputToClose() {
  char buffer[64];
  DWORD read = 0;
  while (ReadFile(GetStdHandle(STD_INPUT_HANDLE), buffer, sizeof(buffer), &read, nullptr) &&
         read > 0) {
  }
}

int HoldFile(const wchar_t* path) {
  wil::unique_hfile file(CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
  if (!file) return 3;
  Print("held\n");
  WaitForInputToClose();
  return 0;
}

int Parent() {
  DWORD parent = 0;
  if (FAILED(pdft::GetParentProcessId(&parent))) {
    Print("parent=none\n");
  } else {
    Print("parent=" + std::to_string(parent) + "\n");
  }
  return 0;
}

int Report(int argc, wchar_t** argv) {
  const DWORD exitCode = wcstoul(argv[3], nullptr, 0);
  pdft::ElevatedReporter reporter = pdft::ElevatedReporter::Connect();
  Print(reporter.connected() ? "connected=1\n" : "connected=0\n");
  for (int i = 4; i < argc; ++i) reporter.Line(argv[i]);
  return static_cast<int>(exitCode);
}

int PipeSquat(const wchar_t* processId) {
  const std::wstring name = pdft::ElevationPipeName(wcstoul(processId, nullptr, 10));
  wil::unique_hfile pipe(CreateNamedPipeW(
      name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 65536, 0,
      nullptr));
  if (!pipe) return 3;
  wil::unique_event_nothrow event;
  if (FAILED(event.create(wil::EventOptions::ManualReset))) return 4;
  Print("ready\n");
  OVERLAPPED overlapped = {};
  overlapped.hEvent = event.get();
  DWORD transferred = 0;
  if (!ConnectNamedPipe(pipe.get(), &overlapped)) {
    const DWORD error = GetLastError();
    if (error == ERROR_IO_PENDING) {
      if (WaitForSingleObject(event.get(), 10000) != WAIT_OBJECT_0) {
        CancelIoEx(pipe.get(), &overlapped);
        GetOverlappedResult(pipe.get(), &overlapped, &transferred, TRUE);
        Print("got=none\n");
        return 0;
      }
    } else if (error != ERROR_PIPE_CONNECTED && error != ERROR_NO_DATA) {
      Print("got=none\n");
      return 0;
    }
  }
  size_t got = 0;
  char buffer[256];
  for (;;) {
    overlapped = OVERLAPPED{};
    overlapped.hEvent = event.get();
    transferred = 0;
    if (!ReadFile(pipe.get(), buffer, sizeof(buffer), &transferred, &overlapped)) {
      if (GetLastError() != ERROR_IO_PENDING) break;
      if (WaitForSingleObject(event.get(), 5000) != WAIT_OBJECT_0 ||
          !GetOverlappedResult(pipe.get(), &overlapped, &transferred, FALSE)) {
        CancelIoEx(pipe.get(), &overlapped);
        GetOverlappedResult(pipe.get(), &overlapped, &transferred, TRUE);
        break;
      }
    }
    got += transferred;
  }
  Print("got=" + std::to_string(got) + "\n");
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
    if (mode == L"apply-task" && argc == 7) return ApplyTask(argv[3], argv[4], argv[5], argv[6]);
    if (mode == L"make-folder" && argc == 4) return MakeFolder(argv[3]);
    if (mode == L"hold-file" && argc == 4) return HoldFile(argv[3]);
    if (mode == L"parent" && argc == 3) return Parent();
    if (mode == L"report" && argc >= 4) return Report(argc, argv);
    if (mode == L"pipe-squat" && argc == 4) return PipeSquat(argv[3]);
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
