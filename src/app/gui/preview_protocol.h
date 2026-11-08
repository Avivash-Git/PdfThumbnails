// What the preview child (--render-preview) sends the settings window over its standard output
// (pure): a header, then one frame per sample, each with its result and, when it worked, its
// pixels as top-down 32-bit BGRA. The window checks every size before it believes one.

#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pdft {

// The samples, in the order the child renders them.
enum class PreviewSample : uint32_t { kDocument = 0, kSlide = 1 };
inline constexpr uint32_t kPreviewSampleCount = 2;

struct PreviewFrame {
  PreviewSample sample = PreviewSample::kDocument;
  HRESULT hr = E_FAIL;
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint32_t> pixels;  // width * height when hr is S_OK, empty otherwise
};

// The bytes of the header, and of one frame. May throw std::bad_alloc.
std::string EncodePreviewHeader();
std::string EncodePreviewFrame(const PreviewFrame& frame);

// The most bytes the child can send for |edge|: what the window reads at most.
size_t PreviewOutputCap(uint32_t edge) noexcept;

// The frames in |bytes|, for a child asked for |edge| (pure). False, with *frames empty, for
// anything but a full header and exactly kPreviewSampleCount frames of the samples in order, each
// with S_OK and a size of 1 to |edge| on both sides, or a failure and no pixels. May throw
// std::bad_alloc.
bool DecodePreview(std::string_view bytes, uint32_t edge, std::vector<PreviewFrame>* frames);

}  // namespace pdft
