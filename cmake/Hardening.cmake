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
#   PDFT_ASAN                AddressSanitizer (/fsanitize=address) for everything, third-party
#                            code included (the STL refuses to link instrumented and plain
#                            objects together). For local test runs only: see "AddressSanitizer"
#                            below for what it changes.
#
# The app's executables and their DLL imports:
#   PDFT_EXE_STATIC_IMPORTS  what they may import statically: KnownDLLs only
#   PDFT_EXE_DELAY_LOADS     everything else they may use, delay-loaded
#   pdft_delay_load(<target> <dll>...)   links and delay-loads DLLs from the second list

include_guard(GLOBAL)

option(PDFT_WARNINGS_AS_ERRORS "Compiler and linker warnings are errors in our own targets" ON)
set(PDFT_SPECTRE "AUTO" CACHE STRING
    "Spectre mitigations (/Qspectre + Spectre-mitigated CRT): AUTO uses them when installed, ON requires them")
set_property(CACHE PDFT_SPECTRE PROPERTY STRINGS AUTO ON OFF)
option(PDFT_REPRODUCIBLE "Deterministic objects and binaries (/Brepro)" OFF)
option(PDFT_ASAN "AddressSanitizer for every target (local test builds only)" OFF)

# ---- DLL imports of the app's executables ---------------------------------------------------
# The executables may run from a folder anyone in the user's session can write to (a download
# or build folder), where a planted DLL would be found before the system's own copy. The loader
# resolves static imports before main() runs, so those are limited to KnownDLLs, which Windows
# always maps from System32 (the list in HKLM\SYSTEM\CurrentControlSet\Control\Session
# Manager\KnownDLLs). Every other DLL is delay-loaded: the first call loads it, after
# HardenProcess() (src/app/core/startup.h) has limited the DLL search to System32.
# Common Controls v6 is the one exception: the settings executable's manifest binds COMCTL32
# to the side-by-side copy in WinSxS, which the loader never looks for next to the executable.
# The gates (gate.imports.*) enforce both lists, and gate.planting tries the second.
set(PDFT_EXE_STATIC_IMPORTS
  KERNEL32.dll USER32.dll GDI32.dll ADVAPI32.dll SHELL32.dll ole32.dll OLEAUT32.dll SHLWAPI.dll
  COMDLG32.dll SHCORE.dll)
set(PDFT_EXE_DELAY_LOADS
  msi.dll wevtapi.dll rstrtmgr.dll propsys.dll version.dll userenv.dll uxtheme.dll dwmapi.dll
  oleacc.dll bcrypt.dll wintrust.dll crypt32.dll)

# pdft_delay_load(<target> <dll>...)
# Links <target> with the import library of each <dll> (which must be on PDFT_EXE_DELAY_LOADS)
# and makes every executable that links it delay-load the DLL (/DELAYLOAD, delayimp.lib). On a
# static library the settings travel to the executables that link it. A static library's object
# that uses a DLL may not be linked into every executable; the linker then reports LNK4199
# ("no imports found"), which is harmless and ignored.
function(pdft_delay_load target)
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "EXECUTABLE")
    set(scope PRIVATE)
  elseif(type STREQUAL "STATIC_LIBRARY" OR type STREQUAL "INTERFACE_LIBRARY")
    set(scope INTERFACE)
  else()
    message(FATAL_ERROR "pdft_delay_load(${target}): only for executables and the libraries they link")
  endif()
  foreach(dll IN LISTS ARGN)
    string(TOLOWER "${dll}" lower)
    set(allowed FALSE)
    foreach(entry IN LISTS PDFT_EXE_DELAY_LOADS)
      string(TOLOWER "${entry}" entry)
      if(lower STREQUAL entry)
        set(allowed TRUE)
      endif()
    endforeach()
    if(NOT allowed)
      message(FATAL_ERROR "pdft_delay_load(${target}): ${dll} is not on PDFT_EXE_DELAY_LOADS "
                          "(cmake/Hardening.cmake); add it there first, the gates use that list")
    endif()
    cmake_path(GET lower STEM stem)
    target_link_libraries(${target} ${scope} ${stem})
    target_link_options(${target} ${scope} "/DELAYLOAD:${lower}")
  endforeach()
  target_link_libraries(${target} ${scope} delayimp)
  target_link_options(${target} ${scope} /IGNORE:4199)
endfunction()

# ---- AddressSanitizer -----------------------------------------------------------------------
# Everything is instrumented (add_compile_options reaches every target defined after this file,
# GoogleTest included). Changes against a normal build:
#   - no /RTC run-time checks (incompatible with ASan);
#   - no /DEPENDENTLOADFLAG:0x800: every program imports the ASan runtime DLL, which is never in
#     System32;
#   - the runtime DLLs are copied next to the programs (target pdft_asan_runtime), so tests and
#     the processes they start find them;
#   - the import, hardening and planting gates do not apply (the x64-asan test preset leaves
#     them out).
if(PDFT_ASAN)
  if(PDFT_REPRODUCIBLE)
    message(FATAL_ERROR "PDFT_ASAN and PDFT_REPRODUCIBLE do not go together (ASan builds are for local tests)")
  endif()
  add_compile_options(/fsanitize=address)
  set(CMAKE_MSVC_RUNTIME_CHECKS "")   # no /RTC (CMP0184)
  # The runtime lives next to cl.exe (...\bin\Host<host>\<target>).
  cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _pdft_cl_dir)
  file(GLOB _pdft_asan_dlls "${_pdft_cl_dir}/clang_rt.asan*_dynamic-*.dll")
  if(NOT _pdft_asan_dlls)
    message(FATAL_ERROR "PDFT_ASAN: no clang_rt.asan*_dynamic-*.dll next to ${CMAKE_CXX_COMPILER}")
  endif()
  add_custom_target(pdft_asan_runtime ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${_pdft_asan_dlls} "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    COMMENT "Copying the AddressSanitizer runtime next to the programs"
    VERBATIM)
  unset(_pdft_cl_dir)
  unset(_pdft_asan_dlls)
endif()

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
               "reproducible ${PDFT_REPRODUCIBLE}, warnings as errors ${PDFT_WARNINGS_AS_ERRORS}, "
               "AddressSanitizer ${PDFT_ASAN}")

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
  target_link_options(${target} PRIVATE
    /DEBUG:FULL /PDBALTPATH:%_PDB% /INCREMENTAL:NO
    /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
  # Static imports resolve from System32 only (0x800 = LOAD_LIBRARY_SEARCH_SYSTEM32); not with
  # ASan, whose runtime DLL is a static import from outside System32.
  if(NOT PDFT_ASAN)
    target_link_options(${target} PRIVATE /DEPENDENTLOADFLAG:0x800)
  endif()
  if(PDFT_ASAN AND TARGET pdft_asan_runtime)
    add_dependencies(${target} pdft_asan_runtime)
  endif()
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
