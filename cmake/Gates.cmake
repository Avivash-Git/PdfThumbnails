# Gates: checks of what we ship, as CTest tests with the label "gate" (run with
# `ctest --preset <preset> -L gate`). They are static checks of the staged files and never run
# the built binaries, so cross-compiled ARM64 builds run them on the x64 host too; the one
# exception is gate.planting, which starts copies of the two executables and is registered for
# native builds only. It also has the label "planting", so `ctest -L gate -LE planting` runs the
# static checks alone. Every gate inspects a fresh `cmake --install --component runtime`
# (fixture gate.stage, in <build>/gate-stage) with dumpbin and `cmake -P` scripts from
# cmake/gates.
#
#   gate.stage                 fixture: stages the runtime component
#   gate.<bin>.exports         exact export list (handler: DllCanUnloadNow, DllGetClassObject)
#   gate.imports.<bin>         imports on an allow list; no CRT DLLs; delay-loads only if listed
#   gate.hardening.<bin>       CFG, CET (x64), EH continuation (x64), NX, ASLR, dependent load
#                              flag 0x800, PDB name only, no build-machine paths; the app's
#                              executables also without WIL's process-wide failure cache
#   gate.version.<bin>         FileVersion = project version, OriginalFilename = file name
#   gate.manifest.<bin>        the app's executables: one embedded manifest, as the template says
#   gate.planting              the app's executables, copied to a fresh folder next to DLLs named
#                              like the ones they (or Windows on their behalf) might load, load
#                              none of them (native builds only: it runs the executables; also
#                              labelled "planting")
#   gate.pdfium.pe             PDFium's own (shorter) hardening profile
#   gate.pdfium.binds          src/handler/pdfium_functions.inc has 18 entries, all exported
#   gate.pdfium.hash           the staged PDFium DLL is the pinned one
#   gate.licenses              every license and notice is staged and named in the notices
#   gate.licenses.tests        the same for the tests component (test bundle), which also
#                              carries GoogleTest's license (fixture gate.stage.tests)
#   gate.buildid               cmake/BuildId.cmake: fixed vector + the staged DLLs
#
# Our own binaries register through pdft_add_binary_gates().

include_guard(GLOBAL)

# dumpbin from the same toolset as the linker. Hostx64\arm64\dumpbin.exe reads ARM64 and x64
# images alike.
cmake_path(GET CMAKE_LINKER PARENT_PATH _pdft_linker_dir)
find_program(PDFT_DUMPBIN NAMES dumpbin HINTS "${_pdft_linker_dir}" NO_DEFAULT_PATH)
find_program(PDFT_DUMPBIN NAMES dumpbin)
unset(_pdft_linker_dir)
if(NOT PDFT_DUMPBIN)
  message(FATAL_ERROR "dumpbin.exe not found next to ${CMAKE_LINKER}: the gates need it")
endif()

set(PDFT_GATE_SCRIPTS "${PROJECT_SOURCE_DIR}/cmake/gates")
set(PDFT_GATE_STAGE "${CMAKE_BINARY_DIR}/gate-stage")
# Text that must never appear in a shipped binary: this checkout, this build tree, and any
# user profile path.
set(PDFT_GATE_FORBIDDEN_PATHS "${PROJECT_SOURCE_DIR}|${CMAKE_BINARY_DIR}|/Users/")

add_test(NAME gate.stage
  COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DSTAGE_DIR=${PDFT_GATE_STAGE}"
          "-DCONFIG=$<CONFIG>" -DCOMPONENT=runtime -P "${PDFT_GATE_SCRIPTS}/Stage.cmake")
set_tests_properties(gate.stage PROPERTIES LABELS gate FIXTURES_SETUP gate.stage)

# pdft_add_gate(<test name> <script> [-D<var>=<value>]...): one gate test on the staged tree.
function(pdft_add_gate name script)
  add_test(NAME ${name}
    COMMAND "${CMAKE_COMMAND}" "-DDUMPBIN=${PDFT_DUMPBIN}" ${ARGN} -P "${PDFT_GATE_SCRIPTS}/${script}")
  set_tests_properties(${name} PROPERTIES
    LABELS gate
    ENVIRONMENT VSLANG=1033
    FIXTURES_REQUIRED gate.stage)
endfunction()

