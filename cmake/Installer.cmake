# Builds the installer, one setup for x64 and ARM64 PCs, with Inno Setup from the staged runtimes
# of both architectures (cmake --install <build> --prefix <folder> --component runtime of the
# release-x64 and release-arm64 presets):
#
#   cmake -DRUNTIME_X64=<folder> -DRUNTIME_ARM64=<folder> -DOUTPUT_DIR=<folder>
#         [-DOUTPUT_BASE=<file name without .exe>] [-DVERSION_TEXT=<text>] [-DISCC=<ISCC.exe>]
#         [-DSIGN=ON | -DSIGN=EXTERNAL -DUNINSTALLER_DIR=<folder> [-DPREPARE=ON]]
#         -P cmake/Installer.cmake
#
# (-D options go before -P.) Run it after code signing, since signing changes the DLLs' bytes and
# so their build folder names (BuildId.cmake). Signed builds:
#   SIGN=ON        the runtimes signed with cmake/Sign.cmake -DDIR=<runtime>, and its PDFT_SIGNING
#                  environment set: it first checks that every binary in both runtimes is signed
#                  and timestamped, and Inno Setup signs the setup, its uninstaller and its
#                  temporary copies through cmake/Sign.cmake.
#   SIGN=EXTERNAL  for a signing service that signs the files it is sent (SignPath), in two
#                  passes. With PREPARE=ON it only has Inno Setup write its uninstaller, unsigned,
#                  into UNINSTALLER_DIR (uninst-*.e32) and prints its path; the service signs it
#                  together with the runtimes. The second pass, without PREPARE, checks that the
#                  runtimes and that uninstaller are signed and timestamped, and builds the setup
#                  with it (Setup's temporary copies of itself are that signed file too). The
#                  setup itself comes out unsigned, for the service to sign last.
# It
#   1. checks that both runtimes are complete, built for their architecture, and of one version;
#   2. computes each build's folder name, <FileVersion>-<build id>, from the final DLLs;
#   3. finds the Inno Setup compiler (-DISCC, else Inno Setup 7, else 6, else ISCC on PATH) and
#      prints its version;
#   4. compiles installer/PdfThumbnails.iss with every name and version passed as /D values, so
#      the script holds none of them itself;
#   5. prints the setup's path and SHA-256.
# VERSION_TEXT replaces the version Installed apps shows (the exe's ProductVersion string); CI
# uses it for the earlier setup of its upgrade test. OUTPUT_BASE defaults to
# <product>-<ProductVersion without build metadata>-setup.

cmake_minimum_required(VERSION 4.3)
include("${CMAKE_CURRENT_LIST_DIR}/ProjectInfo.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/gates/PeTools.cmake")
pdft_require(RUNTIME_X64 RUNTIME_ARM64 OUTPUT_DIR)

