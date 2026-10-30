# Install components. `cmake --install <build> --prefix <dir> --component <name>`:
#   runtime  the flat staging tree that --install (and, in M3, the installer) ships:
#              PdfThumbnails.dll, PdfThumbnails-pdfium.dll   (+ the settings exes from WP6)
#              LICENSE.txt, THIRD-PARTY-NOTICES.md
#              licenses/pdfium/<every license file in the PDFium archive>
#              licenses/pdfium-binaries-LICENSE.txt
#              licenses/wil-LICENSE.txt                      (once the app compiles WIL in)
#            The gate.* tests stage this component and check it (cmake/Gates.cmake).
#   symbols  PDBs of our own binaries
#   tests    developer tools next to the runtime they exercise (thumbtest), with the same
#            notices, because the bundle redistributes PDFium too

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
if(TARGET thumbtest)
  install(FILES "$<TARGET_FILE:thumbtest>" "$<TARGET_FILE:pdft_handler>" "${PDFT_PDFIUM_RUNTIME}"
          DESTINATION . COMPONENT tests)
  pdft_install_notices(tests)
  install(FILES "$<TARGET_PDB_FILE:thumbtest>" DESTINATION . COMPONENT symbols)
endif()
