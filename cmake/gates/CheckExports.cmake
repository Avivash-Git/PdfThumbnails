# gate.<bin>.exports: the image exports exactly the expected names and nothing by ordinal only.
#   cmake -DDUMPBIN=<dumpbin> -DFILE=<image> -DEXPECTED=<name,name,...> -P CheckExports.cmake

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(FILE EXPECTED)

pdft_split(expected "${EXPECTED}" ",")
list(SORT expected)
list(LENGTH expected count)
pdft_pe_exports(names "${FILE}")
list(SORT names)
pdft_pe_export_counts(functions named "${FILE}")

if(NOT names STREQUAL expected)
  pdft_gate_fail("${FILE} exports [${names}], expected exactly [${expected}]")
endif()
if(NOT functions EQUAL count OR NOT named EQUAL count)
  pdft_gate_fail("${FILE} has ${functions} exported functions and ${named} names, expected ${count}")
endif()
pdft_gate_ok("${FILE} exports exactly ${names}")
