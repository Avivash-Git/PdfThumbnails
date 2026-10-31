// pdft_unit_tests. "--pdft-child <mode>" makes the program the child process that some tests
// start: the test program starts itself, so no second locally built program has to be allowed
// to run (Smart App Control may block one).
//   exit7        exits with 7
//   check-token  prints its token like pdft-run-medium --check-token; 0 = Medium, no admin
//   harden       runs the app's start-up hardening and checks it from the inside: 0 = hardened
//                (only when the app is built, PDFT_BUILD_APP)

#include <windows.h>

#include <gtest/gtest.h>

#include <string>

#include "medium_il.h"
#include "project_config.h"

// The app's start-up code comes with pdft_appcore, which exists only when the app is built
// (tests/CMakeLists.txt defines this then). Without the app the rest of the program still builds.
#if defined(PDFT_UNIT_TESTS_WITH_APP)
#include "startup.h"
#endif

namespace {

#if defined(PDFT_UNIT_TESTS_WITH_APP)
// The "harden" child. Exit codes:
//   0  hardened: PDFium next to this program was not found by its bare name
//   1  HardenProcess failed
//   2  the image-load policy is not in force
//   3  PDFium next to this program still loads by bare name
//   4  Application Control blocked that load: the bare name reached a file outside System32
//      (most likely the one next to this program), so hardening failed
//   5  that load failed for another reason (a wrong-architecture copy, a missing dependency):
//      the bare name reached a file too, so hardening is not shown to work
//   6  PDFium is not next to this program, so there is nothing to check against
int HardenedChildMain() {
  if (FAILED(pdft::HardenProcess())) return 1;
  PROCESS_MITIGATION_IMAGE_LOAD_POLICY policy = {};
  if (!GetProcessMitigationPolicy(GetCurrentProcess(), ProcessImageLoadPolicy, &policy,
                                  sizeof(policy)) ||
      !policy.NoRemoteImages || !policy.NoLowMandatoryLabelImages || !policy.PreferSystem32Images) {
    return 2;
  }
  // PDFium sits next to the test programs (build folder and test bundle). Found by bare name
  // before hardening; afterwards only System32 is searched, where it is not, so the load must
  // fail with "module not found" and nothing else. Nothing is loaded when the check passes.
  wchar_t self[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
  std::wstring planted(self, length);
  planted = planted.substr(0, planted.find_last_of(L'\\') + 1) + pdft::kPdfiumDll;
  if (GetFileAttributesW(planted.c_str()) == INVALID_FILE_ATTRIBUTES) return 6;
  HMODULE module = LoadLibraryW(pdft::kPdfiumDll);
  if (module) {
    FreeLibrary(module);
    return 3;
  }
  const DWORD error = GetLastError();
  if (error == ERROR_MOD_NOT_FOUND) return 0;
  if (pdft::test::IsApplicationControlBlock(HRESULT_FROM_WIN32(error))) return 4;
  return 5;
}
#endif

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"--pdft-child") {
    const std::wstring mode = argv[2];
    if (mode == L"exit7") return 7;
    if (mode == L"check-token") return pdft::test::ReportOwnToken();
#if defined(PDFT_UNIT_TESTS_WITH_APP)
    if (mode == L"harden") return HardenedChildMain();
#endif
    return 2;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
