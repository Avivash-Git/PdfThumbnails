// What the preview child sends the settings window (preview_protocol.h): the window believes
// nothing whose sizes do not add up.

#include "preview_protocol.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pdft {
namespace {

PreviewFrame Rendered(PreviewSample sample, uint32_t width, uint32_t height) {
  PreviewFrame frame;
  frame.sample = sample;
  frame.hr = S_OK;
  frame.width = width;
  frame.height = height;
  frame.pixels.assign(size_t{width} * height, 0xFF102030);
  return frame;
}

PreviewFrame Failed(PreviewSample sample, HRESULT hr) {
  PreviewFrame frame;
  frame.sample = sample;
  frame.hr = hr;
  return frame;
}

std::string Both(const PreviewFrame& document, const PreviewFrame& slide) {
  return EncodePreviewHeader() + EncodePreviewFrame(document) + EncodePreviewFrame(slide);
}

const PreviewFrame kDocument = Rendered(PreviewSample::kDocument, 3, 4);
const PreviewFrame kSlide = Rendered(PreviewSample::kSlide, 4, 2);

TEST(PreviewProtocol, FramesComeBackAsTheyWere) {
  std::vector<PreviewFrame> frames;
  ASSERT_TRUE(DecodePreview(Both(kDocument, kSlide), 4, &frames));
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].sample, PreviewSample::kDocument);
  EXPECT_EQ(frames[0].width, 3u);
  EXPECT_EQ(frames[0].height, 4u);
  EXPECT_EQ(frames[0].pixels, kDocument.pixels);
  EXPECT_EQ(frames[1].sample, PreviewSample::kSlide);
  EXPECT_EQ(frames[1].pixels.size(), 8u);
}

TEST(PreviewProtocol, ASampleMayHaveFailed) {
  std::vector<PreviewFrame> frames;
  ASSERT_TRUE(
      DecodePreview(Both(kDocument, Failed(PreviewSample::kSlide, E_NOINTERFACE)), 4, &frames));
  EXPECT_EQ(frames[1].hr, E_NOINTERFACE);
  EXPECT_TRUE(frames[1].pixels.empty());
}

TEST(PreviewProtocol, RefusesWhatDoesNotAddUp) {
  std::vector<PreviewFrame> frames;
  const std::string good = Both(kDocument, kSlide);
  EXPECT_FALSE(DecodePreview("", 4, &frames));
  EXPECT_FALSE(DecodePreview(good.substr(0, good.size() - 1), 4, &frames));
  EXPECT_FALSE(DecodePreview(good + "x", 4, &frames));
  EXPECT_FALSE(DecodePreview("X" + good.substr(1), 4, &frames));
  // Larger than asked for.
  EXPECT_FALSE(DecodePreview(good, 3, &frames));
  // The samples out of order.
  EXPECT_FALSE(DecodePreview(Both(kSlide, kDocument), 4, &frames));
  EXPECT_TRUE(frames.empty());
}

TEST(PreviewProtocol, ACapHoldsTheLargestAnswer) {
  const PreviewFrame document = Rendered(PreviewSample::kDocument, 16, 16);
  const PreviewFrame slide = Rendered(PreviewSample::kSlide, 16, 16);
  EXPECT_EQ(Both(document, slide).size(), PreviewOutputCap(16));
}

}  // namespace
}  // namespace pdft
