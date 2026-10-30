# Install components. `cmake --install <build> --prefix <dir> --component <name>`:
#   runtime  the flat staging tree that --install (and, in M3, the installer) ships:
#              PdfThumbnails.dll, PdfThumbnails-pdfium.dll   (+ the settings exes from WP6)
#              LICENSE.txt, THIRD-PARTY-NOTICES.md
#              licenses/pdfium/<every license file in the PDFium archive>
#              licenses/pdfium-binaries-LICENSE.txt
#              licenses/wil-LICENSE.txt                      (once the app compiles WIL in)
#            The gate.* tests stage this component and check it (cmake/Gates.cmake).
#   symbols  PDBs of the binaries in runtime (the test programs carry theirs in tests)
#   tests    the test programs and developer tools (with their PDBs) next to the runtime they
#            exercise (pdft_unit_tests, pdft_handler_tests, pdft_refuses_elevated,
#            pdft-run-medium, thumbtest), with the same notices, because the bundle
#            redistributes PDFium too, plus licenses/googletest-LICENSE.txt (the test programs
#            link GoogleTest), and a CTestTestfile.cmake with relative paths
#            (tests/CMakeLists.txt), so `ctest --test-dir <dir>` runs the bundle on another
#            machine (CI: the ARM64 runner)

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
  if(DEFINED PDFT_WIL_DIR)   # set by Dependencies.cmake once a target uses WIL
    install(FILES "${PDFT_WIL_DIR}/LICENSE"
            DESTINATION licenses RENAME wil-LICENSE.txt COMPONENT ${component})
  endif()
endfunction()

# ---- runtime ------------------------------------------------------------------------------
install(FILES "$<TARGET_FILE:pdft_handler>" "${PDFT_PDFIUM_RUNTIME}"
        DESTINATION . COMPONENT runtime)
pdft_install_notices(runtime)

# ---- symbols ------------------------------------------------------------------------------
install(FILES "$<TARGET_PDB_FILE:pdft_handler>" DESTINATION . COMPONENT symbols)

# ---- tests --------------------------------------------------------------------------------
set(_pdft_test_programs "")
foreach(_pdft_target IN ITEMS thumbtest pdft_run_medium pdft_unit_tests pdft_handler_tests
                             pdft_refuses_elevated)
  if(TARGET ${_pdft_target})
    list(APPEND _pdft_test_programs ${_pdft_target})
  endif()
endforeach()
if(_pdft_test_programs)
  install(FILES "$<TARGET_FILE:pdft_handler>" "${PDFT_PDFIUM_RUNTIME}"
          DESTINATION . COMPONENT tests)
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
unset(_pdft_target)
