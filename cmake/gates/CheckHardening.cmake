# gate.hardening.<bin> (PROFILE=own) and gate.pdfium.pe (PROFILE=pdfium): PE hardening bits.
#   cmake -DDUMPBIN=<dumpbin> -DFILE=<image> -DPROFILE=own|pdfium -DARCH=x64|arm64
#         -DMACHINE=<8664|AA64> [-DFORBIDDEN=<path|path|...>] -P CheckHardening.cmake
#
# Both profiles: machine type; DLL characteristics GUARD_CF 0x4000, NX_COMPAT 0x0100,
# DYNAMIC_BASE 0x0040, HIGH_ENTROPY_VA 0x0020; load config "CF instrumented" (0x100); on x64
# the CET-compatible extended characteristic (0x1).
# PROFILE=own (our binaries, built by pdft_harden) also requires: Dependent Load Flag 0x800
# (System32 only); on x64 an EH continuation table (Guard Flags 0x400000); a PDB recorded by
# file name only; and none of the FORBIDDEN paths (the source and build folders, any
# \Users\ folder) anywhere in the file, as ASCII or UTF-16 text, in any letter case.
# PDFium is built by a third party: it has CFG and CET but no dependent load flag and no EH
# continuation table, so it gets the shorter profile.

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(FILE PROFILE ARCH MACHINE)
if(NOT PROFILE MATCHES "^(own|pdfium)$")
  message(FATAL_ERROR "PROFILE must be own or pdfium")
endif()

pdft_pe_headers(pe "${FILE}")
set(problems "")

if(NOT pe_MACHINE STREQUAL MACHINE)
  list(APPEND problems "machine ${pe_MACHINE}, expected ${MACHINE} (${ARCH})")
endif()

foreach(bit IN ITEMS "0x4000 GUARD_CF (/GUARD:CF)" "0x0100 NX_COMPAT (/NXCOMPAT)"
                     "0x0040 DYNAMIC_BASE (/DYNAMICBASE)" "0x0020 HIGH_ENTROPY_VA (/HIGHENTROPYVA)")
  string(REGEX MATCH "^[^ ]+" mask "${bit}")
  pdft_has_bits(set_ ${pe_DLLCHAR} ${mask})
  if(NOT set_)
    list(APPEND problems "DLL characteristic ${bit} missing")
  endif()
endforeach()

pdft_has_bits(cf ${pe_GUARD_FLAGS} 0x100)
if(NOT cf)
  list(APPEND problems "load config is not CF instrumented")
endif()

if(ARCH STREQUAL "x64")
  pdft_has_bits(cet ${pe_EXTCHAR} 0x1)
  if(NOT cet)
    list(APPEND problems "not CET compatible (/CETCOMPAT)")
  endif()
endif()

if(PROFILE STREQUAL "own")
  if(NOT pe_DEPENDENT_LOAD EQUAL 2048)
    list(APPEND problems "Dependent Load Flag is ${pe_DEPENDENT_LOAD}, expected 0x800 (/DEPENDENTLOADFLAG:0x800)")
  endif()
  if(ARCH STREQUAL "x64")
    pdft_has_bits(ehcont ${pe_GUARD_FLAGS} 0x400000)
    if(NOT ehcont)
      list(APPEND problems "no EH continuation table (/guard:ehcont)")
    endif()
  endif()
  if(pe_PDB STREQUAL "")
    list(APPEND problems "no CodeView debug directory entry (PDB name)")
  elseif(pe_PDB MATCHES "[:\\\\/]")
    list(APPEND problems "the debug directory records a PDB path (${pe_PDB}), expected a file name (/PDBALTPATH:%_PDB%)")
  endif()

  # Build-machine paths anywhere in the file.
  pdft_split(forbidden "${FORBIDDEN}" "|")
  foreach(path IN LISTS forbidden)
    string(REGEX REPLACE "^[A-Za-z]:" "" path "${path}")   # drive letters vary in case
    string(LENGTH "${path}" length)
    if(length LESS 3)
      continue()
    endif()
    set(regex "")
    math(EXPR last "${length} - 1")
    foreach(i RANGE ${last})
      string(SUBSTRING "${path}" ${i} 1 c)
      if(c MATCHES "[A-Za-z]")
        string(TOUPPER "${c}" upper)
        string(TOLOWER "${c}" lower)
        string(APPEND regex "[${upper}${lower}]")
      elseif(c STREQUAL "/" OR c STREQUAL "\\")
        string(APPEND regex "[\\\\/]")
      elseif(c MATCHES "[0-9 _-]")
        string(APPEND regex "${c}")
      else()
        string(APPEND regex "\\${c}")
      endif()
    endforeach()
    file(STRINGS "${FILE}" ascii_hits REGEX "${regex}")
    file(STRINGS "${FILE}" utf16_hits ENCODING UTF-16LE REGEX "${regex}")
    if(ascii_hits OR utf16_hits)
      if(ascii_hits)
        list(GET ascii_hits 0 example)
      else()
        list(GET utf16_hits 0 example)
      endif()
      list(APPEND problems "contains the build-machine path '${path}' (for example: ${example})")
    endif()
  endforeach()
endif()

if(problems)
  list(JOIN problems "\n  " problems)
  pdft_gate_fail("${FILE}:\n  ${problems}")
endif()
math(EXPR dllchar "${pe_DLLCHAR}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR guard "${pe_GUARD_FLAGS}" OUTPUT_FORMAT HEXADECIMAL)
math(EXPR depload "${pe_DEPENDENT_LOAD}" OUTPUT_FORMAT HEXADECIMAL)
pdft_gate_ok("${FILE}: machine ${pe_MACHINE}, DLL characteristics ${dllchar}, extended ${pe_EXTCHAR}, "
             "guard flags ${guard} (EH continuation count ${pe_EHCONT_COUNT}), dependent load flag "
             "${depload}, PDB '${pe_PDB}' (profile ${PROFILE})")
