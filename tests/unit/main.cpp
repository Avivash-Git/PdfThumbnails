// pdft_unit_tests. "--pdft-child <mode>" makes the program the child process that the
// Medium-integrity tests start: the test program starts itself, so no second locally built
// program has to be allowed to run (Smart App Control may block one).
//   exit7        exits with 7
//   check-token  prints its token like pdft-run-medium --check-token; 0 = Medium, no admin

#include <windows.h>

#include <gtest/gtest.h>

#include <string>

#include "medium_il.h"

int wmain(int argc, wchar_t** argv) {
  if (argc == 3 && std::wstring(argv[1]) == L"--pdft-child") {
    const std::wstring mode = argv[2];
    if (mode == L"exit7") return 7;
    if (mode == L"check-token") return pdft::test::ReportOwnToken();
    return 2;
  }
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
