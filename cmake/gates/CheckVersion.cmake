# gate.version.<bin>: the VERSIONINFO resource carries the project version (the StringFileInfo
# FileVersion string, which names the install folder, and the fixed binary version) and an
# OriginalFilename equal to the file's name.
#   cmake -DFILE=<image> -DVERSION=<a.b.c.d> -P CheckVersion.cmake

cmake_minimum_required(VERSION 3.31)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(FILE VERSION)

pdft_pe_version(v "${FILE}" FileVersion OriginalFilename)
cmake_path(GET FILE FILENAME name)
set(problems "")
if(NOT v_FileVersion STREQUAL VERSION)
  list(APPEND problems "FileVersion string '${v_FileVersion}', expected '${VERSION}'")
endif()
if(NOT v_FIXED STREQUAL VERSION)
  list(APPEND problems "fixed file version '${v_FIXED}', expected '${VERSION}'")
endif()
if(NOT v_OriginalFilename STREQUAL name)
  list(APPEND problems "OriginalFilename '${v_OriginalFilename}', expected '${name}'")
endif()
if(problems)
  list(JOIN problems "; " problems)
  pdft_gate_fail("${FILE}: ${problems}")
endif()
pdft_gate_ok("${FILE}: FileVersion ${v_FileVersion} (fixed ${v_FIXED}), OriginalFilename ${v_OriginalFilename}")
