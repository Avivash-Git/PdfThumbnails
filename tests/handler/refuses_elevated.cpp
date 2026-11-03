// pdft_refuses_elevated: the handler never parses PDFs in a process that may have administrator
// rights. Deliberately without the Medium relaunch: elevated (CI), a valid PDF must give
// WTS_E_FAILEDEXTRACTION. Where this process also has the Administrators group enabled (CI), it
// then starts a copy of itself with its token lowered only to Medium integrity, Administrators
// still enabled, as another program could start a thumbnail process; that copy must get the same
// result. At Medium there is nothing to check and the test reports "skipped" (exit 77, CTest
// SKIP_RETURN_CODE); elevated without Administrators enabled, only the copy is skipped, with the
// reason printed.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "handler_host.h"
#include "medium_il.h"
#include "project_config.h"
#include "temp_dir.h"
#include "test_pdfs.h"

namespace pdft::test {
namespace {

// The argument that makes this program the copy with the label-only Medium token.
constexpr wchar_t kLabelOnlyChild[] = L"--label-only-child";

// Renders a valid PDF with the handler loaded into this process. 0 when the handler refuses with
// WTS_E_FAILEDEXTRACTION; |where| ends the line that says so.
int ExpectRefusal(const char* where) {
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int result = 1;
  {
    TempDir dir(L"elevated");
    std::wstring error;
    std::vector<GeneratedFile> files;
    for (auto& file : MakeTestPdfs()) {
      if (file.name == L"01-portrait.pdf") files.push_back(std::move(file));
    }
    HandlerHost host;
    HRESULT hr = E_FAIL;
    if (!dir.ok() || !UseZoom(dir.path(), 100) || !WriteFiles(dir.path(), files, &error)) {
      std::printf("FAIL: cannot prepare the test file\n");
    } else if (FAILED(hr = host.Load())) {
      std::printf("FAIL: cannot load %s: %s\n", Printable(host.path()).c_str(),
                  HresultText(hr).c_str());
    } else {
      const Rendered r = host.RenderFile(dir.File(L"01-portrait.pdf"), 256);
      std::printf("integrity 0x%04lX, Administrators %s: 01-portrait.pdf -> %s\n",
                  CurrentIntegrityRid(),
                  CurrentTokenHasEnabledAdministrators() ? "enabled" : "not enabled",
                  HresultText(r.hr).c_str());
      if (r.hr == WTS_E_FAILEDEXTRACTION) {
        std::printf("ok: the handler refuses to render %s\n", where);
        result = 0;
      } else {
        std::printf("FAIL: expected %s\n", HresultText(WTS_E_FAILEDEXTRACTION).c_str());
      }
    }
    SetEnvironmentVariableW(pdft::kLegacySettingsEnv, nullptr);
  }
  CoUninitialize();
  std::fflush(stdout);
  return result;
}

// The copy. It tests the handler only with the token it was started for: Medium integrity with
// Administrators enabled.
int LabelOnlyChild() {
  const DWORD rid = CurrentIntegrityRid();
  if (rid != SECURITY_MANDATORY_MEDIUM_RID || !CurrentTokenHasEnabledAdministrators()) {
    std::printf("FAIL: the copy is not at Medium with Administrators enabled (integrity 0x%04lX)\n",
                rid);
    return 1;
  }
  return ExpectRefusal("at Medium integrity with Administrators enabled");
}

// Starts the copy with this process's token lowered only to Medium integrity (Administrators and
// every privilege stay) and waits for it. 0 when it passed, or when Application Control blocked
// it (then skipped, with the reason printed).
int ExpectLabelOnlyRefusal() {
  wchar_t self[MAX_PATH];
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  HANDLE token = nullptr;
  HRESULT hr = length == 0 || length >= MAX_PATH
                   ? E_FAIL
                   : CreateMediumTokenForSelf(MediumToken::kLabelOnly, &token);
  DWORD code = 1;
  if (SUCCEEDED(hr)) {
    hr = RunWithToken(token, self, QuoteArgument(self) + L" " + kLabelOnlyChild, &code);
    CloseHandle(token);
  }
  if (IsApplicationControlBlock(hr)) {
    std::printf(
        "skipped: Application Control (Smart App Control) blocked the copy at Medium with "
        "Administrators enabled: %s\n",
        HresultText(hr).c_str());
    return 0;
  }
  if (FAILED(hr)) {
    std::printf("FAIL: cannot start the copy at Medium with Administrators enabled: %s\n",
                HresultText(hr).c_str());
    return 1;
  }
  if (code != 0) {
    std::printf("FAIL: the copy at Medium with Administrators enabled exited with %lu\n", code);
    return 1;
  }
  return 0;
}

}  // namespace
}  // namespace pdft::test

int wmain(int argc, wchar_t** argv) {
  using namespace pdft::test;
  if (argc == 2 && std::wstring(argv[1]) == kLabelOnlyChild) return LabelOnlyChild();
  const DWORD rid = CurrentIntegrityRid();
  if (rid != kUnknownIntegrity && rid <= SECURITY_MANDATORY_MEDIUM_RID) {
    std::printf("skipped: this process runs at integrity 0x%04lX, not above Medium\n", rid);
    return 77;
  }
  const int elevated = ExpectRefusal("in an elevated process");
  if (!CurrentTokenHasEnabledAdministrators()) {
    std::printf(
        "skipped: the copy at Medium with Administrators enabled: this process does not have "
        "Administrators enabled\n");
    return elevated;
  }
  const int labelOnly = ExpectLabelOnlyRefusal();
  return elevated != 0 ? elevated : labelOnly;
}
