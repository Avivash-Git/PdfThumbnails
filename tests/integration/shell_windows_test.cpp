// File Explorer's folder windows (shell_windows.h) through the running shell; skipped where there
// is none. Everywhere: the windows are listed (only counts are printed, never a folder's name),
// and views are looked for on a folder this test makes, which nobody shows, and on the desktop;
// nothing is done to them. Only on a runner GitHub hosts (GITHUB_ACTIONS=true with
// RUNNER_ENVIRONMENT=github-hosted) does it open windows: one on that folder through the shell,
// which it lists, closes, and opens again from what the listing saved (the way windows are
// reopened), and closes at the end. It closes only a frame that shows nothing but that folder.

#include <windows.h>

#include "shell_windows.h"

#include <gtest/gtest.h>
#include <wil/resource.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "github_runner.h"
#include "paths.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

using std::filesystem::path;

constexpr DWORD kTimeoutMs = 10000;
// The answer where no desktop of File Explorer's can be reached through the shell. A shell
// window can exist without one: some runners GitHub hosts have no desktop registered with it.
constexpr HRESULT kNoShell = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);

// The listed windows that show |folder|.
std::vector<FolderWindow> Showing(const std::vector<FolderWindow>& windows, const path& folder) {
  std::vector<FolderWindow> showing;
  for (const FolderWindow& window : windows) {
    if (IEquals(window.parsingName, folder.native())) showing.push_back(window);
  }
  return showing;
}

// The windows that show |folder|, listed again every 100 ms for up to 10 s until there are at
// least |atLeast| of them (|atLeast| 0: until there are none).
std::vector<FolderWindow> WaitForWindows(const path& folder, size_t atLeast) {
  std::vector<FolderWindow> showing;
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::vector<FolderWindow> windows;
    if (ListFolderWindows(kTimeoutMs, &windows) == S_OK) {
      showing = Showing(windows, folder);
      if (atLeast == 0 ? showing.empty() : showing.size() >= atLeast) break;
    }
    Sleep(100);
  }
  return showing;
}

TEST(ShellWindows, ListsTheFolderWindows) {
  if (!GetShellWindow()) GTEST_SKIP() << "no shell (Explorer) on this desktop";
  std::vector<FolderWindow> windows;
  ASSERT_EQ(ListFolderWindows(kTimeoutMs, &windows), S_OK);
  std::set<ULONG_PTR> frames;
  size_t unnamed = 0;
  for (const FolderWindow& window : windows) {
    frames.insert(window.frame);
    if (window.parsingName.empty()) ++unnamed;
  }
  std::printf("folder windows: %zu in %zu frames, %zu without a name\n", windows.size(),
              frames.size(), unnamed);
}

// The action only counts; it captures its counter by value, as an action must.
TEST(ShellWindows, NoViewShowsANewFolder) {
  if (!GetShellWindow()) GTEST_SKIP() << "no shell (Explorer) on this desktop";
  test::TempDir dir(L"shell");
  ASSERT_TRUE(dir.ok());
  path folder;
  ASSERT_EQ(Canonicalize(dir.path(), &folder), S_OK);
  const path folders[] = {folder};
  auto calls = std::make_shared<std::atomic<size_t>>(0);
  const ViewAction count = [calls](IShellBrowser* browser, IShellView* view) {
    if (browser && view) ++*calls;
  };
  size_t found = 99;
  ASSERT_EQ(ForEachViewShowing(folders, false, kTimeoutMs, count, &found), S_OK);
  EXPECT_EQ(found, 0u);
  EXPECT_EQ(calls->load(), 0u);

  // The desktop has a view of its own, whatever else is open, where it can be reached.
  const HRESULT desktop = ForEachViewShowing(folders, true, kTimeoutMs, count, &found);
  if (desktop == kNoShell) {
    std::printf("the desktop's view: not reachable through the shell here\n");
  } else {
    ASSERT_EQ(desktop, S_OK);
    EXPECT_EQ(found, 1u);
    EXPECT_EQ(calls->load(), 1u);
  }

  ASSERT_EQ(ForEachViewShowing({}, false, kTimeoutMs, count, &found), S_OK);
  EXPECT_EQ(found, 0u);
  EXPECT_EQ(ForEachViewShowing(folders, false, kTimeoutMs, ViewAction(), &found), E_INVALIDARG);
}

