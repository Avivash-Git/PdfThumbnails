# pdft-run-outside.exe: runs a program as the signed-in user in that user's normal desktop
# session, through a temporary scheduled task, so checks started from a sandboxed or containerized
# terminal see the real registry and Explorer (see run_outside.cpp). Developer tool: in no install
# component, never shipped.
#   pdft_run_outside_core  STATIC  the parts without system state (unit-tested in pdft_unit_tests)
#   pdft_run_outside       EXE     the tool
# Command-line quoting and the Application Control check come from tests/support.

add_library(pdft_run_outside_core STATIC run_outside_core.cpp run_outside_core.h)
target_include_directories(pdft_run_outside_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
pdft_harden(pdft_run_outside_core LIB)
target_link_libraries(pdft_run_outside_core PUBLIC pdft_testsupport)

add_executable(pdft_run_outside run_outside.cpp)
set_target_properties(pdft_run_outside PROPERTIES OUTPUT_NAME pdft-run-outside)
pdft_add_version_resource(pdft_run_outside FILETYPE VFT_APP
  DESCRIPTION "${PDFT_PRODUCT_NAME} run-outside developer tool")
# asInvoker, long paths, and no console window for the copy that Task Scheduler starts.
pdft_add_manifest(pdft_run_outside TEMPLATE run-outside)
pdft_harden(pdft_run_outside EXE)
# Task Scheduler is used through COM (taskschd.h); its GUIDs come from __uuidof.
target_link_libraries(pdft_run_outside PRIVATE pdft_run_outside_core pdft_testsupport pdft_config
                                               ole32 oleaut32 advapi32)
