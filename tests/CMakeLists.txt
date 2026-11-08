# Test programs (GoogleTest) and their CTest registrations. One CTest test per program, so
# nothing runs at build time and cross-compiled ARM64 builds work; run them with
#   ctest --preset x64-release -L "unit|handler|integration"
#
#   unit.tests                pdft_unit_tests: crop math, zoom clamping, the generated GUIDs,
#                             test-PDF generator (byte-identical to the 1.x generator),
#                             Medium-integrity support, temp folders, HKCU redirection, the
#                             thumbnail refresh experiment's verdict (which zoom a bitmap's size
#                             shows, the folder rules, its record files under %TEMP%); the app's
#                             pure parts: command line, strings, paths, log, output, exit codes,
#                             virtualization verdicts, named-object descriptors, start-up
#                             hardening, the COM descriptor, the executables' string tables,
#                             registry value decoding, PE header and version-resource parsing
#                             (synthetic files), build ids, slot and zoom decisions, the 1.x
#                             leftover classifier, the Smart App Control verdict, task
#                             triggers, the Explorer test's probe PDF and --test-file checks,
#                             Explorer settings, whether Windows restarts File Explorer and the
#                             Windows build string; the thumbnail-cache rebuild's decisions (the
#                             cache-file name, the refusal order, how a holder is classified and
#                             ended, the wait for File Explorer, the reopen plan, the exit code
#                             and the report); the PDF thumbnail refresh's decisions (the flags it
#                             allows, the refusal order, which files it skips, grouping by
#                             folder) and its engine against a fake thumbnail cache and a fake
#                             File Explorer on a fake clock (what is extracted, sizes dropped,
#                             stopping, progress, notifications); the check's findings, repair
#                             plans and exit codes;
#                             the reports: the status text line by line, the JSON status
#                             (escaping, schema 1, checked by a strict JSON reader) and the
#                             diagnostics' masking; the --fix confirmation rules; the install
#                             side's pure parts (RunOnce clean-up names and command lines, the
#                             broad-group permission check on SDDL descriptors, the elevated steps
#                             and their one command line, the elevated side's entry checks, exit
#                             codes, the report lines, the token facts and the trace of where a
#                             step failed, the installer's records, which DLL holders may be
#                             ended); the 1.x clean-up's pure parts (the table of 1.x files, the
#                             plan for 1.x's per-user folder on fake trees of both layouts, the
#                             plans for build folders for all users) and the installer step's
#                             task options; the install and uninstall side (a build's files and
#                             versions, the class path for all users, the user hives cleaned,
#                             the per-user files removed, trash folder names, exit codes, the
#                             texts of --install, --uninstall and --zoom); pdft-run-outside's
#                             pure parts (its request file, command lines, exit codes); the
#                             system tests' account rules (when they may be made, their names,
#                             their passwords from random bytes, how the standard account's run
#                             of the task tests is read and judged)
#   unit.samples              src/app/res/sample-*.pdf == `thumbtest makesamples` output
#   integration.tests         pdft_integration_tests: the app's core against the real system
#                             (COM under the app's security, WMI, Task Scheduler, named objects
#                             across processes, the process tree, the registry canary)
#                             and both executables run with captured output; the registration,
#                             settings, build ids, DLL headers and 1.x leftovers through private
#                             registry keys and temporary folders; read-only: Smart App Control,
#                             the Code Integrity log, PdfCraft (Windows Installer, Uninstall
#                             keys), the check task and 1.x's task, Explorer settings, the
#                             Windows build, File Explorer's folder windows (counts only; one
#                             window on a folder of its own is opened through the shell, and
#                             closed, only on runners GitHub hosts), whether Windows restarts File
#                             Explorer; the thumbnail-cache rebuild against a fake system (ending
#                             order, deleting, waiting for File Explorer, reopening windows, the
#                             log's counts) and the cache-file listing on a temp folder; finding
#                             PDFs in temp folders for the PDF thumbnail refresh (never into links,
#                             its limits), checking its --folder folders, and the refresh of a temp
#                             folder against a fake thumbnail cache (the log's counts); the whole
#                             check (gathering, repairs, the record, the lock, also against a
#                             process below Medium integrity) through private keys, folders under
#                             %TEMP% and fakes for everything Windows-wide, --checks the same way;
#                             probe PDFs in %TEMP% (deleted); the diagnostics' log reader on files
#                             under %TEMP%; the check task written (each checkbox combination, the
#                             folder or the root-folder name, one copy per user, the check's
#                             repair of a stale task, a run through Task Scheduler) in a task
#                             folder and under root-folder names of its own, deleted afterwards
#                             (elevated also the protected task folder and the uninstall's removal
#                             of every user's task); --status (text and JSON) and --diagnostics of
#                             both executables against the real state, read-only, without the
#                             Explorer test; --checks of both only where a guard refuses it (8,
#                             or 6 where nothing is installed); the install side's file
#                             operations under %TEMP% (reparse point checks, the verified copy,
#                             removal without following links, files held open or mapped as an
#                             image, the trash folder, the build-folder permission check on
#                             folders it gives known permissions) and its RunOnce values in
#                             private keys; deleting at restart only checked to be refused
#                             unelevated; Restart Manager and ending a holder only on its own
#                             children; the elevation report pipe between itself and its
#                             children through fake launchers (never a real elevation), with the
#                             reporter's owner, session and write-time checks, the asking side
#                             reading on past its cap and a pipe name another process took; the
#                             settings executable's --elevated step refused outside the install
#                             folder; deleting files by their contents, the 1.x clean-up (user
#                             part, file part, machine part), --setup-user, --migrate-v1 (with a
#                             fake launcher) and the check's RunOnce clean-up against folders under
#                             %TEMP%, private keys and the fake probes (elevated, --setup-machine
#                             in a test task folder); --setup-user and --migrate-v1 of both
#                             executables only where a guard refuses them (8), --setup-machine
#                             only unelevated (7); --install, --uninstall and --uninstall-cleanup
#                             (copying a build into an install root under %TEMP%, the steps for
#                             all users run in this process through fake launchers, the
#                             uninstall's clean-up of made-up user hives in a private key, the
#                             installer's uninstaller through a fake), --fix's administrator
#                             step through a fake launcher; --install, --uninstall and --zoom <n>
#                             of both executables only where a guard refuses them (8) or the
#                             command line is wrong (2); never extracts a thumbnail;
#                             writes nothing to the real profile but the canary's volatile probe
#                             key, those private keys (HKCU\Software\<product>-Test\<GUID>,
#                             deleted afterwards) and, unelevated, the start and end lines that
#                             every command run appends to the app's log, and its own test tasks
#   handler.tests             pdft_handler_tests: PdfThumbnails.dll in-process at Medium
#                             integrity (relaunches itself as a standard user, Administrators
#                             deny-only, when elevated or when Administrators is enabled) with
#                             HKCU redirected to a private key: crop table, pixels, COM surface,
#                             error HRESULTs, where the zoom comes from (preview override, HKCU,
#                             settings.ini), 8-thread stress
#   handler.refuses_elevated  pdft_refuses_elevated: elevated, the DLL must refuse to render, and
#                             so must a copy of the program started with its token lowered only
#                             to Medium (Administrators still enabled; that part runs only where
#                             Administrators is enabled and prints a skip otherwise); skipped
#                             (exit 77) at Medium
#   system.tests              pdft_system_tests (label system, never in the test presets): the
#                             install for all users, --status, --checks, --zoom, --setup-user,
#                             --setup-machine, --fix's administrator step, --uninstall and
#                             --uninstall-cleanup for real from a staged runtime, 1.1's state
#                             (for all users and per user) replaced by the install and by
#                             --migrate-v1, the administrator steps through UAC and their report
#                             through the asking process's pipe, and nothing left afterwards;
#                             the user is a local administrator account the program makes for
#                             the run (its limited token, so runas goes through UAC) and deletes
#                             afterwards; a second one, a standard account, runs the integration
#                             tests' task tests (TaskWriting must pass, TaskFolderElevated must
#                             skip) and its own --setup-user into the protected task folder the
#                             install made; as the test account, with thumbtest: the thumbnail
#                             refresh experiment's automatic part, --refresh --full refused while
#                             File Explorer is another account's, and what the thumbnail cache
#                             keeps once the uninstall has unregistered the handler (printed, not
#                             judged); runs only with PDFT_SYSTEM_TESTS=1 and GITHUB_ACTIONS=
#                             true with RUNNER_ENVIRONMENT=github-hosted or
#                             PDFT_SYSTEM_TESTS_THROWAWAY_VM=1, elevated, where nothing of 1.x or
#                             2.x is installed (the CI job system-x64), else skipped (77)
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
  unit/registry_sandbox_test.cpp
  unit/refresh_judge_test.cpp)
