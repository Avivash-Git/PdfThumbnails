// pdft_handler_tests: PdfThumbnails.dll loaded in-process, at Medium integrity. The handler
// refuses to render above Medium, so an elevated run (CI) first relaunches this program at
// Medium; the child prints "relaunched at Medium integrity".

#include <windows.h>

#include <gtest/gtest.h>

#include <cstdio>
#include <vector>

#include "handler_env.h"
#include "medium_il.h"
#include "project_config.h"
#include "test_pdfs.h"

namespace pdft::test {

std::unique_ptr<TempDir> HandlerEnvironment::dir_;
std::unique_ptr<HandlerHost> HandlerEnvironment::host_;

void HandlerEnvironment::SetUp() {
  dir_ = std::make_unique<TempDir>(L"handler");
  ASSERT_TRUE(dir_->ok()) << "cannot create a temp folder";
  // Before anything renders: the user's own settings.ini must never decide a test's zoom.
  ASSERT_TRUE(UseZoom(dir_->path(), 100));

  std::vector<GeneratedFile> files = MakeTestPdfs();
  for (auto& sample : MakeSamplePdfs()) files.push_back(std::move(sample));
  std::wstring error;
  ASSERT_TRUE(WriteFiles(dir_->path(), files, &error)) << Printable(error);

  host_ = std::make_unique<HandlerHost>();
  const HRESULT hr = host_->Load();
  ASSERT_EQ(hr, S_OK) << "cannot load " << Printable(host_->path()) << ": " << HresultText(hr);
  std::printf("[ info     ] handler %s, integrity 0x%04lX\n", Printable(host_->path()).c_str(),
              CurrentIntegrityRid());
}

void HandlerEnvironment::TearDown() {
  host_.reset();
  SetEnvironmentVariableW(pdft::kLegacySettingsEnv, nullptr);
  dir_.reset();
}

const HandlerHost& HandlerEnvironment::Host() { return *host_; }

const std::wstring& HandlerEnvironment::Folder() { return dir_->path(); }

std::wstring HandlerEnvironment::Pdf(std::wstring_view prefix) {
  WIN32_FIND_DATAW data;
  const std::wstring pattern = dir_->path() + L"\\" + std::wstring(prefix) + L"*.pdf";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) return dir_->path() + L"\\missing-" + std::wstring(prefix);
  FindClose(find);
  return dir_->File(data.cFileName);
}

Rendered HandlerEnvironment::Render(std::wstring_view prefix, UINT cx) {
  return host_->RenderFile(Pdf(prefix), cx);
}

void HandlerEnvironment::SetZoom(int zoom) {
  ASSERT_TRUE(UseZoom(dir_->path(), zoom)) << "cannot write the settings file for zoom " << zoom;
}

}  // namespace pdft::test

int main(int argc, char** argv) {
  int exitCode = 0;
  if (pdft::test::RelaunchAtMediumIfElevated(&exitCode)) return exitCode;
  // Explorer's thumbnail process calls the handler on STA threads.
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new pdft::test::HandlerEnvironment);
  const int result = RUN_ALL_TESTS();
  CoUninitialize();
  return result;
}
