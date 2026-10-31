# Install components. `cmake --install <build> --prefix <dir> --component <name>`:
#   runtime  the flat staging tree that --install (and later the installer) ships:
#              PdfThumbnailsSettings.exe, PdfThumbnailsCli.exe   (when the app is built)
#              PdfThumbnails.dll, PdfThumbnails-pdfium.dll
#              LICENSE.txt, THIRD-PARTY-NOTICES.md
#              licenses/pdfium/<every license file in the PDFium archive>
#              licenses/pdfium-binaries-LICENSE.txt
#              licenses/wil-LICENSE.txt                          (when the app is built)
#            The gate.* tests stage this component and check it (cmake/Gates.cmake).
#   symbols  PDBs of the binaries in runtime (the test programs carry theirs in tests)
#   tests    the test programs, thumbtest and pdft-run-medium (with their PDBs) next to the
#            runtime they exercise (the handler, PDFium and the app's executables, which tests
#            start), with the same notices, because the bundle redistributes them too, plus
#            licenses/googletest-LICENSE.txt (the test programs link GoogleTest), and a
#            CTestTestfile.cmake with relative paths (tests/CMakeLists.txt), so
#            `ctest --test-dir <dir>` runs the bundle on another machine (CI: the ARM64 runner).
#            A new test program joins the bundle through PDFT_TEST_BUNDLE_PROGRAMS below (and
#            registers its tests with pdft_add_program_test in tests/CMakeLists.txt).
# pdft-run-outside (tools/run-outside) is for developer PCs and in no component.

include_guard(GLOBAL)

# pdft_install_notices(<component>): the project license, the third-party notices and every
# third-party license file, laid out as in the runtime tree.
function(pdft_install_notices component)
  install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
          DESTINATION . RENAME LICENSE.txt COMPONENT ${component})
  install(FILES "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.md"
          DESTINATION . COMPONENT ${component})
  install(DIRECTORY "${PDFT_PDFIUM_DIR}/licenses/"
          DESTINATION licenses/pdfium COMPONENT ${component})
  install(FILES "${PDFT_PDFIUM_DIR}/LICENSE"
          DESTINATION licenses RENAME pdfium-binaries-LICENSE.txt COMPONENT ${component})
  if(DEFINED PDFT_WIL_DIR)   # set by Dependencies.cmake when the app (which uses WIL) is built
    install(FILES "${PDFT_WIL_DIR}/LICENSE"
            DESTINATION licenses RENAME wil-LICENSE.txt COMPONENT ${component})
  endif()
endfunction()

# The shipped binaries we build (PDFium is copied, not built).
set(_pdft_shipped pdft_handler)
foreach(_pdft_target IN ITEMS PdfThumbnailsSettings PdfThumbnailsCli)
  if(TARGET ${_pdft_target})
    list(APPEND _pdft_shipped ${_pdft_target})
  endif()
endforeach()

# ---- runtime ------------------------------------------------------------------------------
foreach(_pdft_target IN LISTS _pdft_shipped)
  install(FILES "$<TARGET_FILE:${_pdft_target}>" DESTINATION . COMPONENT runtime)
endforeach()
install(FILES "${PDFT_PDFIUM_RUNTIME}" DESTINATION . COMPONENT runtime)
pdft_install_notices(runtime)

# ---- symbols ------------------------------------------------------------------------------
foreach(_pdft_target IN LISTS _pdft_shipped)
  install(FILES "$<TARGET_PDB_FILE:${_pdft_target}>" DESTINATION . COMPONENT symbols)
endforeach()

# ---- tests --------------------------------------------------------------------------------
# Test programs and tools in the bundle, if they are built. The integration tests start the
# app's executables, which come with the runtime below.
set(PDFT_TEST_BUNDLE_PROGRAMS thumbtest pdft_run_medium pdft_unit_tests pdft_handler_tests
                              pdft_refuses_elevated pdft_integration_tests)
set(_pdft_test_programs "")
foreach(_pdft_target IN LISTS PDFT_TEST_BUNDLE_PROGRAMS)
  if(TARGET ${_pdft_target})
    list(APPEND _pdft_test_programs ${_pdft_target})
  endif()
endforeach()
if(_pdft_test_programs)
  foreach(_pdft_target IN LISTS _pdft_shipped)
    install(FILES "$<TARGET_FILE:${_pdft_target}>" DESTINATION . COMPONENT tests)
  endforeach()
  install(FILES "${PDFT_PDFIUM_RUNTIME}" DESTINATION . COMPONENT tests)
  pdft_install_notices(tests)
  foreach(_pdft_target IN LISTS _pdft_test_programs)
    install(FILES "$<TARGET_FILE:${_pdft_target}>" "$<TARGET_PDB_FILE:${_pdft_target}>"
            DESTINATION . COMPONENT tests)
  endforeach()
endif()
if(DEFINED PDFT_GTEST_DIR)   # set by tests/CMakeLists.txt
  install(FILES "${PDFT_GTEST_DIR}/LICENSE"
          DESTINATION licenses RENAME googletest-LICENSE.txt COMPONENT tests)
endif()
unset(_pdft_test_programs)
unset(_pdft_shipped)
unset(_pdft_target)
