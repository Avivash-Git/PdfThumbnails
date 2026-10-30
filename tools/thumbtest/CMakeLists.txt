# thumbtest.exe: renders PDFs through the handler (directly or through Explorer's thumbnail
# cache) and reports sizes, timings and PNGs; writes the synthetic test PDFs and the app's
# preview samples; compares PNG pixels. Developer and test tool; never shipped.

add_executable(thumbtest thumbtest.cpp)
pdft_add_version_resource(thumbtest FILETYPE VFT_APP
  DESCRIPTION "${PDFT_PRODUCT_NAME} thumbnail test tool")
pdft_harden(thumbtest EXE)
target_link_libraries(thumbtest PRIVATE pdft_testsupport
                                        ole32 uuid shell32 shlwapi gdi32 user32 windowscodecs)
