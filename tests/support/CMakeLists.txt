# Test support, shared by the developer tools (thumbtest) and the test suites:
#   pdft_testsupport  STATIC  test_pdfs (the test-PDF and preview-sample generator)
# Never shipped.

add_library(pdft_testsupport STATIC test_pdfs.cpp test_pdfs.h)
target_include_directories(pdft_testsupport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
pdft_harden(pdft_testsupport LIB)
