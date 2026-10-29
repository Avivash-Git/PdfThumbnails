# PdfThumbnails.dll: the thumbnail handler that Explorer's thumbnail surrogate loads.
# Plain Win32 + the PDFium headers; PDFium itself is loaded at run time from the DLL's own
# folder under the private name ${PDFT_PDFIUM_DLL}.

add_library(pdft_handler SHARED PdfThumbnails.cpp)
cmake_path(GET PDFT_HANDLER_DLL STEM _pdft_handler_name)
set_target_properties(pdft_handler PROPERTIES OUTPUT_NAME "${_pdft_handler_name}" PREFIX "")
unset(_pdft_handler_name)

pdft_add_handler_def(pdft_handler)
pdft_add_version_resource(pdft_handler FILETYPE VFT_DLL DESCRIPTION "${PDFT_DESCRIPTION}")
pdft_harden(pdft_handler DLL)
target_compile_options(pdft_handler PRIVATE /GR-)   # no RTTI, as in 1.1
target_link_libraries(pdft_handler PRIVATE pdft_pdfium_headers ole32 uuid shell32 gdi32 advapi32)
add_dependencies(pdft_handler pdft_pdfium_runtime)
