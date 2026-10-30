# gate.licenses: the staged tree carries every license and notice the binaries need, unchanged,
# and THIRD-PARTY-NOTICES.md names every staged license file by its path.
#   cmake -DSTAGE_DIR=<stage> -DPDFIUM_DIR=<extracted PDFium archive> -DPROJECT_LICENSE=<LICENSE>
#         -DNOTICES=<THIRD-PARTY-NOTICES.md> [-DWIL_LICENSE=<WIL's LICENSE>]
#         [-DGTEST_LICENSE=<GoogleTest's LICENSE>] -P CheckLicenses.cmake
# WIL_LICENSE is passed once a target compiles WIL in (the settings app); GTEST_LICENSE when
# the staged component is the test bundle (programs linked with GoogleTest).

cmake_minimum_required(VERSION 3.31)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(STAGE_DIR PDFIUM_DIR PROJECT_LICENSE NOTICES)

set(problems "")

# pdft_same_file(<staged relative path> <source>)
function(pdft_same_file staged source)
  set(path "${STAGE_DIR}/${staged}")
  if(NOT EXISTS "${path}")
    set(problems ${problems} "${staged} is not staged" PARENT_SCOPE)
    return()
  endif()
  file(SHA256 "${path}" a)
  file(SHA256 "${source}" b)
  if(NOT a STREQUAL b)
    set(problems ${problems} "${staged} differs from ${source}" PARENT_SCOPE)
  endif()
endfunction()

pdft_same_file("LICENSE.txt" "${PROJECT_LICENSE}")
pdft_same_file("THIRD-PARTY-NOTICES.md" "${NOTICES}")
pdft_same_file("licenses/pdfium-binaries-LICENSE.txt" "${PDFIUM_DIR}/LICENSE")
if(WIL_LICENSE)
  pdft_same_file("licenses/wil-LICENSE.txt" "${WIL_LICENSE}")
endif()
if(GTEST_LICENSE)
  pdft_same_file("licenses/googletest-LICENSE.txt" "${GTEST_LICENSE}")
endif()

# Every license file in the PDFium archive, PDFium's own and FreeType's above all.
file(GLOB pdfium_licenses RELATIVE "${PDFIUM_DIR}/licenses" "${PDFIUM_DIR}/licenses/*")
list(LENGTH pdfium_licenses count)
if(count EQUAL 0)
  list(APPEND problems "${PDFIUM_DIR}/licenses is empty")
endif()
foreach(required IN ITEMS pdfium.txt freetype.txt)
  if(NOT required IN_LIST pdfium_licenses)
    list(APPEND problems "the PDFium archive has no licenses/${required}")
  endif()
endforeach()
foreach(name IN LISTS pdfium_licenses)
  pdft_same_file("licenses/pdfium/${name}" "${PDFIUM_DIR}/licenses/${name}")
endforeach()

# The notices name every staged license file (catches a new component after a PDFium bump).
file(READ "${NOTICES}" notices)
file(GLOB_RECURSE staged RELATIVE "${STAGE_DIR}" "${STAGE_DIR}/licenses/*")
foreach(path IN LISTS staged)
  string(FIND "${notices}" "${path}" at)
  if(at LESS 0)
    list(APPEND problems "THIRD-PARTY-NOTICES.md does not mention ${path}")
  endif()
endforeach()

if(problems)
  list(JOIN problems "\n  " problems)
  pdft_gate_fail("license files:\n  ${problems}")
endif()
list(LENGTH staged staged_count)
pdft_gate_ok("${staged_count} license files staged and named in THIRD-PARTY-NOTICES.md, and LICENSE.txt and "
             "the notices match the sources")
