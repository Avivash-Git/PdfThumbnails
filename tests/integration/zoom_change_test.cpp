// Saving a new zoom and refreshing afterwards (zoom_change.h), against a private registry key,
// with fakes for what would look at the real File Explorer and thumbnail cache.

#include <windows.h>

#include "zoom_change.h"

#include <gtest/gtest.h>

#include <functional>
#include <optional>
#include <semaphore>
#include <string>
#include <thread>

#include "instance.h"
#include "project_config.h"
#include "registry_sandbox.h"
#include "settings.h"
#include "strings.h"
#include "temp_dir.h"

namespace pdft {
namespace {

class ZoomChange : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(sandbox_.ok()) << HResultText(sandbox_.error());
    ASSERT_TRUE(temp_.ok());
    roots_ = {sandbox_.user(), sandbox_.machine()};
    // LoadSettings falls back to the 1.x settings file: never the user's real one.
    const std::wstring missing = temp_.File(L"no-such-settings.ini");
    ASSERT_TRUE(SetEnvironmentVariableW(kLegacySettingsEnv, missing.c_str()));

    steps_.readConditions = [this](const ProcessContext&, const RegistryRoots&,
                                   RefreshConditions* conditions) {
      *conditions = conditions_;
      return readResult_;
    };
    steps_.refresh = [this](const RefreshProgressFn&, std::stop_token, RefreshCounts* counts) {
      ++refreshes_;
      counts->refreshed = 7;
      return S_OK;
    };
    conditions_.writesChecked = true;
    conditions_.classRegistered = true;
    conditions_.effectiveIsOurs = true;
  }
  void TearDown() override { SetEnvironmentVariableW(kLegacySettingsEnv, nullptr); }

  ZoomChangeResult Change(int zoom, bool refresh, DWORD lockWaitMs = 5000) {
    ZoomChangeOptions options;
    options.zoom = zoom;
    options.refresh = refresh;
    options.lockWaitMs = lockWaitMs;
    options.beforeRefresh = [this] { ++beforeRefresh_; };
    ZoomChangeResult result;
    EXPECT_HRESULT_SUCCEEDED(ChangeZoom(options, roots_, steps_, &result));
    return result;
  }

  int SavedZoom() {
    Settings settings;
    EXPECT_HRESULT_SUCCEEDED(LoadSettings(roots_, &settings));
    return settings.zoom;
  }

  test::ScopedRegistrySandbox sandbox_;
  test::TempDir temp_{L"zoomchange"};
  RegistryRoots roots_;
  ZoomRefreshSteps steps_;
  RefreshConditions conditions_;
  HRESULT readResult_ = S_OK;
  int refreshes_ = 0;
  int beforeRefresh_ = 0;
};

TEST_F(ZoomChange, ANewZoomIsSavedAndRefreshed) {
  const ZoomChangeResult result = Change(130, true);
  EXPECT_TRUE(result.saved);
  EXPECT_TRUE(result.changed);
  EXPECT_TRUE(result.refreshRan);
  EXPECT_EQ(result.counts.refreshed, 7u);
  EXPECT_EQ(refreshes_, 1);
  EXPECT_EQ(beforeRefresh_, 1);
  EXPECT_EQ(SavedZoom(), 130);
}

TEST_F(ZoomChange, TheSameZoomRefreshesNothing) {
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 130));
  const ZoomChangeResult result = Change(130, true);
  EXPECT_TRUE(result.saved);
  EXPECT_FALSE(result.changed);
  EXPECT_FALSE(result.refreshRan);
  EXPECT_EQ(refreshes_, 0);
}

TEST_F(ZoomChange, NoRefreshUnlessAsked) {
  const ZoomChangeResult result = Change(120, false);
  EXPECT_TRUE(result.saved);
  EXPECT_FALSE(result.refreshRan);
  EXPECT_EQ(refreshes_, 0);
  EXPECT_EQ(SavedZoom(), 120);
}

TEST_F(ZoomChange, ARefusedRefreshLeavesTheZoomSaved) {
  conditions_.effectiveIsOurs = false;
  const ZoomChangeResult result = Change(150, true);
  EXPECT_TRUE(result.saved);
  EXPECT_EQ(result.refusal, RefreshRefusal::kNotOurHandler);
  EXPECT_FALSE(result.refreshRan);
  EXPECT_EQ(beforeRefresh_, 0);
  EXPECT_EQ(SavedZoom(), 150);
}

TEST_F(ZoomChange, ConditionsThatCannotBeReadCountAsNotChecked) {
  readResult_ = E_FAIL;
  const ZoomChangeResult result = Change(150, true);
  EXPECT_TRUE(result.saved);
  EXPECT_EQ(result.refusal, RefreshRefusal::kVirtualized);
  EXPECT_EQ(refreshes_, 0);
}

TEST_F(ZoomChange, ATakenLockChangesNothing) {
  ASSERT_HRESULT_SUCCEEDED(SaveZoom(roots_, 110));
  std::binary_semaphore held(0);
  std::binary_semaphore release(0);
  std::jthread holder([&held, &release] {
    std::optional<OperationLock> lock;
    ASSERT_HRESULT_SUCCEEDED(OperationLock::Acquire(5000, &lock));
    held.release();
    release.acquire();
  });
  held.acquire();
  const ZoomChangeResult result = Change(130, true, 0);
  release.release();
  EXPECT_TRUE(result.busy);
  EXPECT_FALSE(result.saved);
  EXPECT_EQ(SavedZoom(), 110);
}

}  // namespace
}  // namespace pdft
