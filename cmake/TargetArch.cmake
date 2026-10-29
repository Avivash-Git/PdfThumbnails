# PDFT_ARCH (x64 | arm64) and PDFT_PE_MACHINE (the PE header machine, as dumpbin prints it),
# taken from the architecture cl.exe actually targets. The presets pass PDFT_EXPECTED_ARCH so a
# preset run from the wrong developer environment fails here instead of producing binaries
# for the wrong machine in the wrong folder.

include_guard(GLOBAL)

if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
  set(PDFT_ARCH "x64")
  set(PDFT_PE_MACHINE "8664")
elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
  set(PDFT_ARCH "arm64")
  set(PDFT_PE_MACHINE "AA64")
else()
  message(FATAL_ERROR "Unsupported target architecture '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}': "
                      "build for x64 or ARM64 (vcvarsall x64, x64_arm64 or arm64)")
endif()

set(PDFT_EXPECTED_ARCH "" CACHE STRING "Fail unless the compiler targets this architecture (x64|arm64)")
if(PDFT_EXPECTED_ARCH AND NOT PDFT_EXPECTED_ARCH STREQUAL PDFT_ARCH)
  message(FATAL_ERROR "This preset builds ${PDFT_EXPECTED_ARCH}, but cl.exe targets ${PDFT_ARCH}. "
                      "Use scripts\\build.cmd <preset>, or a developer prompt for ${PDFT_EXPECTED_ARCH}.")
endif()

# The MSVC tools folder (...\VC\Tools\MSVC\<version>), from cl.exe's own location
# (...\bin\Host<host>\<target>\cl.exe), and its Spectre-mitigated runtime libraries.
cmake_path(GET CMAKE_CXX_COMPILER PARENT_PATH _pdft_dir)       # ...\bin\Host<host>\<target>
cmake_path(GET _pdft_dir PARENT_PATH _pdft_dir)                # ...\bin\Host<host>
cmake_path(GET _pdft_dir PARENT_PATH _pdft_dir)                # ...\bin
cmake_path(GET _pdft_dir PARENT_PATH PDFT_VCTOOLS_DIR)         # ...\VC\Tools\MSVC\<version>
set(PDFT_SPECTRE_LIB_DIR "${PDFT_VCTOOLS_DIR}/lib/spectre/${PDFT_ARCH}")
unset(_pdft_dir)

message(STATUS "${PROJECT_NAME}: target ${PDFT_ARCH}, MSVC ${CMAKE_CXX_COMPILER_VERSION} "
               "(toolset v${MSVC_TOOLSET_VERSION}), CMake ${CMAKE_VERSION}, ${CMAKE_GENERATOR}")
