#include "child_process.h"

#include <algorithm>
#include <thread>
#include <vector>

#include "medium_il.h"

namespace pdft::test {
namespace {

HRESULT LastError() {
  const DWORD error = GetLastError();
  return error ? HRESULT_FROM_WIN32(error) : E_FAIL;
}

void Close(HANDLE* handle) {
  if (*handle) CloseHandle(*handle);
  *handle = nullptr;
}

// A pipe whose child end is inheritable and whose other end is not.
bool MakePipe(HANDLE* read, HANDLE* write, bool childReads) {
  SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), nullptr, TRUE};
  if (!CreatePipe(read, write, &inheritable, 1 << 16)) return false;
  SetHandleInformation(childReads ? *write : *read, HANDLE_FLAG_INHERIT, 0);
  return true;
}

// Starts |exe| with |in|, |out| and |err| as its standard handles, which are the only handles
// it inherits (null: none); with |token|, as that token.
HRESULT Spawn(const std::wstring& exe, const std::wstring& arguments, HANDLE in, HANDLE out,
              HANDLE err, PROCESS_INFORMATION* process, HANDLE token = nullptr) {
  std::vector<HANDLE> handles;
  for (HANDLE handle : {in, out, err}) {
    if (handle && std::find(handles.begin(), handles.end(), handle) == handles.end()) {
      handles.push_back(handle);
    }
  }
  SIZE_T size = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<BYTE> attributes(size);
  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
  if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) return LastError();
  HRESULT hr = S_OK;
  if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                 handles.data(), handles.size() * sizeof(HANDLE), nullptr,
                                 nullptr)) {
    hr = LastError();
  }
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = in;
  startup.StartupInfo.hStdOutput = out;
  startup.StartupInfo.hStdError = err;
  std::wstring commandLine = QuoteArgument(exe) + L" " + arguments;
  const DWORD flags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
  const BOOL started =
      SUCCEEDED(hr) &&
      (token ? CreateProcessAsUserW(token, exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                    flags, nullptr, nullptr, &startup.StartupInfo, process)
             : CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, TRUE, flags,
                              nullptr, nullptr, &startup.StartupInfo, process));
  if (SUCCEEDED(hr) && !started) {
    hr = LastError();
  }
  DeleteProcThreadAttributeList(startup.lpAttributeList);
  return hr;
}

void ReadAll(HANDLE pipe, std::string* text) {
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    text->append(buffer, read);
  }
}

}  // namespace

std::wstring SelfPath() {
  wchar_t path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::wstring(path, length);
}

std::wstring SelfFolder() {
  const std::wstring self = SelfPath();
  return self.substr(0, self.find_last_of(L'\\'));
}

std::wstring& NamedObjectSuffix() {
  static std::wstring suffix;
  return suffix;
}

CapturedRun RunCaptured(const std::wstring& exe, const std::wstring& arguments, DWORD timeoutMs,
                        HANDLE token) {
  CapturedRun run;
  HANDLE outRead = nullptr;
  HANDLE outWrite = nullptr;
  HANDLE errRead = nullptr;
  HANDLE errWrite = nullptr;
  if (!MakePipe(&outRead, &outWrite, false) || !MakePipe(&errRead, &errWrite, false)) {
    run.hr = LastError();
    for (HANDLE* handle : {&outRead, &outWrite, &errRead, &errWrite}) Close(handle);
    return run;
  }
  PROCESS_INFORMATION process = {};
  run.hr = Spawn(exe, arguments, nullptr, outWrite, errWrite, &process, token);
  // Only the child holds the write ends now: reading ends when it ends.
  Close(&outWrite);
  Close(&errWrite);
  if (FAILED(run.hr)) {
    Close(&outRead);
    Close(&errRead);
    return run;
  }
  CloseHandle(process.hThread);
  std::thread outReader(ReadAll, outRead, &run.out);
  std::thread errReader(ReadAll, errRead, &run.err);
  if (WaitForSingleObject(process.hProcess, timeoutMs) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, 10000);
    run.hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  } else if (!GetExitCodeProcess(process.hProcess, &run.exitCode)) {
    run.hr = LastError();
  }
  outReader.join();
  errReader.join();
  Close(&outRead);
  Close(&errRead);
  CloseHandle(process.hProcess);
  return run;
}

HRESULT RunSelfChild(const std::wstring& arguments, DWORD* processId, DWORD* exitCode) {
  std::wstring commandLine = QuoteArgument(SelfPath()) + L" --pdft-child " + arguments;
  STARTUPINFOW startup = {sizeof(startup)};
  PROCESS_INFORMATION process = {};
  if (!CreateProcessW(SelfPath().c_str(), commandLine.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
    return LastError();
  }
  CloseHandle(process.hThread);
  HRESULT hr = S_OK;
  if (WaitForSingleObject(process.hProcess, 30000) != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);
    hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  } else if (!GetExitCodeProcess(process.hProcess, exitCode)) {
    hr = LastError();
  }
  *processId = process.dwProcessId;
  CloseHandle(process.hProcess);
  return hr;
}

PipedChild::~PipedChild() {
  CloseInput();
  if (process_ && Wait(10000) == MAXDWORD) {
    TerminateProcess(process_, 1);
    WaitForSingleObject(process_, 10000);
  }
  Close(&process_);
  Close(&output_);
}

HRESULT PipedChild::Start(const std::wstring& exe, const std::wstring& arguments, HANDLE token) {
  HANDLE inRead = nullptr;
  HANDLE outWrite = nullptr;
  if (!MakePipe(&inRead, &input_, true) || !MakePipe(&output_, &outWrite, false)) {
    const HRESULT hr = LastError();
    Close(&inRead);
    Close(&outWrite);
    return hr;
  }
  PROCESS_INFORMATION process = {};
  const HRESULT hr = Spawn(exe, arguments, inRead, outWrite, outWrite, &process, token);
  Close(&inRead);
  Close(&outWrite);
  if (FAILED(hr)) return hr;
  CloseHandle(process.hThread);
  process_ = process.hProcess;
  return S_OK;
}

std::string PipedChild::ReadLine() {
  std::string line;
  char c = 0;
  DWORD read = 0;
  while (ReadFile(output_, &c, 1, &read, nullptr) && read == 1) {
    if (c == '\n') break;
    if (c != '\r') line += c;
  }
  return line;
}

void PipedChild::CloseInput() { Close(&input_); }

DWORD PipedChild::Wait(DWORD timeoutMs) {
  if (!process_ || WaitForSingleObject(process_, timeoutMs) != WAIT_OBJECT_0) return MAXDWORD;
  DWORD code = MAXDWORD;
  GetExitCodeProcess(process_, &code);
  return code;
}

}  // namespace pdft::test
