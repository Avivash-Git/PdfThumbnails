# Checks the Authenticode signatures of Windows binaries: each must be trusted by Windows for code
# (SignTool's /pa policy, the one Smart App Control and SmartScreen apply to a signed file) and
# timestamped.
#
#   cmake -DPATHS=<file or folder>[;...] [-DREQUIRE=ON] [-DSUBJECT=<text>] -P cmake/VerifySignatures.cmake
#
# (-D options go before -P.) Folders are searched for .exe and .dll files, with their subfolders.
# Every file gets one line: its state and the subject of the certificate that signed it.
#   REQUIRE=ON  fails unless every file is signed and timestamped (and, with SUBJECT, signed by a
#               certificate whose subject contains that text). PDFT_SIGN_PDFIUM=off in the
#               environment lets the PDFium DLL be unsigned.
#   REQUIRE=OFF (default) only reports, for builds that are not signed.

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/ProjectInfo.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/SignTool.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/gates/PeTools.cmake")
pdft_require(PATHS)

pdft_find_signtool(signtool)
pdft_pe_files(files ${PATHS})
if(NOT files)
  message(FATAL_ERROR "No .exe or .dll in ${PATHS}")
endif()

set(good 0)
set(bad "")
foreach(file IN LISTS files)
  get_filename_component(name "${file}" NAME)
  pdft_signature_state(state "${signtool}" "${file}")
  pdft_signer_subject(subject "${file}")
  set(fine FALSE)
  if(state STREQUAL "ok")
    set(text "signed and timestamped")
    set(fine TRUE)
    if(DEFINED SUBJECT AND NOT SUBJECT STREQUAL "")
      string(FIND "${subject}" "${SUBJECT}" at)
      if(at EQUAL -1)
        set(text "signed and timestamped, but not by the expected signer")
        set(fine FALSE)
      endif()
    endif()
  elseif(state STREQUAL "untimestamped")
    set(text "signed, NOT timestamped")
  elseif(subject STREQUAL "")
    set(text "NOT signed")
  else()
    set(text "signature NOT trusted")
  endif()
  if(NOT subject STREQUAL "")
    string(APPEND text " (${subject})")
  endif()
  if(NOT fine AND name STREQUAL PDFT_PDFIUM_DLL AND "$ENV{PDFT_SIGN_PDFIUM}" STREQUAL "off")
    string(APPEND text "; allowed: PDFT_SIGN_PDFIUM=off")
    set(fine TRUE)
  endif()
  message(STATUS "${text}: ${file}")
  if(fine)
    math(EXPR good "${good} + 1")
  else()
    list(APPEND bad "${file}")
  endif()
endforeach()

list(LENGTH files total)
list(LENGTH bad failed)
if(failed EQUAL 0)
  message(STATUS "All ${total} files are signed and timestamped.")
elseif(REQUIRE)
  string(REPLACE ";" "\n  " bad "${bad}")
  message(FATAL_ERROR "${failed} of ${total} files are not signed and timestamped as required:\n  ${bad}")
else()
  message(STATUS "${good} of ${total} files are signed and timestamped (not required here).")
endif()
