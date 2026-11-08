// Splitting a child's output into lines as it arrives (program_run.h).

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "program_run.h"

namespace pdft {
namespace {

class Lines : public ::testing::Test {
 protected:
  OutputLines::LineFn Collect() {
    return [this](std::wstring_view line) { lines_.emplace_back(line); };
  }
  OutputLines splitter_;
  std::vector<std::wstring> lines_;
};

TEST_F(Lines, BothLineEndsEndALine) {
  splitter_.Feed("one\r\ntwo\nthree\r\n", Collect());
  EXPECT_EQ(lines_, (std::vector<std::wstring>{L"one", L"two", L"three"}));
}

TEST_F(Lines, ALineMayArriveInPieces) {
  splitter_.Feed("hel", Collect());
  EXPECT_TRUE(lines_.empty());
  splitter_.Feed("lo\r", Collect());
  EXPECT_TRUE(lines_.empty());
  splitter_.Feed("\nnext", Collect());
  EXPECT_EQ(lines_, (std::vector<std::wstring>{L"hello"}));
  splitter_.Finish(Collect());
  EXPECT_EQ(lines_, (std::vector<std::wstring>{L"hello", L"next"}));
}

TEST_F(Lines, ACharacterSplitAcrossPiecesStaysWhole) {
  splitter_.Feed("a\xE2\x86", Collect());
  splitter_.Feed("\x92z\n", Collect());
  EXPECT_EQ(lines_, (std::vector<std::wstring>{L"a→z"}));
}

TEST_F(Lines, AnInvalidByteIsReplaced) {
  splitter_.Feed("a\xFFz\n", Collect());
  EXPECT_EQ(lines_, (std::vector<std::wstring>{L"a�z"}));
}

TEST_F(Lines, NothingLeftMeansNoLastLine) {
  splitter_.Feed("x\n", Collect());
  splitter_.Finish(Collect());
  EXPECT_EQ(lines_.size(), 1u);
}

}  // namespace
}  // namespace pdft