# pdft_add_binary_gates(<short name> <target> IMPORTS <dll>... [DELAY_IMPORTS <dll>...]
#                       [EXPORTS <name>...] [MANIFEST settings|cli] [USES_WIL])
# The gates for one of our own staged binaries. Import names compare case-insensitively; a
# trailing * matches any rest. Without EXPORTS the export list is not checked; MANIFEST names the
# executable's manifest template (gate.manifest.<short name>). USES_WIL: the binary compiles WIL,
# and gate.hardening.<short name> also checks that WIL's process-wide failure cache is left out.
function(pdft_add_binary_gates short target)
  cmake_parse_arguments(PARSE_ARGV 2 arg "USES_WIL" "MANIFEST" "IMPORTS;DELAY_IMPORTS;EXPORTS")
  if(NOT arg_IMPORTS)
    message(FATAL_ERROR "pdft_add_binary_gates(${short}): IMPORTS is required")
  endif()
  set(file "${PDFT_GATE_STAGE}/$<TARGET_FILE_NAME:${target}>")
  list(JOIN arg_IMPORTS "," allow)
  list(JOIN arg_DELAY_IMPORTS "," delay_allow)
  pdft_add_gate(gate.imports.${short} CheckImports.cmake
    "-DFILE=${file}" "-DALLOW=${allow}" "-DDELAY_ALLOW=${delay_allow}")
  set(uses_wil "")
  if(arg_USES_WIL)
    set(uses_wil -DUSES_WIL=ON)
  endif()
  pdft_add_gate(gate.hardening.${short} CheckHardening.cmake
    "-DFILE=${file}" -DPROFILE=own "-DARCH=${PDFT_ARCH}" "-DMACHINE=${PDFT_PE_MACHINE}"
    "-DFORBIDDEN=${PDFT_GATE_FORBIDDEN_PATHS}" ${uses_wil})
  pdft_add_gate(gate.version.${short} CheckVersion.cmake
    "-DFILE=${file}" "-DVERSION=${PROJECT_VERSION}")
  if(arg_EXPORTS)
    list(JOIN arg_EXPORTS "," exports)
    pdft_add_gate(gate.${short}.exports CheckExports.cmake "-DFILE=${file}" "-DEXPECTED=${exports}")
  endif()
  if(arg_MANIFEST)
    pdft_add_gate(gate.manifest.${short} CheckManifest.cmake
      "-DFILE=${file}" "-DKIND=${arg_MANIFEST}" "-DARCH=${PDFT_ARCH}")
  endif()
endfunction()

# ---- the handler ----------------------------------------------------------------------------
pdft_add_binary_gates(handler pdft_handler
  EXPORTS DllCanUnloadNow DllGetClassObject
  IMPORTS KERNEL32.dll ADVAPI32.dll OLE32.dll SHELL32.dll GDI32.dll)

# ---- the app's executables ------------------------------------------------------------------
# Static imports: KnownDLLs only, plus COMCTL32 for the settings executable, which its manifest
# binds to Common Controls v6 in WinSxS. Everything else must be delay-loaded from the list in
# cmake/Hardening.cmake (see there for why).
if(TARGET PdfThumbnailsSettings)
  pdft_add_binary_gates(settings PdfThumbnailsSettings
    IMPORTS ${PDFT_EXE_STATIC_IMPORTS} COMCTL32.dll
    DELAY_IMPORTS ${PDFT_EXE_DELAY_LOADS}
    MANIFEST settings
    USES_WIL)
endif()
if(TARGET PdfThumbnailsCli)
  pdft_add_binary_gates(cli PdfThumbnailsCli
    IMPORTS ${PDFT_EXE_STATIC_IMPORTS}
    DELAY_IMPORTS ${PDFT_EXE_DELAY_LOADS}
    MANIFEST cli
    USES_WIL)
endif()

# gate.planting (tests/planting): copies the staged executables into a fresh folder under
# %TEMP%, plants a canary DLL there under every name below, runs each executable with --version
# and --help (and, when run elevated as on CI, --status --no-live-test), and fails if any canary
# was loaded. The names: every DLL the executables may delay-load, Common Controls (bound
# through the manifest), and DLLs that Windows' own DLLs load by name on a program's behalf, the
# usual targets of DLL planting. Skipped (exit 77) only when Smart App Control or another
# Application Control policy blocks the canary or an executable, because a canary that cannot
# load would make the check pass for the wrong reason; any other failure of the canary fails.
# It starts programs, so it is registered for native builds only.
set(PDFT_PLANTING_NAMES ${PDFT_EXE_DELAY_LOADS}
  comctl32.dll cryptbase.dll cryptsp.dll dbghelp.dll profapi.dll sspicli.dll winmm.dll
  wtsapi32.dll)
