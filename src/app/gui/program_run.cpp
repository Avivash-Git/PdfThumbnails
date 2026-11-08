#include "program_run.h"

#include <wil/result_macros.h>

#include <algorithm>
#include <array>
#include <vector>

namespace pdft {
namespace {

std::wstring Utf8Lossy(std::string_view text) {
  if (text.empty()) return {};
  const int length =
      MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
  return wide;
}

}  // namespace

HRESULT RunningProgram::Start(const ProgramRequest& request,
                              std::unique_ptr<RunningProgram>* program) noexcept try {
  SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), nullptr, TRUE};
  wil::unique_hfile readEnd;
  wil::unique_hfile writeEnd;
  // Only the write end is inheritable: a child holding the read end would keep the pipe open.
  RETURN_IF_WIN32_BOOL_FALSE(CreatePipe(readEnd.put(), writeEnd.put(), nullptr, 0));
  RETURN_IF_WIN32_BOOL_FALSE(
      SetHandleInformation(writeEnd.get(), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT));
  wil::unique_hfile nul(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &inheritable, OPEN_EXISTING, 0, nullptr));
  RETURN_LAST_ERROR_IF(!nul);

  SIZE_T size = 0;
  (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
  std::vector<BYTE> storage(size);
  auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
  RETURN_IF_WIN32_BOOL_FALSE(InitializeProcThreadAttributeList(attributes, 1, 0, &size));
  auto deleteList = wil::scope_exit([attributes] { DeleteProcThreadAttributeList(attributes); });
  std::array<HANDLE, 2> inherited = {writeEnd.get(), nul.get()};
  RETURN_IF_WIN32_BOOL_FALSE(
      UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(),
                                sizeof(inherited), nullptr, nullptr));

  STARTUPINFOEXW startup = {};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = nul.get();
  startup.StartupInfo.hStdOutput = writeEnd.get();
  startup.StartupInfo.hStdError = writeEnd.get();
  startup.lpAttributeList = attributes;

  std::wstring commandLine = L"\"" + request.program.native() + L"\" " + request.arguments;
  PROCESS_INFORMATION started = {};
  RETURN_IF_WIN32_BOOL_FALSE(CreateProcessW(
      request.program.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
      EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
      request.folder.empty() ? nullptr : request.folder.c_str(), &startup.StartupInfo, &started));
  CloseHandle(started.hThread);

  auto result = std::unique_ptr<RunningProgram>(new RunningProgram());
  result->process_.reset(started.hProcess);
  result->output_ = std::move(readEnd);
  *program = std::move(result);
  return S_OK;
}
CATCH_RETURN();

void RunningProgram::Terminate() noexcept {
  if (process_) (void)TerminateProcess(process_.get(), ERROR_CANCELLED);
}

HRESULT RunningProgram::WaitForExit(DWORD timeoutMs, DWORD* exitCode) noexcept {
  const DWORD wait = WaitForSingleObject(process_.get(), timeoutMs);
  if (wait == WAIT_TIMEOUT) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  RETURN_LAST_ERROR_IF(wait != WAIT_OBJECT_0);
  RETURN_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(process_.get(), exitCode));
  return S_OK;
}

void OutputLines::Feed(std::string_view bytes, const LineFn& onLine) {
  partial_.append(bytes);
  size_t start = 0;
  for (size_t end; (end = partial_.find('\n', start)) != std::string::npos; start = end + 1) {
    std::string_view line(partial_.data() + start, end - start);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    onLine(Utf8Lossy(line));
  }
  partial_.erase(0, start);
}

void OutputLines::Finish(const LineFn& onLine) {
  if (!partial_.empty()) onLine(Utf8Lossy(partial_));
  partial_.clear();
}

HRESULT ReadProgramOutput(HANDLE output, size_t cap, const OutputLines::LineFn& onLine,
                          std::string* kept, bool* truncated) noexcept try {
  kept->clear();
  *truncated = false;
  OutputLines lines;
  std::array<char, 65536> buffer;
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(output, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      const DWORD error = GetLastError();
      if (error == ERROR_BROKEN_PIPE) break;  // the normal end
      return HRESULT_FROM_WIN32(error);
    }
    const std::string_view chunk(buffer.data(), read);
    const size_t room = cap - std::min(cap, kept->size());
    kept->append(chunk.substr(0, room));
    if (chunk.size() > room) *truncated = true;
    if (onLine) lines.Feed(chunk, onLine);
  }
  if (onLine) lines.Finish(onLine);
  return S_OK;
}
CATCH_RETURN();

}  // namespace pdft
