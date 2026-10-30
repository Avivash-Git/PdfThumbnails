# pdft_common: header-only code shared by the handler DLL, the tests and tools, and later the
# settings app:
#   zoom.h              the zoom range, the name of its registry value, ClampZoom()
#   crop_math.h         ComputeCrop(): bitmap size and page placement of a thumbnail
#   preview_override.h  IPdfThumbnailsPreviewOptions, the handler's private interface for a
#                       preview at a zoom that is not saved yet

add_library(pdft_common INTERFACE)
target_include_directories(pdft_common INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(pdft_common INTERFACE pdft_config)   # preview_override.h: kPreviewIid