get_filename_component(source_dir "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(iss "${source_dir}/installer/PdfThumbnails.iss")

# ---- 1. The runtimes --------------------------------------------------------------------------
set(versions "")
foreach(arch IN ITEMS x64 arm64)
  string(TOUPPER "${arch}" ARCH)
  get_filename_component(runtime "${RUNTIME_${ARCH}}" ABSOLUTE)
  set(runtime_${arch} "${runtime}")
  foreach(file IN ITEMS ${PDFT_SETTINGS_EXE} ${PDFT_CLI_EXE} ${PDFT_HANDLER_DLL} ${PDFT_PDFIUM_DLL}
                        LICENSE.txt THIRD-PARTY-NOTICES.md licenses)
    if(NOT EXISTS "${runtime}/${file}")
      message(FATAL_ERROR "${runtime}: ${file} is missing; stage the ${arch} runtime first")
    endif()
  endforeach()
  if(arch STREQUAL "x64")
    set(machine 34404)   # IMAGE_FILE_MACHINE_AMD64
  else()
    set(machine 43620)   # IMAGE_FILE_MACHINE_ARM64
  endif()
  foreach(file IN ITEMS ${PDFT_SETTINGS_EXE} ${PDFT_CLI_EXE} ${PDFT_HANDLER_DLL})
    # The PE header's Machine field: e_lfanew at 0x3C points at "PE\0\0", the field follows.
    pdft_read_hex(dos "${runtime}/${file}" 0 64)
    pdft_hex_le(pe_offset "${dos}" 60 4)
    pdft_read_hex(pe "${runtime}/${file}" ${pe_offset} 6)
    pdft_hex_le(pe_machine "${pe}" 4 2)
    if(NOT pe MATCHES "^50450000" OR NOT pe_machine EQUAL machine)
      message(FATAL_ERROR "${runtime}/${file} is not an ${arch} image (machine ${pe_machine})")
    endif()
  endforeach()
  pdft_pe_version(exe "${runtime}/${PDFT_SETTINGS_EXE}" FileVersion ProductVersion)
  if(NOT exe_FileVersion OR NOT exe_ProductVersion)
    message(FATAL_ERROR "${runtime}/${PDFT_SETTINGS_EXE} has no FileVersion or ProductVersion")
  endif()
  list(APPEND versions "${exe_FileVersion}|${exe_ProductVersion}")
  set(file_version "${exe_FileVersion}")
  set(product_version "${exe_ProductVersion}")

  # ---- 2. The build folder --------------------------------------------------------------------
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DDLL=${runtime}/${PDFT_HANDLER_DLL}"
            "-DPDFIUM=${runtime}/${PDFT_PDFIUM_DLL}" -P "${CMAKE_CURRENT_LIST_DIR}/BuildId.cmake"
    OUTPUT_VARIABLE build OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0 OR NOT build MATCHES "^[0-9]+(\\.[0-9]+)*-[0-9a-f]+$")
    message(FATAL_ERROR "BuildId.cmake failed for ${runtime} (${result}): ${build}")
  endif()
  if(NOT build MATCHES "^${file_version}-")
    message(FATAL_ERROR "${runtime}: the DLL's version (${build}) is not the program's (${file_version})")
  endif()
  set(build_${arch} "${build}")
  message(STATUS "${arch}: ${runtime}, build folder app\\${build}")
endforeach()
list(REMOVE_DUPLICATES versions)
list(LENGTH versions count)
if(NOT count EQUAL 1)
  message(FATAL_ERROR "The two runtimes are not of one version: ${versions}")
endif()

if(NOT SIGN)
  set(SIGN OFF)
endif()
string(TOUPPER "${SIGN}" SIGN)
if(SIGN STREQUAL "EXTERNAL")
  pdft_require(UNINSTALLER_DIR)
  get_filename_component(uninstaller_dir "${UNINSTALLER_DIR}" ABSOLUTE)
  file(MAKE_DIRECTORY "${uninstaller_dir}")
  file(GLOB uninstallers_before "${uninstaller_dir}/uninst-*.e32")
  if(NOT PREPARE)
    if(NOT uninstallers_before)
      message(FATAL_ERROR "SIGN=EXTERNAL: ${uninstaller_dir} holds no uninst-*.e32; run with "
                          "-DPREPARE=ON first and have it signed")
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" "-DPATHS=${runtime_x64};${runtime_arm64};${uninstallers_before}"
              -DREQUIRE=ON -P "${CMAKE_CURRENT_LIST_DIR}/VerifySignatures.cmake"
      RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "SIGN=EXTERNAL: the runtimes and the uninstaller must be signed first")
    endif()
  endif()
elseif(PREPARE OR DEFINED UNINSTALLER_DIR)
  message(FATAL_ERROR "PREPARE and UNINSTALLER_DIR go with SIGN=EXTERNAL")
