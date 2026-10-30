# gate.imports.<bin>: the image imports only allowed DLLs, never a C/C++ runtime DLL, and
# uses delay-load imports only from an explicit list (none unless DELAY_ALLOW is given).
#   cmake -DDUMPBIN=<dumpbin> -DFILE=<image> -DALLOW=<dll,dll,...> [-DDELAY_ALLOW=<dll,...>]
#         -P CheckImports.cmake
# Names compare case-insensitively; a trailing '*' matches any rest (api-ms-win-core-*).
# The deny list catches an accidental dynamic CRT (/MD) even when its DLLs are on the allow list
# by pattern, and even though vcruntime140.dll exists in System32 on most PCs.

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(FILE ALLOW)

set(deny "VCRUNTIME*" "MSVCP*" "UCRTBASE*" "api-ms-win-crt-*" "CONCRT*" "VCOMP*" "VCCORLIB*")

# pdft_import_matches(<out-var> <dll name> <pattern>...)
function(pdft_import_matches out name)
  string(TOUPPER "${name}" name)
  foreach(pattern IN LISTS ARGN)
    string(TOUPPER "${pattern}" pattern)
    if(NOT pattern MATCHES "^[-A-Z0-9_.]+[*]?$")
      message(FATAL_ERROR "bad DLL name pattern '${pattern}'")
    endif()
    string(REPLACE "." "\\." regex "${pattern}")
    string(REPLACE "*" ".*" regex "${regex}")
    if(name MATCHES "^${regex}$")
      set(${out} TRUE PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out} FALSE PARENT_SCOPE)
endfunction()

pdft_split(allow "${ALLOW}" ",")
pdft_split(delay_allow "${DELAY_ALLOW}" ",")
pdft_pe_dependents(static delay "${FILE}")
if(NOT static)
  pdft_gate_fail("no static imports found in ${FILE}: dumpbin's /dependents output was not understood")
endif()

set(bad "")
foreach(kind IN ITEMS static delay)
  if(kind STREQUAL "static")
    set(allowed ${allow})
  else()
    set(allowed ${delay_allow})
  endif()
  foreach(dll IN LISTS ${kind})
    pdft_import_matches(denied "${dll}" ${deny})
    pdft_import_matches(ok "${dll}" ${allowed})
    if(denied)
      list(APPEND bad "${dll} (${kind}: C/C++ runtime DLL; build with the static CRT)")
    elseif(NOT ok)
      list(APPEND bad "${dll} (${kind}: not on the allow list)")
    endif()
  endforeach()
endforeach()
if(bad)
  list(JOIN bad ", " bad)
  pdft_gate_fail("${FILE} imports ${bad}")
endif()
if(NOT delay)
  set(delay "none")
endif()
pdft_gate_ok("${FILE} imports ${static} (delay-load: ${delay})")
