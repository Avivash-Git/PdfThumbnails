# gate.buildid: cmake/BuildId.cmake computes the 1.x build-folder formula, and names the staged
# DLLs "<project version>-<8 hex digits>" with the version read from the DLL's own resource.
#   cmake -DSCRIPT=<cmake/BuildId.cmake> -DWORK_DIR=<dir> -DDLL=<staged handler>
#         -DPDFIUM=<staged pdfium> -DVERSION=<a.b.c.d> -P CheckBuildId.cmake
# The fixed vector below was computed independently with the 1.1 manager's PowerShell code
# (Install-Files: Get-FileHash ... -join '', SHA256 over the ASCII, first 4 bytes, 'x2').
# (The 1.1 DLLs give 1.1.0.0-67a1323e.)
# The app's C++ ComputeBuildId() is compared with this script by the integration tests
# (tests/integration/build_info_test.cpp), on the DLLs the build puts next to them.

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(SCRIPT WORK_DIR DLL PDFIUM VERSION)

# pdft_run_build_id(<out-var> <-D args>...)
function(pdft_run_build_id out)
  execute_process(COMMAND "${CMAKE_COMMAND}" ${ARGN} -P "${SCRIPT}"
                  RESULT_VARIABLE rc OUTPUT_VARIABLE text ERROR_VARIABLE err
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT rc EQUAL 0)
    pdft_gate_fail("BuildId.cmake ${ARGN} failed (${rc}): ${err}")
  endif()
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# 1. Fixed vector.
file(REMOVE_RECURSE "${WORK_DIR}")
# No line ends: file(WRITE) writes CRLF on Windows.
file(WRITE "${WORK_DIR}/a.bin" "build-id test vector: dll")
file(WRITE "${WORK_DIR}/b.bin" "build-id test vector: pdfium")
pdft_run_build_id(vector "-DDLL=${WORK_DIR}/a.bin" "-DPDFIUM=${WORK_DIR}/b.bin" -DVERSION=1.2.3.4
                  "-DOUTPUT_FILE=${WORK_DIR}/id.txt")
set(expected "1.2.3.4-ff226b2d")
if(NOT vector STREQUAL expected)
  pdft_gate_fail("the fixed vector gives '${vector}', expected '${expected}'")
endif()
file(READ "${WORK_DIR}/id.txt" written)
if(NOT written STREQUAL expected)
  pdft_gate_fail("OUTPUT_FILE holds '${written}', expected '${expected}'")
endif()
file(REMOVE_RECURSE "${WORK_DIR}")

# 2. The staged DLLs, with the FileVersion read from the handler.
pdft_run_build_id(staged "-DDLL=${DLL}" "-DPDFIUM=${PDFIUM}")
string(REPLACE "." "\\." version_regex "${VERSION}")
set(hex "[0-9a-f]")
if(NOT staged MATCHES "^${version_regex}-${hex}${hex}${hex}${hex}${hex}${hex}${hex}${hex}$")
  pdft_gate_fail("the staged DLLs give '${staged}', expected '${VERSION}-<8 hex digits>'")
endif()
pdft_gate_ok("fixed vector ${vector}, staged build folder name ${staged}")