list(REMOVE_DUPLICATES PDFT_PLANTING_NAMES)
if(TARGET pdft_planting_check AND NOT CMAKE_CROSSCOMPILING)
  set(_pdft_args --canary "$<TARGET_FILE:pdft_planting_canary>")
  foreach(_pdft_target IN ITEMS PdfThumbnailsSettings PdfThumbnailsCli)
    list(APPEND _pdft_args --exe "${PDFT_GATE_STAGE}/$<TARGET_FILE_NAME:${_pdft_target}>")
  endforeach()
  foreach(_pdft_name IN LISTS PDFT_PLANTING_NAMES)
    list(APPEND _pdft_args --plant "${_pdft_name}")
  endforeach()
  add_test(NAME gate.planting COMMAND pdft_planting_check ${_pdft_args})
  set_tests_properties(gate.planting PROPERTIES
    LABELS "gate;planting"
    FIXTURES_REQUIRED gate.stage
    SKIP_RETURN_CODE 77)
  unset(_pdft_args)
  unset(_pdft_target)
  unset(_pdft_name)
endif()

# ---- PDFium (third-party binary, shipped under our private name) ----------------------------
set(_pdft_pdfium "${PDFT_GATE_STAGE}/${PDFT_PDFIUM_DLL}")
pdft_add_gate(gate.imports.pdfium CheckImports.cmake
  "-DFILE=${_pdft_pdfium}" "-DALLOW=KERNEL32.dll,ADVAPI32.dll,GDI32.dll,USER32.dll")
pdft_add_gate(gate.pdfium.pe CheckHardening.cmake
  "-DFILE=${_pdft_pdfium}" -DPROFILE=pdfium "-DARCH=${PDFT_ARCH}" "-DMACHINE=${PDFT_PE_MACHINE}")
pdft_add_gate(gate.pdfium.hash CheckPdfiumHash.cmake
  "-DFILE=${_pdft_pdfium}" "-DEXPECTED=${PDFT_PDFIUM_${PDFT_ARCH}_DLL_SHA256}")
# The count is deliberate: a change to the bound functions must change it too.
pdft_add_gate(gate.pdfium.binds CheckBinds.cmake
  "-DLIST=${PROJECT_SOURCE_DIR}/src/handler/pdfium_functions.inc" -DCOUNT=18 "-DFILE=${_pdft_pdfium}")

# ---- the package as a whole -----------------------------------------------------------------
set(_pdft_wil_license "")
if(DEFINED PDFT_WIL_DIR)
  set(_pdft_wil_license "-DWIL_LICENSE=${PDFT_WIL_DIR}/LICENSE")
endif()
pdft_add_gate(gate.licenses CheckLicenses.cmake
  "-DSTAGE_DIR=${PDFT_GATE_STAGE}" "-DPDFIUM_DIR=${PDFT_PDFIUM_DIR}"
  "-DPROJECT_LICENSE=${PROJECT_SOURCE_DIR}/LICENSE"
  "-DNOTICES=${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md" ${_pdft_wil_license})
# The test bundle (CI artifact) redistributes PDFium and links GoogleTest: same check, own stage.
if(DEFINED PDFT_GTEST_DIR)
  set(_pdft_tests_stage "${CMAKE_BINARY_DIR}/gate-stage-tests")
  add_test(NAME gate.stage.tests
    COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}" "-DSTAGE_DIR=${_pdft_tests_stage}"
            "-DCONFIG=$<CONFIG>" -DCOMPONENT=tests -P "${PDFT_GATE_SCRIPTS}/Stage.cmake")
  set_tests_properties(gate.stage.tests PROPERTIES LABELS gate FIXTURES_SETUP gate.stage.tests)
  add_test(NAME gate.licenses.tests
    COMMAND "${CMAKE_COMMAND}" "-DSTAGE_DIR=${_pdft_tests_stage}" "-DPDFIUM_DIR=${PDFT_PDFIUM_DIR}"
            "-DPROJECT_LICENSE=${PROJECT_SOURCE_DIR}/LICENSE"
            "-DNOTICES=${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
            "-DGTEST_LICENSE=${PDFT_GTEST_DIR}/LICENSE" ${_pdft_wil_license}
            -P "${PDFT_GATE_SCRIPTS}/CheckLicenses.cmake")
  set_tests_properties(gate.licenses.tests PROPERTIES
    LABELS gate FIXTURES_REQUIRED gate.stage.tests)
  unset(_pdft_tests_stage)
endif()
pdft_add_gate(gate.buildid CheckBuildId.cmake
  "-DSCRIPT=${PROJECT_SOURCE_DIR}/cmake/BuildId.cmake" "-DWORK_DIR=${CMAKE_BINARY_DIR}/gate-buildid"
  "-DDLL=${PDFT_GATE_STAGE}/$<TARGET_FILE_NAME:pdft_handler>"
  "-DPDFIUM=${_pdft_pdfium}" "-DVERSION=${PROJECT_VERSION}")
unset(_pdft_pdfium)
unset(_pdft_wil_license)
