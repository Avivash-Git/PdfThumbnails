# Install components. `cmake --install <build> --prefix <dir> --component <name>`:
#   runtime  the flat staging tree that --install (and, in M3, the installer) ships:
#            PdfThumbnails.dll, PdfThumbnails-pdfium.dll, LICENSE.txt,
#            licenses/pdfium/<PDFium's license files>, licenses/pdfium-binaries-LICENSE.txt
#   symbols  PDBs of our own binaries
#   tests    developer tools next to the runtime they exercise (thumbtest)

include_guard(GLOBAL)

# ---- runtime ------------------------------------------------------------------------------
install(FILES "$<TARGET_FILE:pdft_handler>" "${PDFT_PDFIUM_RUNTIME}"
        DESTINATION . COMPONENT runtime)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION . RENAME LICENSE.txt COMPONENT runtime)
install(DIRECTORY "${PDFT_PDFIUM_DIR}/licenses/"
        DESTINATION licenses/pdfium COMPONENT runtime)
install(FILES "${PDFT_PDFIUM_DIR}/LICENSE"
        DESTINATION licenses RENAME pdfium-binaries-LICENSE.txt COMPONENT runtime)

# ---- symbols ------------------------------------------------------------------------------
install(FILES "$<TARGET_PDB_FILE:pdft_handler>" DESTINATION . COMPONENT symbols)

# ---- tests --------------------------------------------------------------------------------
if(TARGET thumbtest)
  install(FILES "$<TARGET_FILE:thumbtest>" "$<TARGET_FILE:pdft_handler>" "${PDFT_PDFIUM_RUNTIME}"
          DESTINATION . COMPONENT tests)
  install(FILES "$<TARGET_PDB_FILE:thumbtest>" DESTINATION . COMPONENT symbols)
endif()
