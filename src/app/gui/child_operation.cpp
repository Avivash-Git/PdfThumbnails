#include "child_operation.h"

#include <filesystem>
#include <memory>
#include <utility>

#include "paths.h"
#include "program_run.h"

namespace pdft {
namespace {

// What is shown of a child's output; it is read to the end either way.
constexpr size_t kOutputCap = 64 * 1024;
// How long a child that closed its output gets to end.
constexpr DWORD kExitWaitMs = 10000;

}  // namespace

ChildOperation::~ChildOperation() {
  Abandon();
  if (thread_.joinable()) thread_.join();
}

void ChildOperation::Start(std::wstring arguments) {
  thread_ = std::thread([this, arguments = std::move(arguments)]() mutable {
    try {
      Run(std::move(arguments));
    } catch (...) {
      const std::lock_guard lock(mutex_);
      if (!result_) {
        result_ = ChildResult{};
        result_->hr = E_OUTOFMEMORY;
      }
    }
    if (!abandoned_) PostMessageW(window_, kChildDoneMessage, 0, 0);
  });
}

void ChildOperation::Run(std::wstring arguments) {
  ChildResult result;
  ProgramRequest request;
  request.arguments = std::move(arguments);
  wchar_t system32[MAX_PATH] = {};
  if (const UINT length = GetSystemDirectoryW(system32, ARRAYSIZE(system32));
      length > 0 && length < ARRAYSIZE(system32)) {
    request.folder = system32;
  }
  std::unique_ptr<RunningProgram> program;
  result.hr = GetOwnExe(&request.program);
  if (SUCCEEDED(result.hr)) result.hr = RunningProgram::Start(request, &program);
  if (SUCCEEDED(result.hr)) {
    std::wstring shown;
    std::string kept;
    bool truncated = false;
    const HRESULT read = ReadProgramOutput(
        program->output(), kOutputCap,
        [this, &shown](std::wstring_view line) {
          if (shown.size() < kOutputCap) {
            if (!shown.empty()) shown += L'\n';
            shown += line;
          }
          {
            const std::lock_guard lock(mutex_);
            line_ = std::wstring(line);
          }
          if (!abandoned_ && !linePosted_.exchange(true)) {
            PostMessageW(window_, kChildLineMessage, 0, 0);
          }
        },
        &kept, &truncated);
    if (FAILED(read) && !abandoned_) result.hr = read;
    result.output = std::move(shown);
    if (!abandoned_ && SUCCEEDED(result.hr)) {
      result.hr = program->WaitForExit(kExitWaitMs, &result.exitCode);
    }
  }
  const std::lock_guard lock(mutex_);
  result_ = std::move(result);
}

std::optional<std::wstring> ChildOperation::TakeLine() {
  linePosted_ = false;
  const std::lock_guard lock(mutex_);
  return std::exchange(line_, std::nullopt);
}

std::optional<ChildResult> ChildOperation::TakeResult() {
  if (thread_.joinable()) thread_.join();
  const std::lock_guard lock(mutex_);
  return std::exchange(result_, std::nullopt);
}

void ChildOperation::Abandon() noexcept {
  abandoned_ = true;
  if (!thread_.joinable()) return;
  // The thread may be between reads when the first cancel comes.
  while (WaitForSingleObject(thread_.native_handle(), 50) == WAIT_TIMEOUT) {
    (void)CancelSynchronousIo(thread_.native_handle());
  }
}

}  // namespace pdft
