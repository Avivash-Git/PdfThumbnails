// Windows Search (search_index.h), read-only, on a new empty folder under %TEMP%, which the index
// does not cover: the answer is a count, never a path. Where Windows Search is off or missing
// (the runners GitHub hosts) the query says so without failing.

#include <windows.h>

#include "search_index.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <stop_token>
#include <vector>

#include "temp_dir.h"

namespace pdft {
namespace {

TEST(SearchIndex, AFolderOutsideTheIndexHasNoFiles) {
  test::TempDir dir(L"search-index");
  ASSERT_TRUE(dir.ok());
  const std::filesystem::path folder = dir.path();
  std::vector<std::filesystem::path> files;
  bool available = true;
  ASSERT_HRESULT_SUCCEEDED(QueryIndexedPdfs(10, &folder, {}, &files, &available));
  std::printf("[ search ] Windows Search available: %s\n", available ? "yes" : "no");
  EXPECT_TRUE(files.empty());
}

TEST(SearchIndex, StoppedBeforeItStarts) {
  test::TempDir dir(L"search-index");
  ASSERT_TRUE(dir.ok());
  const std::filesystem::path folder = dir.path();
  std::stop_source source;
  source.request_stop();
  std::vector<std::filesystem::path> files;
  bool available = true;
  EXPECT_EQ(QueryIndexedPdfs(10, &folder, source.get_token(), &files, &available),
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
  EXPECT_FALSE(available);
  EXPECT_TRUE(files.empty());
}

}  // namespace
}  // namespace pdft
