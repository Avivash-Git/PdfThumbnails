# Test programs (GoogleTest) and their CTest registrations. One CTest test per program, so
# nothing runs at build time and cross-compiled ARM64 builds work; run them with
#   ctest --preset x64-release -L "unit|handler"
#
#   unit.tests                pdft_unit_tests: test-PDF generator (byte-identical to the 1.x
#                             generator), Medium-integrity support, temp folders
#   unit.samples              src/app/res/sample-*.pdf == `thumbtest makesamples` output
#   handler.tests             pdft_handler_tests: PdfThumbnails.dll in-process at Medium
#                             integrity (relaunches itself when elevated): crop table, pixels,
#                             COM surface, error HRESULTs, 8-thread stress
#   handler.refuses_elevated  pdft_refuses_elevated: elevated, the DLL must refuse to render;
#                             skipped (exit 77) when not elevated
# Test support (tests/support) is added by the top-level CMakeLists.txt: thumbtest uses it too.

pdft_add_googletest()
set(PDFT_GTEST_DIR "${PDFT_GTEST_DIR}" PARENT_SCOPE)   # for Install.cmake (its LICENSE)

# ---- unit ----------------------------------------------------------------------------------
add_executable(pdft_unit_tests
  unit/main.cpp
  unit/test_pdfs_test.cpp
  unit/medium_il_test.cpp
  unit/temp_dir_test.cpp)
pdft_harden(pdft_unit_tests TEST)
target_link_libraries(pdft_unit_tests PRIVATE pdft_testsupport GTest::gtest bcrypt shell32)
add_dependencies(pdft_unit_tests pdft_run_medium)   # RunMediumTool tests
add_test(NAME unit.tests COMMAND pdft_unit_tests)
set_tests_properties(unit.tests PROPERTIES LABELS unit)

if(TARGET thumbtest)
  add_test(NAME unit.samples
    COMMAND "${CMAKE_COMMAND}" "-DTHUMBTEST=$<TARGET_FILE:thumbtest>"
            "-DCOMMITTED_DIR=${PROJECT_SOURCE_DIR}/src/app/res"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/samples"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckSamples.cmake")
  set_tests_properties(unit.samples PROPERTIES LABELS unit)
endif()

# ---- handler -------------------------------------------------------------------------------
add_library(pdft_handler_host STATIC handler/handler_host.cpp handler/handler_host.h)
pdft_harden(pdft_handler_host LIB)
target_link_libraries(pdft_handler_host PUBLIC pdft_testsupport pdft_config
                                               ole32 uuid shlwapi gdi32 user32)

add_executable(pdft_handler_tests
  handler/main.cpp
  handler/handler_env.h
  handler/render_dimensions_test.cpp
  handler/render_errors_test.cpp
  handler/stress_test.cpp)
pdft_harden(pdft_handler_tests TEST)
target_link_libraries(pdft_handler_tests PRIVATE pdft_handler_host GTest::gtest)
add_dependencies(pdft_handler_tests pdft_handler)   # PdfThumbnails.dll (+ PDFium) next to it
add_test(NAME handler.tests COMMAND pdft_handler_tests)
# Serial: the stress test wants the machine to itself, and its timing budgets are 1.1's.
set_tests_properties(handler.tests PROPERTIES LABELS handler RUN_SERIAL TRUE)

add_executable(pdft_refuses_elevated handler/refuses_elevated.cpp)
pdft_harden(pdft_refuses_elevated TEST)
target_link_libraries(pdft_refuses_elevated PRIVATE pdft_handler_host)
add_dependencies(pdft_refuses_elevated pdft_handler)
add_test(NAME handler.refuses_elevated COMMAND pdft_refuses_elevated)
set_tests_properties(handler.refuses_elevated PROPERTIES LABELS handler SKIP_RETURN_CODE 77)
