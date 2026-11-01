// pdft_system_tests: installs, uses and uninstalls the app for real (system_test.cpp). Before
// anything else it makes sure it may: the environment asks for it (PDFT_SYSTEM_TESTS=1, which
// only the CI job sets), the process is elevated, its registry writes reach the real hive, and
// nothing of 1.x or 2.x is on this PC, which the tests would change or remove. Otherwise it
// changes nothing and exits 77 (CTest's "skipped"), or 3 where the product is installed.

#include <windows.h>

#include <gtest/gtest.h>
#include <objbase.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "com_security.h"
#include "system_env.h"

int wmain(int argc, wchar_t** argv) {
  using namespace pdft::systest;
  // Checked first, before COM or anything else that touches the system.
  if (EnvironmentValue(kEnableEnv) != L"1") {
    std::printf(
        "pdft_system_tests: not run: PDFT_SYSTEM_TESTS is not 1 (these tests install "
        "and uninstall for real, on a throw-away PC only)\n");
    return kSkipCode;
  }
  const HRESULT com = pdft::InitializeComSecurity();
  if (FAILED(com)) {
    std::printf("InitializeComSecurity failed: 0x%08lX\n", static_cast<unsigned long>(com));
    return 1;
  }
  int result = kSkipCode;
  if (const std::optional<std::string> why = WhyNotHere()) {
    std::printf("pdft_system_tests: not run: %s\n", why->c_str());
  } else if (const std::vector<std::string> found = Leftovers(); !found.empty()) {
    std::printf(
        "pdft_system_tests: refused: this PC has 1.x or 2.x on it, which these tests "
        "would change or remove:\n");
    for (const std::string& line : found) std::printf("  %s\n", line.c_str());
    result = kRefusedCode;
  } else {
    ::testing::InitGoogleTest(&argc, argv);
    result = RUN_ALL_TESTS();
  }
  CoUninitialize();
  return result;
}
