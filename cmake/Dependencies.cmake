# Third-party code, all of it downloaded against the pins in Pins.cmake.
#
# PDFium (per architecture):
#   pdft_pdfium_headers  INTERFACE, SYSTEM include path for the headers only. Never link
#                        pdfium.dll.lib or use the archive's PDFiumConfig.cmake: they import
#                        pdfium.dll by name, i.e. through the DLL search path. The handler loads
#                        its private copy by full path at run time instead.
#   pdft_pdfium_runtime  copies bin/pdfium.dll to <build>/bin/${PDFT_PDFIUM_DLL}.
#   PDFT_PDFIUM_DIR      the extracted archive (licenses, headers).
#   PDFT_PDFIUM_RUNTIME  the private-name copy next to the handler.

include_guard(GLOBAL)

set(PDFT_PDFIUM_ARCHIVE "" CACHE FILEPATH
    "Offline builds: a local pdfium-win-<arch>.tgz to use instead of downloading (checked against the pin)")

pdft_download_verified(_pdft_pdfium_archive
  URL "${PDFT_PDFIUM_${PDFT_ARCH}_URL}"
  SHA256 "${PDFT_PDFIUM_${PDFT_ARCH}_SHA256}"
  LOCAL_FILE "${PDFT_PDFIUM_ARCHIVE}")
set(PDFT_PDFIUM_DIR "${CMAKE_BINARY_DIR}/_deps/pdfium-${PDFT_ARCH}")
pdft_extract_once("${_pdft_pdfium_archive}" "${PDFT_PDFIUM_${PDFT_ARCH}_SHA256}" "${PDFT_PDFIUM_DIR}")
unset(_pdft_pdfium_archive)

# The DLL we ship must be exactly the pinned one, also when the extracted tree is reused.
set(_pdft_pdfium_dll "${PDFT_PDFIUM_DIR}/bin/pdfium.dll")
if(NOT EXISTS "${_pdft_pdfium_dll}")
  message(FATAL_ERROR "${_pdft_pdfium_dll} is missing: the PDFium archive layout changed")
endif()
file(SHA256 "${_pdft_pdfium_dll}" _pdft_actual)
if(NOT PDFT_PDFIUM_${PDFT_ARCH}_DLL_SHA256)
  message(FATAL_ERROR "No pin for the ${PDFT_ARCH} bin/pdfium.dll yet. After checking the archive, set "
                      "PDFT_PDFIUM_${PDFT_ARCH}_DLL_SHA256 in cmake/Pins.cmake to ${_pdft_actual}")
endif()
if(NOT _pdft_actual STREQUAL PDFT_PDFIUM_${PDFT_ARCH}_DLL_SHA256)
  message(FATAL_ERROR "${_pdft_pdfium_dll} has SHA-256 ${_pdft_actual}, expected "
                      "${PDFT_PDFIUM_${PDFT_ARCH}_DLL_SHA256}. Delete ${PDFT_PDFIUM_DIR} and configure again.")
endif()
unset(_pdft_actual)

add_library(pdft_pdfium_headers INTERFACE)
target_include_directories(pdft_pdfium_headers SYSTEM INTERFACE "${PDFT_PDFIUM_DIR}/include")

set(PDFT_PDFIUM_RUNTIME "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${PDFT_PDFIUM_DLL}")
add_custom_command(
  OUTPUT "${PDFT_PDFIUM_RUNTIME}"
  COMMAND "${CMAKE_COMMAND}" -E copy "${_pdft_pdfium_dll}" "${PDFT_PDFIUM_RUNTIME}"
  DEPENDS "${_pdft_pdfium_dll}"
  COMMENT "Copying PDFium ${PDFT_PDFIUM_TAG} (${PDFT_ARCH}) as ${PDFT_PDFIUM_DLL}"
  VERBATIM)
add_custom_target(pdft_pdfium_runtime ALL DEPENDS "${PDFT_PDFIUM_RUNTIME}")
unset(_pdft_pdfium_dll)
