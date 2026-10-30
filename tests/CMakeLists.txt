# Test programs (GoogleTest) and their CTest registrations. One CTest test per program, so
# nothing runs at build time and cross-compiled ARM64 builds work; run them with
#   ctest --preset x64-release -L "unit|handler"
#
#   unit.tests                pdft_unit_tests: crop math, zoom clamping, the generated GUIDs,
#                             test-PDF generator (byte-identical to the 1.x generator),
#                             Medium-integrity support, temp folders, HKCU redirection
#   unit.samples              src/app/res/sample-*.pdf == `thumbtest makesamples` output
#   handler.tests             pdft_handler_tests: PdfThumbnails.dll in-process at Medium
#                             integrity (relaunches itself when elevated) with HKCU redirected to
#                             a private key: crop table, pixels, COM surface, error HRESULTs,
#                             where the zoom comes from (preview override, HKCU, settings.ini),
#                             8-thread stress
#   handler.refuses_elevated  pdft_refuses_elevated: elevated, the DLL must refuse to render;
#                             skipped (exit 77) when not elevated
# Test support (tests/support) is added by the top-level CMakeLists.txt: thumbtest uses it too.
#
# The same tests also run from the installed test bundle, a flat folder made with
#   cmake --install <build> --prefix <folder> --component tests
#   ctest --test-dir <folder> -L "unit|handler"
# CI builds ARM64 on an x64 host and runs the ARM64 bundle on an ARM64 runner. The bundle's
# CTestTestfile.cmake is generated at the end of this file from the registrations below, with
# paths relative to the bundle: register program tests with pdft_add_program_test() so both
# lists stay the same. A cross build (CMAKE_CROSSCOMPILING, the arm64 presets) cannot run its
# programs, so its build tree registers only the gates; its bundle still gets every test.

pdft_add_googletest()
set(PDFT_GTEST_DIR "${PDFT_GTEST_DIR}" PARENT_SCOPE)   # for Install.cmake (its LICENSE)

set_property(GLOBAL PROPERTY PDFT_BUNDLE_TESTS "")

# pdft_bundle_test(<name> COMMAND <word>... [PROPERTIES <property> <value>...])
# Adds one test to the installed bundle only. CTest runs bundle tests in the bundle folder and
# defines no path variables there (not even CMAKE_CURRENT_LIST_DIR), so paths are relative to
# the folder ("./<program>"). Words may use generator expressions.
function(pdft_bundle_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "COMMAND;PROPERTIES")
  set(text "add_test([==[${name}]==]")
  foreach(word IN LISTS arg_COMMAND)
    string(APPEND text " \"${word}\"")
  endforeach()
  string(APPEND text ")\n")
  if(arg_PROPERTIES)
    string(APPEND text "set_tests_properties([==[${name}]==] PROPERTIES")
    foreach(word IN LISTS arg_PROPERTIES)
      string(APPEND text " [==[${word}]==]")
    endforeach()
    string(APPEND text ")\n")
  endif()
  set_property(GLOBAL APPEND_STRING PROPERTY PDFT_BUNDLE_TESTS "${text}")
endfunction()

# pdft_add_program_test(<name> <target> [<property> <value>...])
# A test that runs <target>'s program with no arguments, in the build tree (native builds only)
# and in the bundle.
function(pdft_add_program_test name target)
  if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME ${name} COMMAND ${target})
    if(ARGN)
      set_tests_properties(${name} PROPERTIES ${ARGN})
    endif()
  endif()
  pdft_bundle_test(${name}
    COMMAND "./$<TARGET_FILE_NAME:${target}>"
    PROPERTIES ${ARGN})
endfunction()

# ---- unit ----------------------------------------------------------------------------------
add_executable(pdft_unit_tests
  unit/main.cpp
  unit/crop_math_test.cpp
  unit/zoom_test.cpp
  unit/project_config_test.cpp
  unit/test_pdfs_test.cpp
  unit/medium_il_test.cpp
  unit/temp_dir_test.cpp
  unit/registry_sandbox_test.cpp)
