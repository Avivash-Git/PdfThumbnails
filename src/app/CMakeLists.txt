# The settings app: two executables built from one code base.
#   pdft_appcore           STATIC   everything but the window (namespace pdft): start-up
#                                   hardening, COM security, the command line, the log, output,
#                                   named objects, the process context
#   PdfThumbnailsSettings  WIN32    ${PDFT_SETTINGS_EXE}: the window, scheduled checks, elevated
#                                   steps; shows no console window
#   PdfThumbnailsCli       console  ${PDFT_CLI_EXE}: terminals and scripts (a shell waits for it
#                                   and sees its output and exit code)
# Both are hardened like the handler, import only KnownDLLs statically and delay-load the rest
# (cmake/Hardening.cmake: use pdft_delay_load for a new DLL), embed a manifest, a version
# resource and their own resource script (res/), and are checked by the gates (cmake/Gates.cmake).

add_library(pdft_appcore STATIC
  core/app_main.cpp core/app_main.h
  core/cli_args.cpp core/cli_args.h
  core/com_security.cpp core/com_security.h
  core/commands.cpp core/commands.h
  core/context.cpp core/context.h
  core/exit_code.h
  core/instance.cpp core/instance.h
  core/log.cpp core/log.h
  core/mode.h
  core/output.cpp core/output.h
  core/paths.cpp core/paths.h
  core/startup.cpp core/startup.h
  core/strings.cpp core/strings.h
  core/wmi_registry.cpp core/wmi_registry.h
  res/resource_ids.h)
target_include_directories(pdft_appcore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core"
                                               "${CMAKE_CURRENT_SOURCE_DIR}/res")
pdft_harden(pdft_appcore LIB)
# KnownDLLs only (gate.imports.*). uuid is a static library of GUIDs (the known-folder IDs), not
# a DLL.
target_link_libraries(pdft_appcore PUBLIC pdft_config pdft_common pdft_wil
                                   PRIVATE ole32 oleaut32 advapi32 shell32 user32 uuid)

# pdft_add_app_exe(<target> <WIN32|CONSOLE> <file name> <manifest template> <description>
#                  <source>...)
function(pdft_add_app_exe target subsystem file_name manifest description)
  if(subsystem STREQUAL "WIN32")
    add_executable(${target} WIN32 ${ARGN})
  else()
    add_executable(${target} ${ARGN})
  endif()
  cmake_path(GET file_name STEM name)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME "${name}")
  pdft_add_version_resource(${target} FILETYPE VFT_APP DESCRIPTION "${description}")
  pdft_add_manifest(${target} TEMPLATE ${manifest})
  pdft_harden(${target} EXE)
  target_link_libraries(${target} PRIVATE pdft_appcore)
endfunction()

pdft_add_app_exe(PdfThumbnailsSettings WIN32 "${PDFT_SETTINGS_EXE}" settings
                 "${PDFT_PRODUCT_NAME} settings" main_gui.cpp res/settings.rc)
# Common Controls v6 (TaskDialog), bound through the manifest; the settings executable only.
target_link_libraries(PdfThumbnailsSettings PRIVATE comctl32)
pdft_add_app_exe(PdfThumbnailsCli CONSOLE "${PDFT_CLI_EXE}" cli
                 "${PDFT_PRODUCT_NAME} command line" main_cli.cpp res/cli.rc)
