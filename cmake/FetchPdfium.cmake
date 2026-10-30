# Script mode: puts the pinned PDFium archive for one architecture into the download cache
# (downloading it if needed, SHA-256 checked as in a build) without configuring anything, and
# optionally checks its GitHub artifact attestation: the archive must have been built by
# pdfium-binaries' own release workflow. CI runs it before the build whenever the download
# cache misses (a new pin), so nothing extracts or loads a PDFium DLL of unknown origin; run it
# by hand when bumping PDFium (needs the GitHub CLI, signed in or with GH_TOKEN set).
#   cmake -DARCH=<x64|arm64> [-DVERIFY_ATTESTATION=ON] -P cmake/FetchPdfium.cmake
# PDFT_DOWNLOAD_DIR (-D) overrides the cache folder, as in a build.

cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED ARCH OR NOT ARCH MATCHES "^(x64|arm64)$")
  message(FATAL_ERROR "usage: cmake -DARCH=<x64|arm64> [-DVERIFY_ATTESTATION=ON] -P ${CMAKE_SCRIPT_MODE_FILE}")
endif()

# FetchVerified.cmake's default cache is <repository>/.cache/downloads.
cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH PROJECT_SOURCE_DIR)
include("${CMAKE_CURRENT_LIST_DIR}/Pins.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/FetchVerified.cmake")

pdft_download_verified(archive URL "${PDFT_PDFIUM_${ARCH}_URL}" SHA256 "${PDFT_PDFIUM_${ARCH}_SHA256}")
message(STATUS "PDFium ${PDFT_PDFIUM_TAG} ${ARCH}: ${archive}")

if(VERIFY_ATTESTATION)
  find_program(PDFT_GH gh REQUIRED)
  execute_process(
    COMMAND "${PDFT_GH}" attestation verify "${archive}"
            --repo "${PDFT_PDFIUM_ATTESTATION_REPO}"
            --signer-workflow "${PDFT_PDFIUM_ATTESTATION_WORKFLOW}"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${archive}: no valid attestation from ${PDFT_PDFIUM_ATTESTATION_WORKFLOW} "
                        "(gh attestation verify exited with ${rc})")
  endif()
  message(STATUS "PDFium ${ARCH}: attestation verified (${PDFT_PDFIUM_ATTESTATION_WORKFLOW})")
endif()