if(TARGET pdft_appcore)
  target_sources(pdft_unit_tests PRIVATE
    unit/build_folder_test.cpp
    unit/cli_args_test.cpp
    unit/context_decision_test.cpp
    unit/diagnostics_test.cpp
    unit/elevation_test.cpp
    unit/evaluate_test.cpp
    unit/explorer_settings_test.cpp
    unit/failure_trace_test.cpp
    unit/file_ops_test.cpp
    unit/install_test.cpp
    unit/json_test.cpp
    unit/legacy_test.cpp
    unit/live_test_test.cpp
    unit/log_test.cpp
    unit/migrate_v1_test.cpp
    unit/output_test.cpp
    unit/paths_test.cpp
    unit/pe_info_test.cpp
    unit/preview_protocol_test.cpp
    unit/program_output_test.cpp
    unit/rebuild_test.cpp
    unit/refresh_test.cpp
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
    unit/system_account_test.cpp
    unit/tasks_test.cpp
    # The pure rules of the system tests' accounts (when they may be made, their names and
    # passwords, how the standard account's run of the task tests is judged).
    system/account_rules.cpp system/account_rules.h)
  target_include_directories(pdft_unit_tests PRIVATE system)
  target_link_libraries(pdft_unit_tests PRIVATE pdft_appcore pdft_windowcore)
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
# once through Task Scheduler; never starts a thumbnail extraction or explorer.exe. Only on
# runners GitHub hosts does it open a File Explorer window (on a folder of its own, closed again)
# and run copies of itself inside jobs of its own, which start it suspended and end it at once.
# Its named objects carry a suffix of the run's own, so it never takes or waits for a running
# app's.
if(TARGET pdft_appcore)
  add_executable(pdft_integration_tests
    integration/main.cpp
    integration/build_info_test.cpp
    integration/check_test.cpp
    integration/child_process.cpp integration/child_process.h
    integration/com_security_test.cpp
    integration/context_test.cpp
    integration/diagnostics_files_test.cpp
    integration/elevation_test.cpp
    integration/exes_test.cpp
    integration/fake_probes.h
    integration/file_ops_test.cpp
    integration/github_runner.h
    integration/install_test.cpp
    integration/instance_test.cpp
    integration/live_test_files_test.cpp
    integration/fake_rebuild.h
    integration/probes_test.cpp
    integration/processes_test.cpp
    integration/program_run_test.cpp
    integration/rebuild_test.cpp
    integration/refresh_test.cpp
    integration/registration_test.cpp
    integration/render_preview_test.cpp
    integration/search_index_test.cpp
    integration/settings_test.cpp
    integration/setup_test.cpp
    integration/shell_windows_test.cpp
    integration/tasks_test.cpp
    integration/window_messages_test.cpp
    integration/zoom_change_test.cpp)
  pdft_harden(pdft_integration_tests TEST)
  target_link_libraries(pdft_integration_tests PRIVATE pdft_appcore pdft_windowcore
                                                       pdft_testsupport GTest::gtest
                                                       ole32 oleaut32 advapi32 user32)
  # The handler and PDFium next to it are read (headers, version, build id), never loaded.
  # ... and the settings executable's preview child runs for real, with the handler next to it.
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