TEST(ShellWindows, OpensAFolderWindowThroughTheShell) {
  if (!test::OnGitHubHostedRunner()) {
    GTEST_SKIP() << "opens File Explorer windows: only on a runner GitHub hosts (GITHUB_ACTIONS="
                    "true with RUNNER_ENVIRONMENT=github-hosted)";
  }
  if (!GetShellWindow()) GTEST_SKIP() << "no shell (Explorer) on this desktop";
  test::TempDir dir(L"shell");
  ASSERT_TRUE(dir.ok());
  path folder;
  ASSERT_EQ(Canonicalize(dir.path(), &folder), S_OK);
  // Closes every window on the folder, and waits until they are gone. A frame that also holds a
  // tab on anything else is not this test's to close: it is left alone, and that fails.
  const auto closeWindows = [&folder] {
    std::vector<FolderWindow> windows;
    if (ListFolderWindows(kTimeoutMs, &windows) != S_OK) return false;
    const auto otherTab = [&windows, &folder](ULONG_PTR frame) {
      return std::any_of(windows.begin(), windows.end(), [&folder, frame](const FolderWindow& tab) {
        return tab.frame == frame && !IEquals(tab.parsingName, folder.native());
      });
    };
    std::set<ULONG_PTR> frames;
    bool alone = true;
    for (const FolderWindow& window : Showing(windows, folder)) {
      if (!window.frame || otherTab(window.frame)) {
        alone = false;
      } else {
        frames.insert(window.frame);
      }
    }
    for (const ULONG_PTR frame : frames) {
      PostMessageW(reinterpret_cast<HWND>(frame), WM_CLOSE, 0, 0);
    }
    return alone && WaitForWindows(folder, 0).empty();
  };
  // Before the folder is deleted.
  const auto close = wil::scope_exit([&closeWindows] { EXPECT_TRUE(closeWindows()); });

  FolderWindow wanted;
  wanted.parsingName = folder.native();
  size_t opened = 0;
  size_t failed = 0;
  const HRESULT open = OpenFolderWindows({&wanted, 1}, kTimeoutMs, &opened, &failed);
  if (open == kNoShell) {
    GTEST_SKIP() << "File Explorer's desktop cannot be reached through the shell here, so "
                    "nothing can be opened through it";
  }
  ASSERT_EQ(open, S_OK);
  EXPECT_EQ(opened, 1u);
  EXPECT_EQ(failed, 0u);
  const std::vector<FolderWindow> shown = WaitForWindows(folder, 1);
  ASSERT_EQ(shown.size(), 1u);
  EXPECT_NE(shown[0].frame, 0u);

  const path folders[] = {folder};
  const ViewAction nothing = [](IShellBrowser*, IShellView*) {};
  size_t found = 0;
  ASSERT_EQ(ForEachViewShowing(folders, false, kTimeoutMs, nothing, &found), S_OK);
  EXPECT_EQ(found, 1u);

  // Closed, then opened again from what the listing saved, as windows are reopened after File
  // Explorer restarts; a window without a name is not opened.
  ASSERT_TRUE(closeWindows());
  const FolderWindow again[] = {shown[0], FolderWindow{}};
  ASSERT_EQ(OpenFolderWindows(again, kTimeoutMs, &opened, &failed), S_OK);
  EXPECT_EQ(opened, 1u);
  EXPECT_EQ(failed, 1u);
  EXPECT_EQ(WaitForWindows(folder, 1).size(), 1u);
}

}  // namespace
}  // namespace pdft
