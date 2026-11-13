# Signs Windows binaries with SignTool: Authenticode, SHA-256, and an RFC 3161 timestamp, so the
# signatures stay valid after the certificate expires. Each file's signature is checked right
# after signing (trusted by Windows for code, and timestamped), and the script stops at the first
# file that fails.
#
#   cmake -DFILES=<file>[;<file>...] -P cmake/Sign.cmake    the files named, always signed
#   cmake -DDIR=<folder> -P cmake/Sign.cmake                every .exe and .dll in the folder and
#                                                            its subfolders, except files that
#                                                            already have a valid signature
#
# (-D options go before -P.) Inno Setup calls it with -DFILES=<one file> for the setup, its
# uninstaller and Setup's temporary copies of itself (cmake/Installer.cmake -DSIGN=ON).
#
# The signing identity comes from the environment, never from the command line, so it does not
# show in Inno Setup's or CI's logs:
#   PDFT_SIGNING              how to sign:
#     certificate             a certificate whose private key Windows can use, in the current
#                             user's Personal store (from a hardware token, or a signing
#                             service's tool that puts its certificate there):
#       PDFT_SIGN_CERT_SHA1     its SHA-1 thumbprint (40 hex digits)
#       PDFT_SIGN_TIMESTAMP_URL the certificate authority's RFC 3161 timestamp server
#     artifact-signing        Microsoft's Artifact Signing service, through its SignTool plug-in:
#       PDFT_SIGN_DLIB          the plug-in, Azure.CodeSigning.Dlib.dll (x64)
#       PDFT_SIGN_METADATA      its metadata.json (endpoint, account, certificate profile)
#       PDFT_SIGN_TIMESTAMP_URL optional; default http://timestamp.acs.microsoft.com
#   PDFT_SIGN_PDFIUM          "off": -DDIR leaves the PDFium DLL unsigned (for a signing service
#                             that signs only the project's own files)
#   PDFT_SIGNTOOL             optional: the signtool.exe to use (default: the newest Windows SDK's)

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/ProjectInfo.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/SignTool.cmake")

if(NOT DEFINED FILES AND NOT DEFINED DIR)
  message(FATAL_ERROR "Pass -DFILES=<file>[;<file>...] or -DDIR=<folder>")
endif()

# ---- How to sign ------------------------------------------------------------------------------
set(mode "$ENV{PDFT_SIGNING}")
set(timestamp "$ENV{PDFT_SIGN_TIMESTAMP_URL}")
if(mode STREQUAL "certificate")
  set(thumbprint "$ENV{PDFT_SIGN_CERT_SHA1}")
  string(REPLACE " " "" thumbprint "${thumbprint}")
  if(NOT thumbprint MATCHES "^[0-9A-Fa-f]+$")
    message(FATAL_ERROR "PDFT_SIGNING=certificate needs PDFT_SIGN_CERT_SHA1, the certificate's "
                        "SHA-1 thumbprint")
  endif()
  string(LENGTH "${thumbprint}" length)
  if(NOT length EQUAL 40)
    message(FATAL_ERROR "PDFT_SIGN_CERT_SHA1 has ${length} hex digits, not 40")
  endif()
  if(timestamp STREQUAL "")
    message(FATAL_ERROR "PDFT_SIGNING=certificate needs PDFT_SIGN_TIMESTAMP_URL, the certificate "
                        "authority's timestamp server")
  endif()
  set(identity /sha1 "${thumbprint}")
elseif(mode STREQUAL "artifact-signing")
  foreach(var IN ITEMS PDFT_SIGN_DLIB PDFT_SIGN_METADATA)
    if("$ENV{${var}}" STREQUAL "" OR NOT EXISTS "$ENV{${var}}")
      message(FATAL_ERROR "PDFT_SIGNING=artifact-signing needs ${var}, the path of an existing file")
    endif()
  endforeach()
  if(timestamp STREQUAL "")
    set(timestamp "http://timestamp.acs.microsoft.com")
  endif()
  set(identity /dlib "$ENV{PDFT_SIGN_DLIB}" /dmdf "$ENV{PDFT_SIGN_METADATA}")
elseif(mode STREQUAL "")
  message(FATAL_ERROR "PDFT_SIGNING is not set: nothing to sign with")
elseif(mode STREQUAL "signpath")
  message(FATAL_ERROR "PDFT_SIGNING=signpath: SignPath signs on its side (build.yml, job "
                      "installer); this script does not sign then")
else()
  message(FATAL_ERROR "PDFT_SIGNING=${mode} is not one of: certificate, artifact-signing")
endif()
if(NOT timestamp MATCHES "^https?://[^ \"]+$")
  message(FATAL_ERROR "PDFT_SIGN_TIMESTAMP_URL is not an http(s) URL")
endif()
pdft_find_signtool(signtool)
message(STATUS "SignTool: ${signtool}; signing with ${mode}; timestamps from ${timestamp}")

# ---- Which files ------------------------------------------------------------------------------
set(always_sign TRUE)
if(DEFINED DIR)
  pdft_pe_files(files "${DIR}")
  set(always_sign FALSE)
  if(NOT files)
    message(FATAL_ERROR "${DIR} holds no .exe or .dll")
  endif()
else()
  set(files "")
  foreach(file IN LISTS FILES)
    file(TO_CMAKE_PATH "${file}" file)
    if(NOT EXISTS "${file}")
      message(FATAL_ERROR "${file} does not exist")
    endif()
    list(APPEND files "${file}")
  endforeach()
endif()

# ---- Sign and check each one ------------------------------------------------------------------
foreach(file IN LISTS files)
  get_filename_component(name "${file}" NAME)
  if(NOT always_sign)
    if(name STREQUAL PDFT_PDFIUM_DLL AND "$ENV{PDFT_SIGN_PDFIUM}" STREQUAL "off")
      message(STATUS "left unsigned (PDFT_SIGN_PDFIUM=off): ${file}")
      continue()
    endif()
    pdft_signature_state(state "${signtool}" "${file}")
    if(state STREQUAL "ok")
      message(STATUS "already signed and timestamped, kept: ${file}")
      continue()
    endif()
  endif()

  # A timestamp server that is busy fails a signing now and then: try three times.
  file(TO_NATIVE_PATH "${file}" native)
  foreach(attempt RANGE 1 3)
    execute_process(
      COMMAND "${signtool}" sign /fd SHA256 ${identity} /tr "${timestamp}" /td SHA256
              /d "${PDFT_PRODUCT_NAME}" /du "${PDFT_HOMEPAGE}" "${native}"
      RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(rc EQUAL 0)
      break()
    endif()
    message(STATUS "signing ${name} failed (attempt ${attempt} of 3, exit ${rc}):\n${out}")
    if(attempt LESS 3)
      execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 10)
    endif()
  endforeach()
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "SignTool could not sign ${file}")
  endif()

  pdft_signature_state(state "${signtool}" "${file}")
  if(NOT state STREQUAL "ok")
    execute_process(COMMAND "${signtool}" verify /pa /tw /v "${native}")
    message(FATAL_ERROR "${file} was signed, but its signature is not valid and timestamped "
                        "(${state}); SignTool's report is above")
  endif()
  message(STATUS "signed and timestamped: ${file}")
endforeach()