# ---- system --------------------------------------------------------------------------------
# Installs, uses and uninstalls the app for real (HKLM, HKCU, Program Files, Task Scheduler, UAC)
# from a staged runtime, as a local administrator account it makes for the run and deletes
# afterwards, with a standard account made the same way for the task tests (it runs
# pdft_integration_tests next to it); the administrator account also runs thumbtest next to it on
# its own thumbnail cache: only on a throw-away PC (the CI job system-x64). The
# program itself refuses to run unless PDFT_SYSTEM_TESTS=1 and GITHUB_ACTIONS=true with
# RUNNER_ENVIRONMENT=github-hosted or PDFT_SYSTEM_TESTS_THROWAWAY_VM=1, it is elevated and nothing
# of 1.x or 2.x is installed; the test presets leave the label out. 1.x's shipped files for its
# migration fixture are copied next to it (system-fixture), in the build folder and in the test
# bundle.
if(TARGET pdft_appcore)
  add_executable(pdft_system_tests
    system/main.cpp
    system/account_rules.cpp system/account_rules.h
    system/system_env.cpp system/system_env.h
    system/system_test.cpp
    system/test_account.cpp system/test_account.h
    integration/child_process.cpp integration/child_process.h)
  target_include_directories(pdft_system_tests PRIVATE integration)
  pdft_harden(pdft_system_tests TEST)
  target_link_libraries(pdft_system_tests PRIVATE pdft_appcore pdft_testsupport GTest::gtest
                                                  ole32 oleaut32 advapi32 netapi32 userenv
                                                  bcrypt shell32 wtsapi32)
  # The test accounts' tokens are checked with pdft-run-medium --check-token, and the standard
  # account runs the integration tests' task tests (with pdft-run-medium for their tasks).
  add_dependencies(pdft_system_tests pdft_run_medium pdft_integration_tests)
  # The test account runs thumbtest for the thumbnail refresh (the test bundle carries it too).
  if(TARGET thumbtest)
    add_dependencies(pdft_system_tests thumbtest)
  endif()
  set(_pdft_v1_fixture "${PROJECT_SOURCE_DIR}/PdfThumbnails.ps1"
                       "${PROJECT_SOURCE_DIR}/PdfThumbnails.cmd" "${PROJECT_SOURCE_DIR}/LICENSE")
  add_custom_command(TARGET pdft_system_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:pdft_system_tests>/system-fixture"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_pdft_v1_fixture}
            "$<TARGET_FILE_DIR:pdft_system_tests>/system-fixture"
    VERBATIM)
  install(FILES ${_pdft_v1_fixture} DESTINATION system-fixture COMPONENT tests)
  unset(_pdft_v1_fixture)
  pdft_add_program_test(system.tests pdft_system_tests LABELS system RUN_SERIAL TRUE
                        SKIP_RETURN_CODE 77 TIMEOUT 2400)
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