pdft_harden(pdft_unit_tests TEST)
target_link_libraries(pdft_unit_tests PRIVATE pdft_testsupport pdft_common pdft_config GTest::gtest
                                              bcrypt shell32 ole32 uuid)
add_dependencies(pdft_unit_tests pdft_run_medium)   # RunMediumTool tests
pdft_add_program_test(unit.tests pdft_unit_tests LABELS unit)

if(TARGET thumbtest)
  if(NOT CMAKE_CROSSCOMPILING)   # it runs thumbtest
    add_test(NAME unit.samples
      COMMAND "${CMAKE_COMMAND}" "-DTHUMBTEST=$<TARGET_FILE:thumbtest>"
              "-DCOMMITTED_DIR=${PROJECT_SOURCE_DIR}/src/app/res"
              "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/samples"
              -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckSamples.cmake")
    set_tests_properties(unit.samples PROPERTIES LABELS unit)
  endif()
  # No CMAKE_COMMAND in a bundle either: it runs the cmake on PATH.
  pdft_bundle_test(unit.samples
    COMMAND cmake "-DTHUMBTEST=./$<TARGET_FILE_NAME:thumbtest>" -DCOMMITTED_DIR=samples
            -DWORK_DIR=samples-work -P CheckSamples.cmake
    PROPERTIES LABELS unit)
  install(FILES CheckSamples.cmake DESTINATION . COMPONENT tests)
  install(FILES "${PROJECT_SOURCE_DIR}/src/app/res/sample-document.pdf"
                "${PROJECT_SOURCE_DIR}/src/app/res/sample-slide.pdf"
          DESTINATION samples COMPONENT tests)
endif()

# ---- handler -------------------------------------------------------------------------------
add_library(pdft_handler_host STATIC handler/handler_host.cpp handler/handler_host.h)
pdft_harden(pdft_handler_host LIB)
target_link_libraries(pdft_handler_host PUBLIC pdft_testsupport pdft_config pdft_common
                                               ole32 uuid shlwapi gdi32 user32)

add_executable(pdft_handler_tests
  handler/main.cpp
  handler/handler_env.h
  handler/render_dimensions_test.cpp
  handler/render_errors_test.cpp
  handler/zoom_precedence_test.cpp
  handler/stress_test.cpp)
pdft_harden(pdft_handler_tests TEST)
target_link_libraries(pdft_handler_tests PRIVATE pdft_handler_host GTest::gtest)
add_dependencies(pdft_handler_tests pdft_handler)   # PdfThumbnails.dll (+ PDFium) next to it
# Serial: the stress test wants the machine to itself, and its timing budgets are 1.1's.
pdft_add_program_test(handler.tests pdft_handler_tests LABELS handler RUN_SERIAL TRUE)

add_executable(pdft_refuses_elevated handler/refuses_elevated.cpp)
pdft_harden(pdft_refuses_elevated TEST)
target_link_libraries(pdft_refuses_elevated PRIVATE pdft_handler_host)
add_dependencies(pdft_refuses_elevated pdft_handler)
pdft_add_program_test(handler.refuses_elevated pdft_refuses_elevated
                      LABELS handler SKIP_RETURN_CODE 77)

# ---- installed test bundle -----------------------------------------------------------------
get_property(_pdft_bundle_tests GLOBAL PROPERTY PDFT_BUNDLE_TESTS)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle/CTestTestfile.cmake"
     CONTENT "# Generated by tests/CMakeLists.txt: the tests of the installed test bundle.\n# Run: ctest --test-dir <this folder> -L \"unit|handler\"\n${_pdft_bundle_tests}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/bundle/CTestTestfile.cmake"
        DESTINATION . COMPONENT tests)
unset(_pdft_bundle_tests)
