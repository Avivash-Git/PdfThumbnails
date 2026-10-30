# The install folder name of a build, "<FileVersion>-<build id>", from the final bytes of the
# two DLLs (the formula of 1.x, unchanged):
#   build id = lower-hex(first 4 bytes of SHA-256(ASCII(UPPER-HEX(SHA-256(dll)) +
#                                                       UPPER-HEX(SHA-256(pdfium)))))
#   FileVersion = the handler's StringFileInfo FileVersion string (or -DVERSION=...)
#
#   cmake -DDLL=<PdfThumbnails.dll> -DPDFIUM=<PdfThumbnails-pdfium.dll> [-DVERSION=<a.b.c.d>]
#         [-DOUTPUT_FILE=<file>] -P cmake/BuildId.cmake
# (-D options go before -P.) Prints the name on stdout and, with OUTPUT_FILE, writes it there
# (just the name, no line end).
# Release builds run it after code signing, because signing changes the bytes (M3/M4).

cmake_minimum_required(VERSION 3.31)
include("${CMAKE_CURRENT_LIST_DIR}/gates/PeTools.cmake")
pdft_require(DLL PDFIUM)

foreach(file IN ITEMS "${DLL}" "${PDFIUM}")
  if(NOT EXISTS "${file}")
    message(FATAL_ERROR "${file} does not exist")
  endif()
endforeach()

if(NOT VERSION)
  pdft_pe_version(dll "${DLL}" FileVersion)
  set(VERSION "${dll_FileVersion}")
  if(NOT VERSION)
    message(FATAL_ERROR "${DLL} has no FileVersion string; pass -DVERSION=<a.b.c.d>")
  endif()
endif()

file(SHA256 "${DLL}" dll_hash)
file(SHA256 "${PDFIUM}" pdfium_hash)
string(TOUPPER "${dll_hash}${pdfium_hash}" hashes)
string(SHA256 id "${hashes}")
string(SUBSTRING "${id}" 0 8 id)
string(TOLOWER "${id}" id)
set(name "${VERSION}-${id}")

if(OUTPUT_FILE)
  file(WRITE "${OUTPUT_FILE}" "${name}")   # no line end (file(WRITE) would make it CRLF)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${name}")
