# thumbtest.exe: renders PDFs through the handler (directly or through Explorer's thumbnail
# cache) and reports sizes, timings and PNGs; writes the synthetic test PDFs and the app's
# preview samples; compares PNG pixels. With the settings app's core (PDFT_BUILD_APP) also the
# thumbnail refresh experiment's modes (refresh_modes.cpp): asking the cache what it holds,
# refreshing with the experiment's flags, the automatic verdict, the cache files' metadata, what
# the cache rebuild would find, and a File Open dialog. Developer and test tool; never shipped.

add_executable(thumbtest thumbtest.cpp thumbtest.h)
pdft_add_version_resource(thumbtest FILETYPE VFT_APP
  DESCRIPTION "${PDFT_PRODUCT_NAME} thumbnail test tool")
pdft_harden(thumbtest EXE)
target_link_libraries(thumbtest PRIVATE pdft_testsupport pdft_config pdft_common
                                        ole32 uuid shell32 shlwapi gdi32 user32 windowscodecs)
if(TARGET pdft_appcore)
  target_sources(thumbtest PRIVATE refresh_modes.cpp)
  target_link_libraries(thumbtest PRIVATE pdft_appcore advapi32)
  target_compile_definitions(thumbtest PRIVATE PDFT_THUMBTEST_REFRESH=1)
endif()
