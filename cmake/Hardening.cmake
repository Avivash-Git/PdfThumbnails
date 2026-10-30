# Compiler and linker settings for our own targets (never third-party code), plus the switches
# that control them. Included after TargetArch.cmake (needs PDFT_ARCH and PDFT_SPECTRE_LIB_DIR).
#
#   pdft_harden(<target> DLL|EXE|LIB|TEST)
#     DLL, EXE  everything below.
#     LIB       compile options only (the linking target applies the link options).
#     TEST      an executable that links GoogleTest, which is built without these flags: like
#               EXE but without /guard:ehcont, because the linker refuses EH continuation
#               metadata checks on objects compiled without it (LNK2047).
#
# Options:
#   PDFT_WARNINGS_AS_ERRORS  compiler (COMPILE_WARNING_AS_ERROR) and linker (LINK_WARNING_AS_ERROR)
#                            warnings fail the build. Both are target properties, so third-party
#                            targets never get them.
#   PDFT_SPECTRE             AUTO | ON | OFF: /Qspectre and the Spectre-mitigated CRT. AUTO uses
#                            them when the libraries are installed; ON fails configure without.
#   PDFT_REPRODUCIBLE        /Brepro (undocumented; used by Chromium): no time stamps in objects
#                            and binaries, so identical inputs give identical files.

include_guard(GLOBAL)

option(PDFT_WARNINGS_AS_ERRORS "Compiler and linker warnings are errors in our own targets" ON)
set(PDFT_SPECTRE "AUTO" CACHE STRING
    "Spectre mitigations (/Qspectre + Spectre-mitigated CRT): AUTO uses them when installed, ON requires them")
set_property(CACHE PDFT_SPECTRE PROPERTY STRINGS AUTO ON OFF)
option(PDFT_REPRODUCIBLE "Deterministic objects and binaries (/Brepro)" OFF)

# ---- Spectre ------------------------------------------------------------------------------
if(NOT PDFT_SPECTRE MATCHES "^(AUTO|ON|OFF)$")
  message(FATAL_ERROR "PDFT_SPECTRE must be AUTO, ON or OFF (is '${PDFT_SPECTRE}')")
endif()
if(PDFT_SPECTRE STREQUAL "OFF")
  set(PDFT_SPECTRE_ACTIVE OFF)
elseif(EXISTS "${PDFT_SPECTRE_LIB_DIR}/libcmt.lib")
  set(PDFT_SPECTRE_ACTIVE ON)
elseif(PDFT_SPECTRE STREQUAL "ON")
  message(FATAL_ERROR "PDFT_SPECTRE=ON, but the Spectre-mitigated libraries are not installed "
                      "(${PDFT_SPECTRE_LIB_DIR}). Install the components in .vsconfig.")
else()
  set(PDFT_SPECTRE_ACTIVE OFF)
  message(WARNING "Spectre-mitigated libraries not found (${PDFT_SPECTRE_LIB_DIR}); building without "
                  "/Qspectre. Install the components in .vsconfig to match CI.")
endif()
message(STATUS "${PROJECT_NAME}: Spectre mitigations ${PDFT_SPECTRE_ACTIVE} (PDFT_SPECTRE=${PDFT_SPECTRE}), "
               "reproducible ${PDFT_REPRODUCIBLE}, warnings as errors ${PDFT_WARNINGS_AS_ERRORS}")

# pdft_harden(<target> DLL|EXE|LIB|TEST): see the top of this file.
function(pdft_harden target kind)
  if(NOT kind MATCHES "^(DLL|EXE|LIB|TEST)$")
    message(FATAL_ERROR "pdft_harden(${target} ${kind}): kind must be DLL, EXE, LIB or TEST")
  endif()
  # EH continuation metadata pairs with CET shadow stacks, which exist on x64 only.
  set(ehcont OFF)
  if(PDFT_ARCH STREQUAL "x64" AND NOT kind STREQUAL "TEST")
    set(ehcont ON)
  endif()

  # ---- compile ----
  target_compile_options(${target} PRIVATE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8
    /external:anglebrackets /external:W0
    /sdl /Gy /Gw /guard:cf)
  target_compile_definitions(${target} PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN STRICT _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  if(PDFT_WARNINGS_AS_ERRORS)
    set_property(TARGET ${target} PROPERTY COMPILE_WARNING_AS_ERROR ON)
  endif()
  if(ehcont)
    target_compile_options(${target} PRIVATE /guard:ehcont)
  endif()
  if(PDFT_SPECTRE_ACTIVE)
    target_compile_options(${target} PRIVATE /Qspectre)
  endif()
  if(PDFT_REPRODUCIBLE)
    target_compile_options(${target} PRIVATE /Brepro)
  endif()
  if(kind STREQUAL "LIB")
    return()
  endif()

  # ---- link ----
  if(PDFT_WARNINGS_AS_ERRORS)
    set_property(TARGET ${target} PROPERTY LINK_WARNING_AS_ERROR ON)
  endif()
  # PDBs for every configuration; the binary records only the PDB's file name, never a path.
  # Static imports resolve from System32 only (0x800 = LOAD_LIBRARY_SEARCH_SYSTEM32).
  target_link_options(${target} PRIVATE
    /DEBUG:FULL /PDBALTPATH:%_PDB% /INCREMENTAL:NO
    /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF /DEPENDENTLOADFLAG:0x800
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
  if(PDFT_ARCH STREQUAL "x64")
    target_link_options(${target} PRIVATE /CETCOMPAT)   # "only applicable to x64"
  endif()
  if(ehcont)
    target_link_options(${target} PRIVATE /guard:ehcont)
  endif()
  if(PDFT_SPECTRE_ACTIVE)
    # Searched before the LIB environment variable, so the Spectre CRT wins.
    target_link_options(${target} PRIVATE "/LIBPATH:${PDFT_SPECTRE_LIB_DIR}")
  endif()
  if(PDFT_REPRODUCIBLE)
    target_link_options(${target} PRIVATE /Brepro)
    # Relinking over an existing PDB bumps its age, which the binary records: start from no PDB
    # so a relink gives the same bytes as a clean build.
    add_custom_command(TARGET ${target} PRE_LINK
      COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_PDB_FILE:${target}>"
      VERBATIM)
  endif()
endfunction()
