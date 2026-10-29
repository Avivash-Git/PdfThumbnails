# Verified downloads into a content-addressed cache, and extraction that happens once per
# archive hash.

include_guard(GLOBAL)

set(PDFT_DOWNLOAD_DIR "${PROJECT_SOURCE_DIR}/.cache/downloads" CACHE PATH
    "Download cache; files are stored as <sha256>-<file name>")

# pdft_download_verified(<out-var> URL <url> SHA256 <hex> [LOCAL_FILE <path>])
# Sets <out-var> to a local file whose SHA-256 is <hex>: LOCAL_FILE if given (offline builds;
# still checked), else the cached copy, else a fresh download. Any mismatch is fatal.
function(pdft_download_verified out)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "URL;SHA256;LOCAL_FILE" "")
  string(TOLOWER "${arg_SHA256}" sha)
  string(REPEAT "[0-9a-f]" 64 sha_pattern)
  if(NOT sha MATCHES "^${sha_pattern}$" OR NOT arg_URL)
    message(FATAL_ERROR "pdft_download_verified: URL and a 64-digit SHA256 are required")
  endif()

  if(arg_LOCAL_FILE)
    file(SHA256 "${arg_LOCAL_FILE}" actual)
    if(NOT actual STREQUAL sha)
      message(FATAL_ERROR "${arg_LOCAL_FILE} has SHA-256 ${actual}; the pin for ${arg_URL} is ${sha}")
    endif()
    set(${out} "${arg_LOCAL_FILE}" PARENT_SCOPE)
    return()
  endif()

  cmake_path(GET arg_URL FILENAME name)
  set(dest "${PDFT_DOWNLOAD_DIR}/${sha}-${name}")
  file(MAKE_DIRECTORY "${PDFT_DOWNLOAD_DIR}")
  # Parallel configures (several presets) share the cache.
  file(LOCK "${dest}.lock" GUARD FUNCTION TIMEOUT 600 RESULT_VARIABLE locked)
  if(NOT locked EQUAL 0)
    message(FATAL_ERROR "Cannot lock ${dest}.lock: ${locked}")
  endif()

  if(EXISTS "${dest}")
    file(SHA256 "${dest}" actual)
    if(actual STREQUAL sha)
      set(${out} "${dest}" PARENT_SCOPE)
      return()
    endif()
    message(WARNING "Discarding ${dest}: its SHA-256 is ${actual}")
    file(REMOVE "${dest}")
  endif()

  set(part "${dest}.part")
  file(REMOVE "${part}")
  message(STATUS "Downloading ${arg_URL}")
  file(DOWNLOAD "${arg_URL}" "${part}" EXPECTED_HASH SHA256=${sha} TLS_VERIFY ON
       INACTIVITY_TIMEOUT 60 TIMEOUT 600 STATUS status)
  list(GET status 0 code)
  if(NOT code EQUAL 0)
    list(GET status 1 reason)
    file(REMOVE "${part}")
    message(FATAL_ERROR "Download of ${arg_URL} failed: ${reason} (${code})")
  endif()
  file(RENAME "${part}" "${dest}")
  set(${out} "${dest}" PARENT_SCOPE)
endfunction()

# pdft_extract_once(<archive> <sha256> <dest-dir>)
# Extracts <archive> into <dest-dir> unless <dest-dir> already holds this archive's contents
# (recorded in <dest-dir>.sha256). A different hash wipes <dest-dir> first.
function(pdft_extract_once archive sha dest)
  set(stamp "${dest}.sha256")
  if(IS_DIRECTORY "${dest}" AND EXISTS "${stamp}")
    file(READ "${stamp}" previous)
    string(STRIP "${previous}" previous)
    if(previous STREQUAL sha)
      return()
    endif()
  endif()
  file(REMOVE "${stamp}")
  file(REMOVE_RECURSE "${dest}")
  file(MAKE_DIRECTORY "${dest}")
  message(STATUS "Extracting ${archive}")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${dest}")
  file(WRITE "${stamp}" "${sha}\n")
endfunction()
