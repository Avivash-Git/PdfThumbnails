#include "preview_protocol.h"

#include <cstring>
#include <optional>

namespace pdft {
namespace {

constexpr uint32_t kMagic = 0x56504450;  // "PDPV"
constexpr uint32_t kVersion = 1;

struct Header {
  uint32_t magic;
  uint32_t version;
  uint32_t count;
};

struct FrameHeader {
  uint32_t sample;
  int32_t hr;
  uint32_t width;
  uint32_t height;
  uint32_t byteCount;
};

// Reads a T from the front of |bytes| and moves past it.
template <typename T>
std::optional<T> Take(std::string_view* bytes) noexcept {
  if (bytes->size() < sizeof(T)) return std::nullopt;
  T value;
  std::memcpy(&value, bytes->data(), sizeof(T));
  bytes->remove_prefix(sizeof(T));
  return value;
}

}  // namespace

std::string EncodePreviewHeader() {
  const Header header{kMagic, kVersion, kPreviewSampleCount};
  return std::string(reinterpret_cast<const char*>(&header), sizeof(header));
}

std::string EncodePreviewFrame(const PreviewFrame& frame) {
  const uint32_t byteCount = static_cast<uint32_t>(frame.pixels.size() * sizeof(uint32_t));
  const FrameHeader header{static_cast<uint32_t>(frame.sample), frame.hr, frame.width, frame.height,
                           byteCount};
  std::string bytes(reinterpret_cast<const char*>(&header), sizeof(header));
  bytes.append(reinterpret_cast<const char*>(frame.pixels.data()), byteCount);
  return bytes;
}

size_t PreviewOutputCap(uint32_t edge) noexcept {
  return sizeof(Header) + kPreviewSampleCount * (sizeof(FrameHeader) + size_t{edge} * edge * 4);
}

bool DecodePreview(std::string_view bytes, uint32_t edge, std::vector<PreviewFrame>* frames) {
  frames->clear();
  const std::optional<Header> header = Take<Header>(&bytes);
  if (!header || header->magic != kMagic || header->version != kVersion ||
      header->count != kPreviewSampleCount) {
    return false;
  }
  std::vector<PreviewFrame> decoded;
  for (uint32_t index = 0; index < kPreviewSampleCount; ++index) {
    const std::optional<FrameHeader> at = Take<FrameHeader>(&bytes);
    if (!at || at->sample != index) return false;
    PreviewFrame frame;
    frame.sample = static_cast<PreviewSample>(at->sample);
    frame.hr = at->hr;
    if (at->hr == S_OK) {
      if (at->width < 1 || at->width > edge || at->height < 1 || at->height > edge) return false;
      const size_t expected = size_t{at->width} * at->height * 4;
      if (at->byteCount != expected || bytes.size() < expected) return false;
      frame.width = at->width;
      frame.height = at->height;
      frame.pixels.resize(expected / 4);
      std::memcpy(frame.pixels.data(), bytes.data(), expected);
      bytes.remove_prefix(expected);
    } else if (SUCCEEDED(at->hr) || at->width != 0 || at->height != 0 || at->byteCount != 0) {
      return false;
    }
    decoded.push_back(std::move(frame));
  }
  if (!bytes.empty()) return false;
  *frames = std::move(decoded);
  return true;
}

}  // namespace pdft
