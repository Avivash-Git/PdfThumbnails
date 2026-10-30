// Shared state of pdft_handler_tests: the loaded handler DLL, and the 18 test PDFs plus the two
// preview samples, generated once into a private temp folder. The zoom always comes from a
// private settings.ini (PDFTHUMBNAILS_SETTINGS), never from the user's own settings.

#pragma once

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "handler_host.h"
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
  // Makes the handler read Zoom=<zoom> from now on.
  static void SetZoom(int zoom);
  static const std::wstring& Folder();

 private:
  static std::unique_ptr<TempDir> dir_;
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