elseif(SIGN)
  if("$ENV{PDFT_SIGNING}" STREQUAL "")
    message(FATAL_ERROR "SIGN=ON needs PDFT_SIGNING and its settings in the environment "
                        "(cmake/Sign.cmake)")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPATHS=${runtime_x64};${runtime_arm64}" -DREQUIRE=ON
            -P "${CMAKE_CURRENT_LIST_DIR}/VerifySignatures.cmake"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "SIGN=ON: the runtimes must be signed first (cmake/Sign.cmake -DDIR=...)")
  endif()
endif()

if(NOT VERSION_TEXT)
  set(VERSION_TEXT "${product_version}")
endif()
if(NOT OUTPUT_BASE)
  string(REGEX REPLACE "[-+].*$" "" short_version "${product_version}")
  set(OUTPUT_BASE "${PDFT_PRODUCT_NAME}-${short_version}-setup")
endif()
if(NOT OUTPUT_BASE MATCHES "^[A-Za-z0-9._-]+$")
  message(FATAL_ERROR "OUTPUT_BASE '${OUTPUT_BASE}' is not a plain file name")
endif()

# ---- 3. The compiler --------------------------------------------------------------------------
# The install folders first, then PATH: a package manager's shim on PATH (Chocolatey's, on
# GitHub's runners) is a small launcher without the compiler's version.
if(NOT ISCC)
  find_program(ISCC NAMES ISCC.exe NO_CACHE NO_DEFAULT_PATH
    PATHS "$ENV{ProgramW6432}/Inno Setup 7" "$ENV{ProgramFiles}/Inno Setup 7"
          "$ENV{SystemDrive}/Program Files (x86)/Inno Setup 6"
          "$ENV{SystemDrive}/Program Files/Inno Setup 6")
endif()
if(NOT ISCC)
  find_program(ISCC NAMES ISCC.exe NO_CACHE)
endif()
if(NOT ISCC OR NOT EXISTS "${ISCC}")
  message(FATAL_ERROR "The Inno Setup compiler (ISCC.exe) was not found; install Inno Setup 6.3 "
                      "or later, or pass -DISCC=<path to ISCC.exe>")
endif()
# Its banner names its version ("Inno Setup 6 Command-Line Compiler" ... "Compiler engine
# version: Inno Setup 6.7.1"); its version resource is not one the PE reader understands.
execute_process(COMMAND "${ISCC}" /? OUTPUT_VARIABLE banner ERROR_VARIABLE banner)
if(banner MATCHES "Inno Setup ([0-9]+\\.[0-9]+(\\.[0-9]+)?)")
  set(iscc_version "${CMAKE_MATCH_1}")
  message(STATUS "Inno Setup compiler: ${ISCC} (${iscc_version})")
  if(iscc_version VERSION_LESS "6.3")
    message(FATAL_ERROR "Inno Setup ${iscc_version} is too old: the script needs 6.3 or later")
  endif()
else()
  message(STATUS "Inno Setup compiler: ${ISCC} (version not shown; the script needs 6.3 or later)")
endif()

# ---- 4. Compile -------------------------------------------------------------------------------
get_filename_component(output_dir "${OUTPUT_DIR}" ABSOLUTE)
if(PREPARE)
  # A setup this pass makes is thrown away: only the uninstaller matters.
  set(output_dir "${output_dir}/prepare")
endif()
file(MAKE_DIRECTORY "${output_dir}")
# The class GUID and the AppId are passed without their braces: Inno Setup reads "{" as the start
# of a constant, and the script adds them back escaped.
string(REGEX REPLACE "[{}]" "" app_id "${PDFT_INNO_APPID}")
foreach(path IN ITEMS runtime_x64 runtime_arm64 output_dir)
  file(TO_NATIVE_PATH "${${path}}" ${path}_native)
