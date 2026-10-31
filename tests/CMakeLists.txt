# Test programs (GoogleTest) and their CTest registrations. One CTest test per program, so
# nothing runs at build time and cross-compiled ARM64 builds work; run them with
#   ctest --preset x64-release -L "unit|handler|integration"
#
#   unit.tests                pdft_unit_tests: crop math, zoom clamping, the generated GUIDs,
#                             test-PDF generator (byte-identical to the 1.x generator),
#                             Medium-integrity support, temp folders, HKCU redirection; the app's
#                             pure parts: command line, strings, paths, log, output, exit codes,
#                             virtualization verdicts, named-object descriptors, start-up
#                             hardening, the COM descriptor, the executables' string tables,
#                             registry value decoding, PE header and version-resource parsing
#                             (synthetic files), build ids, slot and zoom decisions, the 1.x
#                             leftover classifier, the Smart App Control verdict, task
#                             triggers, the Explorer test's probe PDF and --test-file checks,
#                             Explorer settings and the Windows build string; the check's
#                             findings, repair plans and exit codes; the reports: the status
#                             text line by line, the JSON status (escaping, schema 1, checked by
#                             a strict JSON reader) and the diagnostics' masking; the --fix
#                             confirmation rules; pdft-run-outside's pure parts (its request
#                             file, command lines, exit codes)
#   unit.samples              src/app/res/sample-*.pdf == `thumbtest makesamples` output
#   integration.tests         pdft_integration_tests: the app's core against the real system
#                             (COM under the app's security, WMI, Task Scheduler, named objects
#                             across processes, the process tree, the registry canary)
#                             and both executables run with captured output; the registration,
#                             settings, build ids, DLL headers and 1.x leftovers through private
#                             registry keys and temporary folders; read-only: Smart App Control,
#                             the Code Integrity log, PdfCraft (Windows Installer, Uninstall
#                             keys), the check task and 1.x's task, Explorer settings, the
#                             Windows build; the whole check (gathering, repairs, the record,
#                             the lock, also against a process below Medium integrity) through
#                             private keys, folders under %TEMP% and fakes for everything
#                             Windows-wide, --checks the same way; probe PDFs in %TEMP%
#                             (deleted); the diagnostics' log reader on files under %TEMP%; the
#                             check task written (each checkbox combination, the folder or the
#                             root-folder name, one copy per user, the check's repair of a stale
#                             task, a run through Task Scheduler) in a task folder and under
#                             root-folder names of its own, deleted afterwards (elevated also the
#                             protected task folder and the uninstall's removal of every user's
#                             task); --status (text and JSON) and --diagnostics of both
#                             executables against the real state, read-only, without the
#                             Explorer test; --checks of both only where a guard refuses it (8,
#                             or 6 where nothing is installed); never extracts a thumbnail;
#                             writes nothing to the real profile but the canary's volatile probe
#                             key, those private keys (HKCU\Software\<product>-Test\<GUID>,
#                             deleted afterwards) and, unelevated, the start and end lines that
#                             every command run appends to the app's log, and its own test tasks
#   handler.tests             pdft_handler_tests: PdfThumbnails.dll in-process at Medium
#                             integrity (relaunches itself when elevated) with HKCU redirected to
#                             a private key: crop table, pixels, COM surface, error HRESULTs,
#                             where the zoom comes from (preview override, HKCU, settings.ini),
#                             8-thread stress
#   handler.refuses_elevated  pdft_refuses_elevated: elevated, the DLL must refuse to render;
#                             skipped (exit 77) when not elevated
# Also built here: pdft-planting-check and its canary DLL, for the gate gate.planting.
# Test support (tests/support) is added by the top-level CMakeLists.txt: thumbtest uses it too.
#
# The same tests also run from the installed test bundle, a flat folder made with
#   cmake --install <build> --prefix <folder> --component tests
#   ctest --test-dir <folder> -L "unit|handler|integration"
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
if(TARGET pdft_appcore)
  target_sources(pdft_unit_tests PRIVATE
    unit/build_folder_test.cpp
    unit/cli_args_test.cpp
    unit/context_decision_test.cpp
    unit/diagnostics_test.cpp
    unit/evaluate_test.cpp
    unit/explorer_settings_test.cpp
    unit/json_test.cpp
    unit/legacy_test.cpp
    unit/live_test_test.cpp
    unit/log_test.cpp
    unit/output_test.cpp
    unit/paths_test.cpp
    unit/pe_info_test.cpp
    unit/registration_test.cpp
    unit/registry_roots_test.cpp
    unit/repair_test.cpp
    unit/resources_test.cpp
    unit/sac_test.cpp
    unit/settings_test.cpp
    unit/snapshot_fixtures.h
    unit/startup_test.cpp
    unit/status_format_test.cpp
    unit/strings_test.cpp
    unit/tasks_test.cpp)
  target_link_libraries(pdft_unit_tests PRIVATE pdft_appcore)
  # main.cpp's start-up hardening child needs the app's start-up code (startup.h).
  target_compile_definitions(pdft_unit_tests PRIVATE PDFT_UNIT_TESTS_WITH_APP=1)
  # The string-table test reads the executables next to the test program, and the hardening
  # child checks that PDFium next to it no longer loads by bare name (the test bundle ships it
  # there too).
  add_dependencies(pdft_unit_tests PdfThumbnailsCli PdfThumbnailsSettings pdft_pdfium_runtime)
