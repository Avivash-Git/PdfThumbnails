# PdfThumbnails: PDF thumbnail handler for Windows Explorer.
# Build with scripts\build.cmd <preset> (see CMakePresets.json).

# 3.31 is what Visual Studio 2022 bundles; the code and build must work with VS 2022 (v143)
# and VS 2026 (v145) alike.
cmake_minimum_required(VERSION 3.31)

include(cmake/ProjectInfo.cmake)
project(${PDFT_PRODUCT_NAME}
  VERSION ${PDFT_VERSION}.0
  DESCRIPTION "${PDFT_DESCRIPTION}"
  LANGUAGES CXX RC)

if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "Build with MSVC (cl.exe) from a Visual Studio developer environment: "
                      "scripts\\build.cmd <preset>")
endif()
if(MSVC_VERSION LESS 1930)
  message(FATAL_ERROR "MSVC ${CMAKE_CXX_COMPILER_VERSION} is too old: use Visual Studio 2022 (v143) or later")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)   # no C++20 modules; skips the dependency scan
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")                        # /Z7; link writes PDBs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")   # DLLs and exes side by side
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(PDFT_BUILD_TOOLS "Build the developer tools (thumbtest)" ON)
option(PDFT_WARNINGS_AS_ERRORS "Compiler and linker warnings are errors in our own targets" ON)
set(PDFT_SPECTRE "AUTO" CACHE STRING
    "Spectre mitigations (/Qspectre + Spectre-mitigated CRT): AUTO uses them when installed, ON requires them")
set_property(CACHE PDFT_SPECTRE PROPERTY STRINGS AUTO ON OFF)

include(cmake/TargetArch.cmake)
include(cmake/Pins.cmake)
include(cmake/FetchVerified.cmake)
include(cmake/Dependencies.cmake)
pdft_generate_project_config()

# ---- Spectre ------------------------------------------------------------------------------
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
message(STATUS "${PROJECT_NAME}: Spectre mitigations ${PDFT_SPECTRE_ACTIVE} (PDFT_SPECTRE=${PDFT_SPECTRE})")

# pdft_harden(<target> DLL|EXE|LIB)
# Compiler and linker settings for our own targets (never third-party code).
# M1 baseline; WP2 moves this into cmake/Hardening.cmake and adds CFG, CET, EH continuation,
# the dependent-load flag and /Brepro.
function(pdft_harden target kind)
  if(NOT kind MATCHES "^(DLL|EXE|LIB)$")
    message(FATAL_ERROR "pdft_harden(${target} ${kind}): kind must be DLL, EXE or LIB")
  endif()
  target_compile_options(${target} PRIVATE
    /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8
    /external:anglebrackets /external:W0 /Gy /Gw)
  target_compile_definitions(${target} PRIVATE
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN STRICT _WIN32_WINNT=0x0A00 WINVER=0x0A00)
  if(PDFT_WARNINGS_AS_ERRORS)
    set_property(TARGET ${target} PROPERTY COMPILE_WARNING_AS_ERROR ON)
  endif()
  if(PDFT_SPECTRE_ACTIVE)
    target_compile_options(${target} PRIVATE /Qspectre)
  endif()
  if(kind STREQUAL "LIB")
    return()
  endif()
  # Linker warnings as errors through an explicit /WX: CMAKE_LINK_WARNING_AS_ERROR needs
  # CMake 4.0, and VS 2022 ships 3.31.
  if(PDFT_WARNINGS_AS_ERRORS)
    target_link_options(${target} PRIVATE /WX)
  endif()
  # PDBs for every configuration; the binary records only the PDB's file name.
  target_link_options(${target} PRIVATE
    /DEBUG:FULL /PDBALTPATH:%_PDB% /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>)
  if(PDFT_SPECTRE_ACTIVE)
    # Searched before the LIB environment variable, so the Spectre CRT wins.
    target_link_options(${target} PRIVATE "/LIBPATH:${PDFT_SPECTRE_LIB_DIR}")
  endif()
endfunction()

add_subdirectory(src/handler)
if(PDFT_BUILD_TOOLS)
  add_subdirectory(tools/thumbtest)
endif()

include(CTest)

include(cmake/Install.cmake)