endforeach()
file(TO_NATIVE_PATH "${source_dir}/src/app/res/app.ico" icon_native)
file(TO_NATIVE_PATH "${iss}" iss_native)
set(defines
  "/DAppName=${PDFT_PRODUCT_NAME}"
  "/DAppIdGuid=${app_id}"
  "/DPublisher=${PDFT_PUBLISHER}"
  "/DHomepage=${PDFT_HOMEPAGE}"
  "/DCopyright=${PDFT_COPYRIGHT}"
  "/DDescription=${PDFT_DESCRIPTION}"
  "/DSettingsExe=${PDFT_SETTINGS_EXE}"
  "/DCliExe=${PDFT_CLI_EXE}"
  "/DHandlerDll=${PDFT_HANDLER_DLL}"
  "/DPdfiumDll=${PDFT_PDFIUM_DLL}"
  "/DFileVersion=${file_version}"
  "/DVersionText=${VERSION_TEXT}"
  "/DRuntimeX64=${runtime_x64_native}"
  "/DRuntimeArm64=${runtime_arm64_native}"
  "/DBuildX64=${build_x64}"
  "/DBuildArm64=${build_arm64}"
  "/DSetupIcon=${icon_native}"
  "/DOutputDir=${output_dir_native}"
  "/DOutputBase=${OUTPUT_BASE}")
set(sign_tool "")
if(SIGN STREQUAL "EXTERNAL")
  file(TO_NATIVE_PATH "${uninstaller_dir}" uninstaller_dir_native)
  list(APPEND defines "/DSignedUninstallerDir=${uninstaller_dir_native}")
elseif(SIGN)
  # Inno Setup runs this command for each file it signs: $f is the file's name, quoted, and $q a
  # quote. The signing identity reaches cmake/Sign.cmake through the environment.
  file(TO_NATIVE_PATH "${CMAKE_COMMAND}" cmake_native)
  file(TO_NATIVE_PATH "${CMAKE_CURRENT_LIST_DIR}/Sign.cmake" sign_native)
  set(sign_tool "/Spdftsign=$q${cmake_native}$q -DFILES=$f -P $q${sign_native}$q")
  list(APPEND defines "/DSignToolName=pdftsign")
endif()
execute_process(COMMAND "${ISCC}" /Qp ${defines} ${sign_tool} "${iss_native}"
                COMMAND_ECHO STDOUT RESULT_VARIABLE result)
if(PREPARE)
  # Without a Sign Tool, Inno Setup writes a new uninstaller into SignedUninstallerDir when it has
  # no signed one for this script and compiler, and may stop there with an error asking for it to
  # be signed: either way, that new file is what this pass is for.
  file(GLOB uninstallers "${uninstaller_dir}/uninst-*.e32")
  if(uninstallers_before)
    list(REMOVE_ITEM uninstallers ${uninstallers_before})
  endif()
  file(REMOVE_RECURSE "${output_dir}")
  list(LENGTH uninstallers count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "ISCC (exit ${result}) wrote ${count} new uninst-*.e32 files into "
                        "${uninstaller_dir}, not one (it reuses one already there: start empty)")
  endif()
  message(STATUS "Uninstaller to sign: ${uninstallers}")
  return()
endif()
if(NOT result EQUAL 0)
  message(FATAL_ERROR "ISCC failed (${result})")
endif()

# ---- 5. The result ----------------------------------------------------------------------------
set(setup "${output_dir}/${OUTPUT_BASE}.exe")
if(NOT EXISTS "${setup}")
  message(FATAL_ERROR "ISCC made no ${setup}")
endif()
if(SIGN STREQUAL "ON")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" "-DPATHS=${setup}" -DREQUIRE=ON
            -P "${CMAKE_CURRENT_LIST_DIR}/VerifySignatures.cmake"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "The setup is not signed and timestamped")
  endif()
endif()
file(SHA256 "${setup}" hash)
message(STATUS "Setup: ${setup}")
message(STATUS "SHA-256: ${hash}")
message(STATUS "Version: ${VERSION_TEXT}; x64 app\\${build_x64}; ARM64 app\\${build_arm64}")
