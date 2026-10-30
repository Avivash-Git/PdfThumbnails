// Shared state of pdft_handler_tests: the loaded handler DLL, and the 18 test PDFs plus the two
// preview samples, generated once into a private temp folder. HKEY_CURRENT_USER is redirected to
// an empty private key for the whole run, and the 1.x settings file is a private settings.ini
// (PDFTHUMBNAILS_SETTINGS), so neither the user's registry nor their own settings ever decide a
// test's zoom.

#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "handler_host.h"
#include "registry_sandbox.h"
#include "temp_dir.h"

namespace pdft::test {

class HandlerEnvironment : public ::testing::Environment {
 public:
  void SetUp() override;
  void TearDown() override;

  static const HandlerHost& Host();
  // Full path of the generated file whose name starts with |prefix| ("01-", "sample-slide").
  static std::wstring Pdf(std::wstring_view prefix);
  // Renders the generated file |prefix| at |cx|.
  static Rendered Render(std::wstring_view prefix, UINT cx = 256);
  // The same as a preview: SetZoom(|zoom|) on the object first.
  static Rendered RenderAtZoom(std::wstring_view prefix, int zoom, UINT cx = 256);
  // Makes the handler read Zoom=<zoom> from the settings file from now on.
  static void SetZoom(int zoom);
  static const std::wstring& Folder();
  // The private key that HKEY_CURRENT_USER stands for during the run. Write the handler's
  // registry values through Registry().key(), never through HKEY_CURRENT_USER.
  static const ScopedHkcuRedirect& Registry();

 private:
  static std::unique_ptr<TempDir> dir_;
  static std::unique_ptr<ScopedHkcuRedirect> registry_;
  static std::unique_ptr<HandlerHost> host_;
};

// Restores zoom 100 when it goes out of scope.
class ScopedZoom {
 public:
  explicit ScopedZoom(int zoom) { HandlerEnvironment::SetZoom(zoom); }
  ScopedZoom(const ScopedZoom&) = delete;
  ScopedZoom& operator=(const ScopedZoom&) = delete;
  ~ScopedZoom() { HandlerEnvironment::SetZoom(100); }
};

}  // namespace pdft::test
