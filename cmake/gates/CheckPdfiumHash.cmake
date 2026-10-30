# gate.pdfium.hash: the staged PDFium DLL is byte-for-byte the pinned one (Pins.cmake,
# PDFT_PDFIUM_<arch>_DLL_SHA256).
#   cmake -DFILE=<staged pdfium dll> -DEXPECTED=<sha256> -P CheckPdfiumHash.cmake

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(FILE EXPECTED)

if(NOT EXISTS "${FILE}")
  pdft_gate_fail("${FILE} is not staged")
endif()
file(SHA256 "${FILE}" actual)
string(TOLOWER "${EXPECTED}" expected)
if(NOT actual STREQUAL expected)
  pdft_gate_fail("${FILE} has SHA-256 ${actual}, the pin is ${expected}")
endif()
pdft_gate_ok("${FILE} SHA-256 ${actual} matches the pin")
