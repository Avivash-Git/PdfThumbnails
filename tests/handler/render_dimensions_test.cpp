// What the handler renders: result and bitmap size for every test PDF at several zoom levels
// (the 1.1 crop table, checked through the DLL), the edge cap and the 1-pixel minimum, and the
// pixels that prove orientation, CropBox, first page, annotations and the crop direction.

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <tuple>

#include "handler_env.h"

namespace pdft::test {
namespace {

struct Size {
  int w, h;
};

// Expected result at edge 256 for zoom 100, 120, 141, 142 and 200. The values follow 1.1's crop
// formula: the visible part of the longer side is longer*100/zoom, but never less than the
// shorter side; 141 is still just short of a square on A4, 142 is a square.
struct Case {
  const wchar_t* prefix;
  HRESULT hr;
  Size size[5];
};

constexpr int kZooms[] = {100, 120, 141, 142, 200};

#define PORTRAIT \
  S_OK, { {181, 256}, {217, 256}, {255, 256}, {256, 256}, {256, 256} }
#define LANDSCAPE \
  S_OK, { {256, 181}, {256, 217}, {256, 255}, {256, 256}, {256, 256} }
#define FAILS \
  WTS_E_FAILEDEXTRACTION, {}

const Case kCases[] = {
    {L"01-", PORTRAIT},
    {L"02-", LANDSCAPE},
    {L"03-", LANDSCAPE},  // /Rotate 90 of a portrait page
    {L"04-", LANDSCAPE},  // CropBox: the top half of A4, 595 x 421 pt
    {L"05-", PORTRAIT},
    // 30 x 3000 pt: the narrow side stays a few pixels wide.
    {L"06-", S_OK, {{3, 256}, {3, 256}, {4, 256}, {4, 256}, {5, 256}}},
    {L"07-", PORTRAIT},
    {L"08-", PORTRAIT},
    {L"09-", S_OK, {{256, 256}, {256, 256}, {256, 256}, {256, 256}, {256, 256}}},
    // MediaBox [0 0 0 0]: PDFium falls back to Letter, 612 x 792 pt.
    {L"10-", S_OK, {{198, 256}, {237, 256}, {256, 256}, {256, 256}, {256, 256}}},
    {L"11-", FAILS},  // no pages
    {L"12-", FAILS},  // 0 bytes
    {L"13-", FAILS},  // random bytes
    {L"14-", FAILS},  // text file
    {L"15-", FAILS},  // truncated
    {L"16-", PORTRAIT},
    {L"17-", FAILS},  // dynamic XFA keeps the icon
    {L"18-", PORTRAIT},
    {L"sample-document", PORTRAIT},
    {L"sample-slide", S_OK, {{256, 144}, {256, 173}, {256, 203}, {256, 204}, {256, 256}}},
};

#undef PORTRAIT
#undef LANDSCAPE
#undef FAILS

// 08 (a 5 MB page) and 09 (a 14400 pt page) are the slow ones: on a slow machine they may hit
// the 8 s render budget, which is the documented outcome, not a bug.
bool IsSlowCase(const wchar_t* prefix) {
  return std::wstring(prefix) == L"08-" || std::wstring(prefix) == L"09-";
}

class RenderDimensions : public ::testing::TestWithParam<std::tuple<size_t, size_t>> {};

TEST_P(RenderDimensions, MatchesTheCropTable) {
  const Case& c = kCases[std::get<0>(GetParam())];
  const size_t zoomIndex = std::get<1>(GetParam());
  ScopedZoom zoom(kZooms[zoomIndex]);
  const Rendered r = HandlerEnvironment::Render(c.prefix);
  if (IsSlowCase(c.prefix)) {
    std::printf("[ info     ] %s at zoom %d: %s in %.0f ms\n", Printable(c.prefix).c_str(),
                kZooms[zoomIndex], HresultText(r.hr).c_str(), r.ms);
    if (r.hr == WTS_E_EXTRACTIONTIMEDOUT) {
      GTEST_SKIP() << "hit the render budget on this machine (" << r.ms << " ms)";
    }
  }
  ASSERT_EQ(HresultText(r.hr), HresultText(c.hr));
  if (FAILED(c.hr)) return;
  EXPECT_EQ(r.width, c.size[zoomIndex].w);
  EXPECT_EQ(r.height, c.size[zoomIndex].h);
  EXPECT_EQ(r.alpha, WTSAT_RGB);
  EXPECT_TRUE(r.AllOpaque());
}

std::string CaseName(const ::testing::TestParamInfo<std::tuple<size_t, size_t>>& info) {
  std::string name;
  for (const wchar_t* p = kCases[std::get<0>(info.param)].prefix; *p; ++p) {
    if ((*p >= L'0' && *p <= L'9') || (*p >= L'a' && *p <= L'z')) name += static_cast<char>(*p);
  }
  return "f" + name + "_zoom" + std::to_string(kZooms[std::get<1>(info.param)]);
}

INSTANTIATE_TEST_SUITE_P(AllFiles, RenderDimensions,
                         ::testing::Combine(::testing::Range<size_t>(0, std::size(kCases)),
                                            ::testing::Range<size_t>(0, std::size(kZooms))),
                         CaseName);

// ---- Edge size --------------------------------------------------------------------------------

TEST(RenderEdge, CappedAt2560Pixels) {
  for (UINT cx : {2560u, 2561u, 4000u}) {
    const Rendered r = HandlerEnvironment::Render(L"01-", cx);
    ASSERT_EQ(HresultText(r.hr), "0x00000000") << cx;
    EXPECT_EQ(r.width, 1809) << cx;
    EXPECT_EQ(r.height, 2560) << cx;
  }
}

TEST(RenderEdge, SmallEdges) {
  Rendered r = HandlerEnvironment::Render(L"01-", 32);
  ASSERT_EQ(r.hr, S_OK);
  EXPECT_EQ(r.width, 23);
  EXPECT_EQ(r.height, 32);
  r = HandlerEnvironment::Render(L"02-", 1);
  ASSERT_EQ(r.hr, S_OK);
  EXPECT_EQ(r.width, 1);
  EXPECT_EQ(r.height, 1);
}

TEST(RenderEdge, NarrowSideNeverBelowOnePixel) {
  const Rendered r = HandlerEnvironment::Render(L"06-", 16);  // 30 pt * 16/3000 = 0.16 px
  ASSERT_EQ(r.hr, S_OK);
  EXPECT_EQ(r.width, 1);
  EXPECT_EQ(r.height, 16);
}

// ---- Pixels -----------------------------------------------------------------------------------

struct Rgb {
  int r, g, b;
};
constexpr Rgb kRed = {255, 0, 0};
constexpr Rgb kGreen = {0, 153, 0};  // 0 0.6 0 rg
constexpr Rgb kBlue = {0, 0, 255};
constexpr Rgb kWhite = {255, 255, 255};

Rgb ColorAt(const Rendered& r, int x, int y) {
  const uint32_t p = r.At(x, y);
  return {static_cast<int>((p >> 16) & 0xFF), static_cast<int>((p >> 8) & 0xFF),
          static_cast<int>(p & 0xFF)};
}

::testing::AssertionResult ColorNear(const Rendered& r, int x, int y, Rgb want,
                                     int tolerance = 24) {
  if (x < 0 || y < 0 || x >= r.width || y >= r.height) {
    return ::testing::AssertionFailure()
           << "(" << x << "," << y << ") is outside " << r.width << "x" << r.height;
  }
  const Rgb got = ColorAt(r, x, y);
  if (std::abs(got.r - want.r) <= tolerance && std::abs(got.g - want.g) <= tolerance &&
      std::abs(got.b - want.b) <= tolerance) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "pixel (" << x << "," << y << ") is " << got.r << "," << got.g << "," << got.b
         << ", expected about " << want.r << "," << want.g << "," << want.b;
}

// Pixels close to pure red / green / blue: the test card's squares.
int CountNear(const Rendered& r, Rgb want) {
  int count = 0;
  for (int y = 0; y < r.height; ++y) {
    for (int x = 0; x < r.width; ++x) {
      const Rgb got = ColorAt(r, x, y);
      if (std::abs(got.r - want.r) <= 40 && std::abs(got.g - want.g) <= 40 &&
          std::abs(got.b - want.b) <= 40) {
        ++count;
      }
    }
  }
  return count;
}

// The test card's squares are 60 pt, 20 pt from the edges: their centres sit 50 pt in.
// |x|, |y| in points from the top-left of the shown page, scaled to the whole-page bitmap.
int Px(double points, double pagePoints, int pixels) {
  return static_cast<int>(points * pixels / pagePoints);
}

TEST(RenderPixels, PortraitCardCorners) {
  for (const wchar_t* prefix : {L"01-", L"16-", L"18-"}) {
    const Rendered r = HandlerEnvironment::Render(prefix);
    ASSERT_EQ(r.hr, S_OK) << Printable(prefix);
    const int left = Px(50, 595, r.width), right = Px(545, 595, r.width);
    const int top = Px(50, 842, r.height), bottom = Px(792, 842, r.height);
    EXPECT_TRUE(ColorNear(r, left, top, kRed)) << Printable(prefix);
    EXPECT_TRUE(ColorNear(r, right, top, kGreen)) << Printable(prefix);
    EXPECT_TRUE(ColorNear(r, left, bottom, kBlue)) << Printable(prefix);
    EXPECT_TRUE(ColorNear(r, right, bottom, kWhite)) << Printable(prefix);
  }
}

TEST(RenderPixels, LandscapeCardCorners) {
  const Rendered r = HandlerEnvironment::Render(L"02-");
  ASSERT_EQ(r.hr, S_OK);
  const int left = Px(50, 842, r.width), right = Px(792, 842, r.width);
  const int top = Px(50, 595, r.height), bottom = Px(545, 595, r.height);
  EXPECT_TRUE(ColorNear(r, left, top, kRed));
  EXPECT_TRUE(ColorNear(r, right, top, kGreen));
  EXPECT_TRUE(ColorNear(r, left, bottom, kBlue));
  EXPECT_TRUE(ColorNear(r, right, bottom, kWhite));
}

// /Rotate 90 turns the page clockwise: red top-right, green bottom-right, blue top-left.
TEST(RenderPixels, Rotate90TurnsClockwise) {
  const Rendered r = HandlerEnvironment::Render(L"03-");
  ASSERT_EQ(r.hr, S_OK);
  const int left = Px(50, 842, r.width), right = Px(792, 842, r.width);
  const int top = Px(50, 595, r.height), bottom = Px(545, 595, r.height);
  EXPECT_TRUE(ColorNear(r, right, top, kRed));
  EXPECT_TRUE(ColorNear(r, right, bottom, kGreen));
  EXPECT_TRUE(ColorNear(r, left, top, kBlue));
  EXPECT_TRUE(ColorNear(r, left, bottom, kWhite));
}

// The CropBox is the top half: red and green squares, but no blue square at the bottom.
TEST(RenderPixels, CropBoxShowsTheTopHalf) {
  const Rendered r = HandlerEnvironment::Render(L"04-");
  ASSERT_EQ(r.hr, S_OK);
  const int left = Px(50, 595, r.width), right = Px(545, 595, r.width);
  const int top = Px(50, 421, r.height), bottom = Px(371, 421, r.height);
  EXPECT_TRUE(ColorNear(r, left, top, kRed));
  EXPECT_TRUE(ColorNear(r, right, top, kGreen));
  EXPECT_TRUE(ColorNear(r, left, bottom, kWhite));
  EXPECT_EQ(CountNear(r, kBlue), 0);
}

TEST(RenderPixels, FirstPageOfAMultipageFile) {
  const Rendered r = HandlerEnvironment::Render(L"05-");
  ASSERT_EQ(r.hr, S_OK);
  EXPECT_TRUE(ColorNear(r, Px(50, 595, r.width), Px(50, 842, r.height), {0, 178, 0}));
  EXPECT_EQ(CountNear(r, kRed), 0) << "page two is red";
}

TEST(RenderPixels, AnnotationAppearanceIsDrawn) {
  const Rendered r = HandlerEnvironment::Render(L"07-");
  ASSERT_EQ(r.hr, S_OK);
  // The square annotation covers 150..450 pt both ways: the page centre is orange.
  EXPECT_TRUE(ColorNear(r, r.width / 2, r.height / 2, {255, 128, 0}));
}

TEST(RenderPixels, TallStripBands) {
  const Rendered r = HandlerEnvironment::Render(L"06-");
  ASSERT_EQ(r.hr, S_OK);
  EXPECT_TRUE(ColorNear(r, r.width / 2, 1, {255, 102, 0}));             // top band: 1 0.4 0
  EXPECT_TRUE(ColorNear(r, r.width / 2, r.height - 2, {0, 102, 255}));  // bottom: 0 0.4 1
}

TEST(RenderPixels, ZeroSizePageIsBlankPaper) {
  const Rendered r = HandlerEnvironment::Render(L"10-");
  ASSERT_EQ(r.hr, S_OK);
  for (uint32_t pixel : r.pixels) ASSERT_EQ(pixel, 0xFFFFFFFFu);
}

// Zoom crops the longer side evenly: at 200 % an A4 page becomes a square cut from its middle,
// so the corner squares (top and bottom) are gone while the frame's sides stay.
TEST(RenderPixels, ZoomCropsTheLongerSideFromBothEnds) {
  ScopedZoom zoom(200);
  const Rendered portrait = HandlerEnvironment::Render(L"01-");
  ASSERT_EQ(portrait.hr, S_OK);
  ASSERT_EQ(portrait.width, 256);
  EXPECT_EQ(CountNear(portrait, kRed), 0);
  EXPECT_EQ(CountNear(portrait, kGreen), 0);
  EXPECT_EQ(CountNear(portrait, kBlue), 0);
  // The frame's left and right edges (10 pt in, 3 pt wide) are still visible mid-height.
  const int mid = portrait.height / 2;
  EXPECT_LT(ColorAt(portrait, Px(10, 595, 256), mid).r, 128);
  EXPECT_LT(ColorAt(portrait, Px(585, 595, 256), mid).r, 128);

  const Rendered landscape = HandlerEnvironment::Render(L"02-");
  ASSERT_EQ(landscape.hr, S_OK);
  EXPECT_EQ(CountNear(landscape, kRed), 0);
  EXPECT_EQ(CountNear(landscape, kGreen), 0);
  EXPECT_EQ(CountNear(landscape, kBlue), 0);
  const int centre = landscape.width / 2;
  EXPECT_LT(ColorAt(landscape, centre, Px(10, 595, 256)).r, 128);
  EXPECT_LT(ColorAt(landscape, centre, Px(585, 595, 256)).r, 128);
}

}  // namespace
}  // namespace pdft::test
