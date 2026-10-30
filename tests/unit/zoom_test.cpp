// ClampZoom: values inside 100..200 pass unchanged; everything else, however it arrives (int,
// DWORD, long long), clamps to the nearest limit without wrapping around.

#include <windows.h>

#include "zoom.h"

#include <gtest/gtest.h>

#include <climits>

namespace pdft::test {
namespace {

static_assert(ClampZoom(150) == 150);
static_assert(ClampZoom(-5) == kMinZoom);
static_assert(ClampZoom(0xFFFFFFFFu) == kMaxZoom);
static_assert(noexcept(ClampZoom(0)));

TEST(Zoom, Constants) {
  EXPECT_EQ(kMinZoom, 100);
  EXPECT_EQ(kMaxZoom, 200);
  EXPECT_STREQ(kZoomValueName, L"Zoom");
}

TEST(Zoom, InsideAndAtTheLimits) {
  for (int zoom : {100, 101, 120, 141, 142, 150, 199, 200}) {
    EXPECT_EQ(ClampZoom(zoom), zoom);
  }
}

TEST(Zoom, BelowTheMinimum) {
  for (long long zoom : {99ll, 50ll, 1ll, 0ll, -1ll, -5ll, -100ll, -200ll}) {
    EXPECT_EQ(ClampZoom(zoom), 100) << zoom;
  }
  EXPECT_EQ(ClampZoom(INT_MIN), 100);
  EXPECT_EQ(ClampZoom(INT_MIN + 1), 100);
  EXPECT_EQ(ClampZoom(LLONG_MIN), 100);
}

TEST(Zoom, AboveTheMaximum) {
  for (long long zoom : {201ll, 300ll, 1000ll, 5000ll, 65536ll}) {
    EXPECT_EQ(ClampZoom(zoom), 200) << zoom;
  }
  EXPECT_EQ(ClampZoom(INT_MAX), 200);
  EXPECT_EQ(ClampZoom(UINT_MAX), 200);
  EXPECT_EQ(ClampZoom(LLONG_MAX), 200);
}

// A REG_DWORD arrives as an unsigned 32-bit value: above INT_MAX it must not turn negative.
TEST(Zoom, DwordsDoNotWrapAround) {
  const DWORD all = 0xFFFFFFFF;
  const DWORD topBit = 0x80000000;
  const DWORD minusFive = static_cast<DWORD>(-5);  // what -5 written as a DWORD reads back as
  EXPECT_EQ(ClampZoom(all), 200);
  EXPECT_EQ(ClampZoom(topBit), 200);
  EXPECT_EQ(ClampZoom(minusFive), 200);
  EXPECT_EQ(ClampZoom(DWORD{120}), 120);
  EXPECT_EQ(ClampZoom(DWORD{0}), 100);
  // GetPrivateProfileIntW returns a UINT: the same rules.
  EXPECT_EQ(ClampZoom(UINT{4294967295u}), 200);
}

}  // namespace
}  // namespace pdft::test
