// The settings window's live preview: a thread that keeps only the latest request and renders it
// in a short-lived child process (preview_child.h), so a slider being dragged costs one child at a
// time and the handler never loads into the window. The result waits here; one bare
// kPreviewReadyMessage says so.

#pragma once

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include "preview_protocol.h"

namespace pdft {

inline constexpr UINT kPreviewReadyMessage = WM_APP + 3;

struct PreviewResult {
  int zoom = 100;
  int edge = 96;
  bool ok = false;  // the child ran and sent frames that add up
  std::vector<PreviewFrame> frames;
};

class PreviewRenderer {
 public:
  explicit PreviewRenderer(HWND window);
  // Ends a child still running and the thread.
  ~PreviewRenderer();
  PreviewRenderer(const PreviewRenderer&) = delete;
  PreviewRenderer& operator=(const PreviewRenderer&) = delete;

  // Replaces any request not started yet.
  void Request(int zoom, int edge);
  // The newest result; nothing when there is none since the last call.
  std::optional<PreviewResult> Take();

 private:
  struct Job {
    int zoom;
    int edge;
  };

  void Loop(std::stop_token stop);
  void Render(const Job& job, std::stop_token stop);

  HWND window_;
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::optional<Job> pending_;
  std::optional<PreviewResult> result_;
  bool blocked_ = false;  // Application Control refused a child: no more starts, each one notifies
  std::jthread thread_;   // last: it uses everything above
};

}  // namespace pdft
