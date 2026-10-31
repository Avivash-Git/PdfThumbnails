# Third-party code, all of it downloaded against the pins in Pins.cmake (WIL and GoogleTest:
# see below).
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

# WIL, the Windows Implementation Libraries (header-only; compiled into the settings app only,
# so it is fetched only when the app is built):
#   pdft_wil      INTERFACE, SYSTEM include path, plus two definitions for every target that
#                 uses it (all of them get the same, so the linker never sees a mix):
#                 RESULT_DIAGNOSTICS_LEVEL=2: WIL's error macros record line numbers only. From
#                 level 3 on (WIL's default) every RETURN_IF_* site embeds __FILE__, the absolute
#                 source path, in the binary; gate.hardening.* would catch that.
#                 RESULT_SUPPRESS_STATIC_INITIALIZERS: otherwise wil/result.h sets up, before
#                 main, a process-wide failure cache that every reported failure then looks up
#                 through named objects (Local\SM0:<pid>:...). Any process in the session can
#                 create those names first and so choose a pointer that the app then writes
#                 through, also in an elevated settings window. The app never calls
#                 wil::WilInitialize_Result, which would set the same cache up again;
#                 gate.hardening.settings and .cli fail if the names are in the executables.
#   PDFT_WIL_DIR  the extracted sources (include/, LICENSE). Install.cmake stages its LICENSE
#                 and gate.licenses requires it whenever this is defined.
# Not WIL's own CMakeLists.txt: it would pull in its NuGet and test logic.
set(PDFT_WIL_ARCHIVE "" CACHE FILEPATH
    "Offline builds: a local WIL source archive to use instead of downloading (checked against the pin)")

if(PDFT_BUILD_APP)
  pdft_download_verified(_pdft_wil_archive
    URL "${PDFT_WIL_URL}"
    SHA256 "${PDFT_WIL_SHA256}"
    LOCAL_FILE "${PDFT_WIL_ARCHIVE}")
  set(_pdft_wil_root "${CMAKE_BINARY_DIR}/_deps/wil")
  pdft_extract_once("${_pdft_wil_archive}" "${PDFT_WIL_SHA256}" "${_pdft_wil_root}")
  # A GitHub source archive holds one folder, wil-<commit>.
  file(GLOB _pdft_wil_dirs LIST_DIRECTORIES true "${_pdft_wil_root}/*")
  list(LENGTH _pdft_wil_dirs _pdft_count)
  if(NOT _pdft_count EQUAL 1 OR NOT EXISTS "${_pdft_wil_dirs}/include/wil/result_macros.h"
     OR NOT EXISTS "${_pdft_wil_dirs}/LICENSE")
    message(FATAL_ERROR "${_pdft_wil_root} does not hold one WIL source tree: the archive layout changed")
  endif()
  set(PDFT_WIL_DIR "${_pdft_wil_dirs}")

  add_library(pdft_wil INTERFACE)
  target_include_directories(pdft_wil SYSTEM INTERFACE "${PDFT_WIL_DIR}/include")
  target_compile_definitions(pdft_wil INTERFACE RESULT_DIAGNOSTICS_LEVEL=2
                                                RESULT_SUPPRESS_STATIC_INITIALIZERS)
  unset(_pdft_wil_archive)
  unset(_pdft_wil_root)
  unset(_pdft_wil_dirs)
  unset(_pdft_count)
endif()

# GoogleTest (test programs only; never shipped). pdft_add_googletest() defines GTest::gtest
# and GTest::gtest_main from the pinned release archive and sets PDFT_GTEST_DIR (the extracted
# sources, for its LICENSE). It is built with its own flags: no pdft_harden, no /WX from us.
set(PDFT_GTEST_ARCHIVE "" CACHE FILEPATH
    "Offline builds: a local googletest-<version>.tar.gz to use instead of downloading (checked against the pin)")

function(pdft_add_googletest)
  pdft_download_verified(archive
    URL "${PDFT_GTEST_URL}"
    SHA256 "${PDFT_GTEST_SHA256}"
    LOCAL_FILE "${PDFT_GTEST_ARCHIVE}")
  include(FetchContent)
  FetchContent_Declare(googletest
    URL "${archive}"
    URL_HASH SHA256=${PDFT_GTEST_SHA256}
    SYSTEM EXCLUDE_FROM_ALL)
  # Static libraries with the static CRT (CMAKE_MSVC_RUNTIME_LIBRARY), GoogleTest only.
  set(BUILD_GMOCK OFF)
  set(INSTALL_GTEST OFF)
  set(gtest_force_shared_crt OFF)
  FetchContent_MakeAvailable(googletest)
  set(PDFT_GTEST_DIR "${googletest_SOURCE_DIR}" PARENT_SCOPE)
endfunction()
