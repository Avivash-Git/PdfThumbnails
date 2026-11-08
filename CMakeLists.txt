# PdfThumbnails: PDF thumbnail handler for Windows Explorer.
# Build with scripts\build.cmd <preset> (see CMakePresets.json).

# Requires Visual Studio 2026 (v145) and CMake 4.3, which Visual Studio 2026 18.11 brings.
cmake_minimum_required(VERSION 4.3)

include(cmake/ProjectInfo.cmake)
project(${PDFT_PRODUCT_NAME}
  VERSION ${PDFT_VERSION}.0
  DESCRIPTION "${PDFT_DESCRIPTION}"
  LANGUAGES CXX RC)

if(NOT MSVC OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "Build with MSVC (cl.exe) from a Visual Studio developer environment: "
                      "scripts\\build.cmd <preset>")
endif()
if(MSVC_VERSION LESS 1950)
  message(FATAL_ERROR "MSVC ${CMAKE_CXX_COMPILER_VERSION} is too old: use Visual Studio 2026 (v145) or later")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)   # no C++20 modules; skips the dependency scan
# Static CRT. A cache entry, so a deliberate -DCMAKE_MSVC_RUNTIME_LIBRARY=... can override it;
# the gate.imports.* tests then fail, which is how the imports gate is checked.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING
    "MSVC runtime library; the shipped binaries must use the static CRT")
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "Embedded")                        # /Z7; link writes PDBs
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")   # DLLs and exes side by side
set(CMAKE_PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(PDFT_BUILD_APP "Build the settings app (the settings and command-line executables)" ON)
# The developer install verbs (install from the build's own folder, the elevated machine-install
# copy). Release builds, which an installer puts in place, turn this off. The app reads it as
# pdft::kDevInstall (project_config.h).
option(PDFT_DEV_INSTALL "Developer install verbs in the settings app (OFF for release builds)" ON)
# Whether Save in the settings window and --zoom refresh PDF thumbnails in place; otherwise they
# offer the full rebuild of the thumbnail cache, which restarts File Explorer. The refresh
# experiment showed that open folder windows pick the new thumbnails up, so it is on. The app
# reads it as pdft::kPdfRefreshDefault (project_config.h).
option(PDFT_PDF_REFRESH_DEFAULT
    "Save and --zoom refresh PDF thumbnails without restarting File Explorer (set from the refresh experiment)"
    ON)
option(PDFT_BUILD_TOOLS "Build the developer tools (thumbtest, pdft-run-outside)" ON)
include(CTest)   # BUILD_TESTING (default ON): gates, unit, handler and integration tests

include(cmake/TargetArch.cmake)
include(cmake/Hardening.cmake)
include(cmake/Pins.cmake)
include(cmake/FetchVerified.cmake)
include(cmake/Dependencies.cmake)
pdft_generate_project_config()

add_subdirectory(src/common)   # header-only: zoom, crop math, the private preview interface
add_subdirectory(src/handler)
if(PDFT_BUILD_APP)
  add_subdirectory(src/app)   # the settings and command-line executables
endif()
if(PDFT_BUILD_TOOLS OR BUILD_TESTING)
  add_subdirectory(tests/support)   # test PDFs, Medium-IL relaunch, temp dirs, HKCU redirection
endif()
if(PDFT_BUILD_TOOLS)
  add_subdirectory(tools/thumbtest)
  # pdft-run-outside: runs a program in the signed-in user's own, unvirtualized context.
  add_subdirectory(tools/run-outside)
endif()
if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

include(cmake/Install.cmake)

if(BUILD_TESTING)
  include(cmake/Gates.cmake)
endif()