endif()
if(TARGET pdft_run_outside_core)   # tools/run-outside (with PDFT_BUILD_TOOLS)
  target_sources(pdft_unit_tests PRIVATE unit/run_outside_test.cpp)
  target_link_libraries(pdft_unit_tests PRIVATE pdft_run_outside_core)
endif()
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
# Exit 77: an Application Control policy blocked PdfThumbnails.dll, so no test ran.
pdft_add_program_test(handler.tests pdft_handler_tests LABELS handler RUN_SERIAL TRUE
                      SKIP_RETURN_CODE 77)

add_executable(pdft_refuses_elevated handler/refuses_elevated.cpp)
pdft_harden(pdft_refuses_elevated TEST)
target_link_libraries(pdft_refuses_elevated PRIVATE pdft_handler_host)
add_dependencies(pdft_refuses_elevated pdft_handler)
pdft_add_program_test(handler.refuses_elevated pdft_refuses_elevated
                      LABELS handler SKIP_RETURN_CODE 77)

# ---- integration ---------------------------------------------------------------------------
# Writes nothing to the real profile except the registry canary's volatile probe key and private
# test keys (HKCU\Software\<product>-Test\<GUID>), which it deletes, and the start and end lines
# the executables' read-only commands (--status, --diagnostics) append to the app's log when not
# elevated, and test tasks in a Task Scheduler folder and under root-folder names of its own,
# which it deletes; starts itself (child modes in integration/main.cpp), the two executables, a
# writing command of theirs only where its writes are swallowed anyway, and pdft-run-medium.exe
# once through Task Scheduler; never starts a thumbnail extraction. Its named objects carry a
# suffix of the run's own, so it never takes or waits for a running app's.
if(TARGET pdft_appcore)
  add_executable(pdft_integration_tests
    integration/main.cpp
    integration/build_info_test.cpp
    integration/check_test.cpp
    integration/child_process.cpp integration/child_process.h
    integration/com_security_test.cpp
    integration/context_test.cpp
    integration/diagnostics_files_test.cpp
    integration/exes_test.cpp
    integration/instance_test.cpp
    integration/live_test_files_test.cpp
    integration/probes_test.cpp
    integration/registration_test.cpp
    integration/settings_test.cpp
    integration/tasks_test.cpp)
  pdft_harden(pdft_integration_tests TEST)
  target_link_libraries(pdft_integration_tests PRIVATE pdft_appcore pdft_testsupport GTest::gtest
                                                       ole32 oleaut32 advapi32 user32)
  # The handler and PDFium next to it are read (headers, version, build id), never loaded.
  add_dependencies(pdft_integration_tests PdfThumbnailsCli PdfThumbnailsSettings pdft_handler
                                          pdft_pdfium_runtime)
  # The check task tests point their tasks at pdft-run-medium.exe, and run it once (--exit 0).
  add_dependencies(pdft_integration_tests pdft_run_medium)
  pdft_add_program_test(integration.tests pdft_integration_tests LABELS integration)
  if(NOT CMAKE_CROSSCOMPILING)
    # The build tree has cmake and the build-id script, so the app's build id is compared with
    # the script's; the test bundle has neither and skips that comparison.
    set_tests_properties(integration.tests PROPERTIES ENVIRONMENT
      "PDFT_TEST_CMAKE=${CMAKE_COMMAND};PDFT_TEST_BUILDID_SCRIPT=${PROJECT_SOURCE_DIR}/cmake/BuildId.cmake")
  endif()
endif()

# ---- DLL planting (gate.planting, registered in cmake/Gates.cmake) --------------------------
# The canary DLL and the program that plants it next to copies of the app's executables. Built
# for both architectures; the gate runs only where the executables can (native builds).
if(TARGET PdfThumbnailsSettings AND TARGET PdfThumbnailsCli)
  # A module: no exports and no import library.
  add_library(pdft_planting_canary MODULE planting/canary.cpp)
  set_target_properties(pdft_planting_canary PROPERTIES OUTPUT_NAME pdft-planting-canary PREFIX "")
  pdft_harden(pdft_planting_canary DLL)

  add_executable(pdft_planting_check planting/planting_check.cpp)
  set_target_properties(pdft_planting_check PROPERTIES OUTPUT_NAME pdft-planting-check)
  pdft_add_version_resource(pdft_planting_check FILETYPE VFT_APP
    DESCRIPTION "${PDFT_PRODUCT_NAME} DLL planting check (tests)")
  pdft_harden(pdft_planting_check EXE)
  target_link_libraries(pdft_planting_check PRIVATE pdft_testsupport pdft_config)
  add_dependencies(pdft_planting_check pdft_planting_canary)
endif()

# ---- installed test bundle -----------------------------------------------------------------
get_property(_pdft_bundle_tests GLOBAL PROPERTY PDFT_BUNDLE_TESTS)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bundle/CTestTestfile.cmake"
     CONTENT "# Generated by tests/CMakeLists.txt: the tests of the installed test bundle.\n# Run: ctest --test-dir <this folder> -L \"unit|handler|integration\"\n${_pdft_bundle_tests}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/bundle/CTestTestfile.cmake"
        DESTINATION . COMPONENT tests)
unset(_pdft_bundle_tests)
