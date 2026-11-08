#include "preview_renderer.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include "paths.h"
#include "program_run.h"
#include "sac.h"

namespace pdft {
namespace {

// A drag sends many requests: only the one still there after this is rendered.
constexpr auto kDebounce = std::chrono::milliseconds(100);
// A child that takes longer than this is stuck; the tile keeps what it has.
constexpr auto kChildLimit = std::chrono::seconds(10);

}  // namespace

PreviewRenderer::PreviewRenderer(HWND window)
    : window_(window), thread_([this](std::stop_token stop) { Loop(std::move(stop)); }) {}

PreviewRenderer::~PreviewRenderer() {
  thread_.request_stop();
  thread_.join();
}

void PreviewRenderer::Request(int zoom, int edge) {
  {
    const std::lock_guard lock(mutex_);
    if (blocked_) return;
    pending_ = Job{zoom, edge};
  }
  wake_.notify_all();
}

std::optional<PreviewResult> PreviewRenderer::Take() {
  const std::lock_guard lock(mutex_);
  return std::exchange(result_, std::nullopt);
}

void PreviewRenderer::Loop(std::stop_token stop) {
  for (;;) {
    Job job{};
    {
      std::unique_lock lock(mutex_);
      if (!wake_.wait(lock, stop, [this] { return pending_.has_value(); })) return;
      (void)wake_.wait_for(lock, stop, kDebounce, [] { return false; });
      if (stop.stop_requested()) return;
      job = *pending_;
      pending_.reset();
    }
    try {
      Render(job, stop);
    } catch (...) {
      // Out of memory: this preview is lost, the tile keeps what it has.
    }
  }
}

void PreviewRenderer::Render(const Job& job, std::stop_token stop) {
  PreviewResult result;
  result.zoom = job.zoom;
  result.edge = job.edge;

  ProgramRequest request;
  std::unique_ptr<RunningProgram> program;
  HRESULT hr = GetOwnExe(&request.program);
  if (SUCCEEDED(hr)) {
    request.arguments =
        L"--render-preview " + std::to_wstring(job.zoom) + L"," + std::to_wstring(job.edge);
    hr = RunningProgram::Start(request, &program);
  }
  if (FAILED(hr)) {
    const std::lock_guard lock(mutex_);
    blocked_ = IsLoadBlockedHresult(hr);
  } else {
    // Whatever ends the child ends its output, and with it the read below.
    const std::stop_callback onStop(stop, [&program] { program->Terminate(); });
    std::jthread watchdog([&program](std::stop_token finished) {
      std::mutex mutex;
      std::condition_variable_any never;
      std::unique_lock lock(mutex);
      (void)never.wait_for(lock, finished, kChildLimit, [] { return false; });
      if (!finished.stop_requested()) program->Terminate();
    });
    std::string bytes;
    bool truncated = false;
    const HRESULT read =
        ReadProgramOutput(program->output(), PreviewOutputCap(static_cast<uint32_t>(job.edge)), {},
                          &bytes, &truncated);
    watchdog.request_stop();
    watchdog.join();
    DWORD code = 1;
    result.ok = SUCCEEDED(read) && !truncated && SUCCEEDED(program->WaitForExit(2000, &code)) &&
                code == 0 && DecodePreview(bytes, static_cast<uint32_t>(job.edge), &result.frames);
  }
  {
    const std::lock_guard lock(mutex_);
    result_ = std::move(result);
  }
  PostMessageW(window_, kPreviewReadyMessage, 0, 0);
}

}  // namespace pdft
