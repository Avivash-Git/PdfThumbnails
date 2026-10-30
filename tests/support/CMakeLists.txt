# Test support, shared by the GoogleTest suites (tests/) and the developer tools (thumbtest):
#   pdft_testsupport  STATIC  test_pdfs (the generator), medium_il (Medium-integrity relaunch),
#                             temp_dir
#   pdft_run_medium   EXE     pdft-run-medium.exe: runs a command at Medium integrity
# Built when tools or tests are; never shipped.

add_library(pdft_testsupport STATIC
  test_pdfs.cpp test_pdfs.h
  medium_il.cpp medium_il.h
  temp_dir.cpp temp_dir.h)
target_include_directories(pdft_testsupport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
pdft_harden(pdft_testsupport LIB)
target_link_libraries(pdft_testsupport PUBLIC advapi32)

add_executable(pdft_run_medium run_medium.cpp)
set_target_properties(pdft_run_medium PROPERTIES OUTPUT_NAME pdft-run-medium)
pdft_add_version_resource(pdft_run_medium FILETYPE VFT_APP
  DESCRIPTION "${PDFT_PRODUCT_NAME} Medium-integrity launcher (tests)")
pdft_harden(pdft_run_medium EXE)
target_link_libraries(pdft_run_medium PRIVATE pdft_testsupport)
