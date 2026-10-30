// ComputeCrop: the 1.1 dimension table without PDFium. The expected numbers are worked out by
// hand from the formula (visible = longer side * 100 / zoom, but at least the shorter side;
// scale = edge / visible; round half up; at least 1 px; bitmap = page capped at the edge; offset
// = (bitmap - page) / 2, truncated toward zero), and the bitmap sizes agree with what the 1.1
// DLL renders and with the handler tests (render_dimensions_test.cpp).

#include "crop_math.h"

#include <gtest/gtest.h>

#include <climits>

namespace pdft::test {
namespace {

// It is constexpr: the preview and the tests may evaluate it at compile time.
static_assert(ComputeCrop(595, 842, 256, 100).bmpW == 181);
static_assert(ComputeCrop(595, 842, 256, 120).pageTop == -25);
static_assert(noexcept(ComputeCrop(1, 1, 1, 100)));

::testing::AssertionResult Same(const CropGeometry& got, const CropGeometry& want) {
  if (got.bmpW == want.bmpW && got.bmpH == want.bmpH && got.pageLeft == want.pageLeft &&
      got.pageTop == want.pageTop && got.pageW == want.pageW && got.pageH == want.pageH) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
         << "bitmap " << got.bmpW << "x" << got.bmpH << ", page " << got.pageW << "x" << got.pageH
         << " at (" << got.pageLeft << "," << got.pageTop << "); expected bitmap " << want.bmpW
         << "x" << want.bmpH << ", page " << want.pageW << "x" << want.pageH << " at ("
         << want.pageLeft << "," << want.pageTop << ")";
}

struct Case {
  const char* what;
  double widthPt, heightPt;
  unsigned edge;
  int zoom;
  CropGeometry want;  // bmpW, bmpH, pageLeft, pageTop, pageW, pageH
};

// Edge 256 at zoom 100, 120, 141, 142 and 200, as in the handler's crop table.
const Case kTable[] = {
    // A4 portrait: 141 is still just short of a square, 142 is one (842 * 100 / 142 < 595).
    {"A4", 595, 842, 256, 100, {181, 256, 0, 0, 181, 256}},
    {"A4", 595, 842, 256, 120, {217, 256, 0, -25, 217, 307}},
    {"A4", 595, 842, 256, 141, {255, 256, 0, -52, 255, 361}},
    {"A4", 595, 842, 256, 142, {256, 256, 0, -53, 256, 362}},
    {"A4", 595, 842, 256, 200, {256, 256, 0, -53, 256, 362}},
    // A4 landscape: the same numbers, mirrored.
    {"A4 landscape", 842, 595, 256, 100, {256, 181, 0, 0, 256, 181}},
    {"A4 landscape", 842, 595, 256, 120, {256, 217, -25, 0, 307, 217}},
    {"A4 landscape", 842, 595, 256, 141, {256, 255, -52, 0, 361, 255}},
    {"A4 landscape", 842, 595, 256, 142, {256, 256, -53, 0, 362, 256}},
    {"A4 landscape", 842, 595, 256, 200, {256, 256, -53, 0, 362, 256}},
    // 30 x 3000 pt strip: the narrow side stays a few pixels wide, and zoom crops up to 200.
    {"strip", 30, 3000, 256, 100, {3, 256, 0, 0, 3, 256}},
    {"strip", 30, 3000, 256, 120, {3, 256, 0, -25, 3, 307}},
    {"strip", 30, 3000, 256, 141, {4, 256, 0, -52, 4, 361}},
    {"strip", 30, 3000, 256, 142, {4, 256, 0, -54, 4, 364}},
    {"strip", 30, 3000, 256, 200, {5, 256, 0, -128, 5, 512}},
    // Letter (also what PDFium reports for a MediaBox of [0 0 0 0]): a square from 130 on.
    {"Letter", 612, 792, 256, 100, {198, 256, 0, 0, 198, 256}},
    {"Letter", 612, 792, 256, 120, {237, 256, 0, -25, 237, 307}},
    {"Letter", 612, 792, 256, 141, {256, 256, 0, -37, 256, 331}},
    {"Letter", 612, 792, 256, 142, {256, 256, 0, -37, 256, 331}},
    {"Letter", 612, 792, 256, 200, {256, 256, 0, -37, 256, 331}},
    // The top half of A4 (04-cropbox), 595 x 421 pt.
    {"half A4", 595, 421, 256, 100, {256, 181, 0, 0, 256, 181}},
    {"half A4", 595, 421, 256, 120, {256, 217, -25, 0, 307, 217}},
    {"half A4", 595, 421, 256, 141, {256, 255, -52, 0, 361, 255}},
    {"half A4", 595, 421, 256, 142, {256, 256, -53, 0, 362, 256}},
    {"half A4", 595, 421, 256, 200, {256, 256, -53, 0, 362, 256}},
    // 16:9 slide (sample-slide), 960 x 540 pt: a square only from 178 on.
    {"slide", 960, 540, 256, 100, {256, 144, 0, 0, 256, 144}},
    {"slide", 960, 540, 256, 120, {256, 173, -25, 0, 307, 173}},
    {"slide", 960, 540, 256, 141, {256, 203, -52, 0, 361, 203}},
    {"slide", 960, 540, 256, 142, {256, 204, -54, 0, 364, 204}},
    {"slide", 960, 540, 256, 200, {256, 256, -99, 0, 455, 256}},
    // 14400 pt square (09-max-page): nothing to crop.
    {"square", 14400, 14400, 256, 100, {256, 256, 0, 0, 256, 256}},
    {"square", 14400, 14400, 256, 200, {256, 256, 0, 0, 256, 256}},
};

TEST(CropMath, TheCropTableAtEdge256) {
  for (const Case& c : kTable) {
    EXPECT_TRUE(Same(ComputeCrop(c.widthPt, c.heightPt, c.edge, c.zoom), c.want))
        << c.what << " at zoom " << c.zoom;
  }
}

TEST(CropMath, EdgeIsCappedAt2560) {
  for (unsigned edge : {2560u, 2561u, 4000u, UINT_MAX}) {
    EXPECT_TRUE(Same(ComputeCrop(595, 842, edge, 100), {1809, 2560, 0, 0, 1809, 2560})) << edge;
    EXPECT_TRUE(Same(ComputeCrop(595, 842, edge, 120), {2171, 2560, 0, -256, 2171, 3072})) << edge;
  }
  EXPECT_EQ(kMaxEdge, 2560u);
}

TEST(CropMath, SmallEdges) {
  EXPECT_TRUE(Same(ComputeCrop(595, 842, 32, 100), {23, 32, 0, 0, 23, 32}));
  EXPECT_TRUE(Same(ComputeCrop(842, 595, 1, 100), {1, 1, 0, 0, 1, 1}));
  EXPECT_TRUE(Same(ComputeCrop(595, 842, 1, 200), {1, 1, 0, 0, 1, 1}));
}

// A side that would round to 0 px is drawn 1 px wide.
TEST(CropMath, NeverBelowOnePixel) {
  EXPECT_TRUE(Same(ComputeCrop(30, 3000, 16, 100), {1, 16, 0, 0, 1, 16}));  // 0.16 px wide
  EXPECT_TRUE(Same(ComputeCrop(1, 999999, 256, 100), {1, 256, 0, 0, 1, 256}));
  EXPECT_TRUE(Same(ComputeCrop(1, 999999, 256, 200), {1, 256, 0, -128, 1, 512}));
  EXPECT_TRUE(Same(ComputeCrop(999999, 1, 256, 200), {256, 1, -128, 0, 512, 1}));
}

// Edge 0 is no image at all; GetThumbnail refuses cx == 0 before it gets here.
TEST(CropMath, EdgeZero) {
  EXPECT_TRUE(Same(ComputeCrop(595, 842, 0, 100), {0, 0, 0, 0, 1, 1}));
  EXPECT_TRUE(Same(ComputeCrop(595, 842, 0, 200), {0, 0, 0, 0, 1, 1}));
}

// The overhang is split with C++ integer division: an odd overhang leaves the extra pixel at
// the far end (bottom or right), never at the start.
TEST(CropMath, OffsetsTruncateTowardZero) {
  EXPECT_EQ(ComputeCrop(595, 842, 256, 120).pageTop, -25);   // overhang 51, not -26
  EXPECT_EQ(ComputeCrop(842, 595, 256, 120).pageLeft, -25);  // the same, mirrored
  EXPECT_EQ(ComputeCrop(612, 792, 256, 141).pageTop, -37);   // overhang 75, not -38
  EXPECT_EQ(ComputeCrop(960, 540, 256, 200).pageLeft, -99);  // overhang 199, not -100
  EXPECT_EQ(ComputeCrop(30, 3000, 256, 200).pageTop, -128);  // even overhang: exact
}

// Out-of-range zooms give exactly the geometry of the nearest limit. The strip at edge 2560 shows
// any difference: zoom 199 already gives a 5094 px page instead of 5120.
TEST(CropMath, ZoomIsClamped) {
  const CropGeometry a4At100 = ComputeCrop(595, 842, 256, 100);
  for (int zoom : {99, 50, 1, 0, -5, INT_MIN}) {
    EXPECT_TRUE(Same(ComputeCrop(595, 842, 256, zoom), a4At100)) << zoom;
  }
  const CropGeometry stripAt200 = ComputeCrop(30, 3000, 2560, 200);
  EXPECT_TRUE(Same(stripAt200, {51, 2560, 0, -1280, 51, 5120}));
  EXPECT_TRUE(Same(ComputeCrop(30, 3000, 2560, 199), {51, 2560, 0, -1267, 51, 5094}));
  for (int zoom : {201, 1000, 5000, INT_MAX}) {
    EXPECT_TRUE(Same(ComputeCrop(30, 3000, 2560, zoom), stripAt200)) << zoom;
  }
}

// For every zoom and a range of pages and edges: the longer side fills the edge, the shorter
// side is never cropped, the crop is centred, and more zoom never shows more of the page.
TEST(CropMath, InvariantsOverEveryZoom) {
  const struct {
    double w, h;
  } kPages[] = {{595, 842}, {842, 595},     {30, 3000},  {3000, 30}, {612, 792},
                {960, 540}, {14400, 14400}, {1, 999999}, {595, 421}, {1, 1}};
  for (const auto& page : kPages) {
    const bool wide = page.w >= page.h;
    for (unsigned edge : {1u, 16u, 256u, 1024u, 2560u}) {
      int previousLong = 0;
      for (int zoom = kMinZoom; zoom <= kMaxZoom; ++zoom) {
        const CropGeometry g = ComputeCrop(page.w, page.h, edge, zoom);
        const int bmpLong = wide ? g.bmpW : g.bmpH, bmpShort = wide ? g.bmpH : g.bmpW;
        const int pageLong = wide ? g.pageW : g.pageH, pageShort = wide ? g.pageH : g.pageW;
        SCOPED_TRACE(::testing::Message()
                     << page.w << "x" << page.h << " pt, edge " << edge << ", zoom " << zoom);
        ASSERT_EQ(bmpLong, static_cast<int>(edge));
        ASSERT_EQ(bmpShort, pageShort);
        ASSERT_GE(bmpShort, 1);
        ASSERT_GE(pageLong, bmpLong);
        ASSERT_EQ(g.pageLeft, (g.bmpW - g.pageW) / 2);
        ASSERT_EQ(g.pageTop, (g.bmpH - g.pageH) / 2);
        ASSERT_LE(g.pageLeft, 0);
        ASSERT_LE(g.pageTop, 0);
        ASSERT_GE(pageLong, previousLong);
        previousLong = pageLong;
      }
    }
  }
}

}  // namespace
}  // namespace pdft::test
