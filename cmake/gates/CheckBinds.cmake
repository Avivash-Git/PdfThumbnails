# gate.pdfium.binds: the handler's X-macro list of bound PDFium functions has exactly COUNT
# entries, no duplicates, and every one is exported by the staged PDFium DLL (the handler
# refuses to load PDFium if any is missing, and then every thumbnail fails).
#   cmake -DDUMPBIN=<dumpbin> -DLIST=<pdfium_functions.inc> -DCOUNT=<n> -DFILE=<pdfium dll>
#         -P CheckBinds.cmake

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(LIST COUNT FILE)

file(STRINGS "${LIST}" lines REGEX "^[ \t]*PDFT_FN")
set(functions "")
set(members "")
foreach(line IN LISTS lines)
  string(STRIP "${line}" line)
  if(NOT line MATCHES "^PDFT_FN\\(([A-Za-z_][A-Za-z0-9_]*), *([A-Za-z_][A-Za-z0-9_]*)\\)$")
    pdft_gate_fail("${LIST}: cannot parse '${line}' (expected PDFT_FN(<function>, <member>))")
  endif()
  list(APPEND functions "${CMAKE_MATCH_1}")
  list(APPEND members "${CMAKE_MATCH_2}")
endforeach()

list(LENGTH functions count)
if(NOT count EQUAL COUNT)
  pdft_gate_fail("${LIST} lists ${count} PDFium functions, expected ${COUNT}. If the change is "
                 "intended, update the count in cmake/Gates.cmake in the same commit.")
endif()
foreach(kind IN ITEMS functions members)
  set(unique ${${kind}})
  list(REMOVE_DUPLICATES unique)
  list(LENGTH unique unique_count)
  if(NOT unique_count EQUAL count)
    pdft_gate_fail("${LIST} has duplicate ${kind}")
  endif()
endforeach()

pdft_pe_exports(exports "${FILE}")
set(missing "")
foreach(function IN LISTS functions)
  if(NOT function IN_LIST exports)
    list(APPEND missing "${function}")
  endif()
endforeach()
if(missing)
  pdft_gate_fail("${FILE} does not export ${missing}")
endif()
list(LENGTH exports export_count)
pdft_gate_ok("${FILE} (${export_count} exports) exports all ${count} functions the handler binds")
