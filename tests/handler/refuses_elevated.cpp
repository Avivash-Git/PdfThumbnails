// pdft_refuses_elevated: the handler never parses PDFs above Medium integrity (as in 1.1).
// Deliberately without the Medium relaunch: elevated (CI), a valid PDF must give
// WTS_E_FAILEDEXTRACTION; at Medium there is nothing to check and the test reports "skipped"
// (exit 77, CTest SKIP_RETURN_CODE).

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "handler_host.h"
#include "medium_il.h"
#include "project_config.h"
#include "temp_dir.h"
#include "test_pdfs.h"

int wmain() {
  using namespace pdft::test;
  const DWORD rid = CurrentIntegrityRid();
  if (rid != kUnknownIntegrity && rid <= SECURITY_MANDATORY_MEDIUM_RID) {
    std::printf("skipped: this process runs at integrity 0x%04lX, not above Medium\n", rid);
    return 77;
  }
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
      std::printf("integrity 0x%04lX: 01-portrait.pdf -> %s\n", rid, HresultText(r.hr).c_str());
      if (r.hr == WTS_E_FAILEDEXTRACTION) {
        std::printf("ok: the handler refuses to render in an elevated process\n");
        result = 0;
      } else {
        std::printf("FAIL: expected %s\n", HresultText(WTS_E_FAILEDEXTRACTION).c_str());
      }
    }
    SetEnvironmentVariableW(pdft::kLegacySettingsEnv, nullptr);
  }
  CoUninitialize();
  return result;
}
