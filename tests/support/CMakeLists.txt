# Test support, shared by the GoogleTest suites (tests/) and the developer tools (thumbtest):
#   pdft_testsupport  STATIC  test_pdfs (the generator), medium_il (Medium-integrity relaunch),
#                             temp_dir, registry_sandbox (HKCU redirected to a private key),
#                             json_check (a strict JSON reader for the app's JSON output),
#                             refresh_judge (the thumbnail refresh experiment's verdict from the
#                             sizes the cache reports, and its record files)
#   pdft_run_medium   EXE     pdft-run-medium.exe: runs a command at Medium integrity
# Built when tools or tests are; never shipped.

add_library(pdft_testsupport STATIC
  test_pdfs.cpp test_pdfs.h
  medium_il.cpp medium_il.h
  temp_dir.cpp temp_dir.h
  registry_sandbox.cpp registry_sandbox.h
  json_check.cpp json_check.h
  refresh_judge.cpp refresh_judge.h)
target_include_directories(pdft_testsupport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
pdft_harden(pdft_testsupport LIB)
# pdft_common: crop_math.h, for the refresh experiment's verdict.
target_link_libraries(pdft_testsupport PRIVATE pdft_config pdft_common PUBLIC advapi32 ole32)

add_executable(pdft_run_medium run_medium.cpp)
set_target_properties(pdft_run_medium PROPERTIES OUTPUT_NAME pdft-run-medium)
pdft_add_version_resource(pdft_run_medium FILETYPE VFT_APP
  DESCRIPTION "${PDFT_PRODUCT_NAME} Medium-integrity launcher (tests)")
pdft_harden(pdft_run_medium EXE)
target_link_libraries(pdft_run_medium PRIVATE pdft_testsupport)
