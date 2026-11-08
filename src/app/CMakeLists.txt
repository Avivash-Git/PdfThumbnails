# The settings app: two executables built from one code base.
#   pdft_appcore           STATIC   everything but the window (namespace pdft): start-up
#                                   hardening, COM security, the command line, the log, output,
#                                   named objects, the process context; what the checks read
#                                   and repair (the COM class and .pdf slots, settings, build
#                                   folders, DLL headers, 1.x leftovers, Smart App Control,
#                                   PdfCraft, the check task, Explorer's settings and the
#                                   Explorer test); the check itself (gathering, findings,
#                                   repairs, the operation lock around them); when the checks
#                                   run (the user's check task); its reports (the
#                                   status text, the JSON status, the masked diagnostics); the
#                                   install side (copying and removing builds, the sign-in
#                                   clean-up, build-folder permissions, processes holding DLLs,
#                                   the elevation boundary, its report pipe and where its steps
#                                   failed, the installer's records); the 1.x clean-up (with
#                                   the table of the files 1.x shipped,
#                                   cmake/scripts/V1FileTable.cmake) and the installer's
#                                   per-user and machine-wide steps; installing from a build's
#                                   folder and uninstalling, with their administrator steps;
#                                   File Explorer's folder windows (listed, and opened through
#                                   the running shell); rebuilding the thumbnail cache (ending
#                                   File Explorer and the thumbnail processes, deleting the cache
#                                   files, and letting Windows start File Explorer again);
#                                   refreshing cached PDF thumbnails in place (finding the PDFs in
#                                   folders, asking the cache which thumbnails it holds, making
#                                   those again in the thumbnail process, telling File Explorer)
#   PdfThumbnailsSettings  WIN32    ${PDFT_SETTINGS_EXE}: the window, scheduled checks, elevated
#                                   steps; shows no console window
#   PdfThumbnailsCli       console  ${PDFT_CLI_EXE}: terminals and scripts (a shell waits for it
#                                   and sees its output and exit code)
# Both are hardened like the handler, import only KnownDLLs statically and delay-load the rest
# (cmake/Hardening.cmake: use pdft_delay_load for a new DLL), embed a manifest, a version
# resource and their own resource script (res/), and are checked by the gates (cmake/Gates.cmake).

add_library(pdft_appcore STATIC
  core/app_main.cpp core/app_main.h
  core/build_folder.cpp core/build_folder.h
  core/check.cpp core/check.h
  core/check_schedule.cpp core/check_schedule.h
  core/cli_args.cpp core/cli_args.h
  core/com_security.cpp core/com_security.h
  core/commands.cpp core/commands.h
  core/context.cpp core/context.h
  core/delay_load.cpp core/delay_load.h
  core/diagnostics.cpp core/diagnostics.h
  core/elevation.cpp core/elevation.h
  core/evaluate.cpp core/evaluate.h
  core/exit_code.h
  core/explorer_settings.cpp core/explorer_settings.h
  core/failure_trace.cpp core/failure_trace.h
  core/file_ops.cpp core/file_ops.h
  core/inno.cpp core/inno.h
  core/install.cpp core/install.h
  core/instance.cpp core/instance.h
  core/json.cpp core/json.h
  core/legacy.cpp core/legacy.h
  core/live_test.cpp core/live_test.h
  core/log.cpp core/log.h
  core/migrate_v1.cpp core/migrate_v1.h core/v1_files.inc
  core/mode.h
  core/output.cpp core/output.h
  core/paths.cpp core/paths.h
  core/pdfcraft.cpp core/pdfcraft.h
  core/pe_info.cpp core/pe_info.h
  core/processes.cpp core/processes.h
  core/rebuild.cpp core/rebuild.h
  core/refresh.cpp core/refresh.h
  core/search_index.cpp core/search_index.h
  core/registration.cpp core/registration.h
  core/registry_roots.cpp core/registry_roots.h
  core/repair.cpp core/repair.h
  core/sac.cpp core/sac.h
  core/settings.cpp core/settings.h
  core/setup.cpp core/setup.h
  core/shell_windows.cpp core/shell_windows.h
  core/snapshot.cpp core/snapshot.h
  core/startup.cpp core/startup.h
  core/status_format.cpp core/status_format.h
  core/strings.cpp core/strings.h
  core/system_probes.cpp core/system_probes.h
  core/tasks.cpp core/tasks.h
  core/uninstall.cpp core/uninstall.h
  core/window_messages.cpp core/window_messages.h
  core/wmi_registry.cpp core/wmi_registry.h
  res/resource_ids.h)
target_include_directories(pdft_appcore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/core"
                                               "${CMAKE_CURRENT_SOURCE_DIR}/res")
pdft_harden(pdft_appcore LIB)
# KnownDLLs only (gate.imports.*). uuid is a static library of GUIDs (the known-folder IDs), not
# a DLL.
target_link_libraries(pdft_appcore PUBLIC pdft_config pdft_common pdft_wil
                                   PRIVATE ole32 oleaut32 advapi32 shell32 shlwapi user32 uuid)
# Not KnownDLLs: CNG (build ids and copies hash files), the event log (Code Integrity blocks),
# Windows Installer (finding PdfCraft), Restart Manager (which processes hold a build's DLLs) and
# userenv (the user's environment for the File Explorer the thumbnail-cache rebuild restarts).
# The event-log, Windows Installer, Restart Manager and userenv code checks that its DLL loads
# before the first call (delay_load.h), so a missing one is an answer, not a crash.
pdft_delay_load(pdft_appcore bcrypt.dll wevtapi.dll msi.dll rstrtmgr.dll userenv.dll)

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
