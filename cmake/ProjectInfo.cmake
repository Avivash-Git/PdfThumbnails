# Every product name, ID and version lives here, and only here. The build generates
# project_config.h, the version resources and the handler's .def file from these values
# (see the "Generated files" section below), so a rename before publishing is a change to
# this file alone.
#
# Included before project(): values only, plus the functions that use them.

include_guard(GLOBAL)

# ---- Product ----------------------------------------------------------------------------
set(PDFT_PRODUCT_NAME          "PdfThumbnails")            # may change before publishing
set(PDFT_VERSION               "2.0.0")                    # 4-part: ${PDFT_VERSION}.0
set(PDFT_VERSION_SUFFIX        "" CACHE STRING
    "Appended to the ProductVersion string only (e.g. -dev+g1a2b3c4); FileVersion never changes")
set(PDFT_DESCRIPTION           "PDF thumbnails for Windows Explorer (PDFium)")
set(PDFT_COPYRIGHT             "Copyright (c) 2026 ${PDFT_PRODUCT_NAME} contributors")
# PDFT_HOMEPAGE is added when the GitHub repository exists (WP4).

# ---- COM and file names -----------------------------------------------------------------
set(PDFT_HANDLER_CLSID         "{0C3534C4-5B7A-48AB-828D-1CD660DC447F}")   # NEVER changes
set(PDFT_HANDLER_CLASS_NAME    "${PDFT_PRODUCT_NAME} PDF thumbnail provider")
# PDFT_PREVIEW_IID (private preview interface) is generated in WP5.
set(PDFT_HANDLER_DLL           "${PDFT_PRODUCT_NAME}.dll")
set(PDFT_PDFIUM_DLL            "${PDFT_PRODUCT_NAME}-pdfium.dll")
set(PDFT_SETTINGS_EXE          "${PDFT_PRODUCT_NAME}Settings.exe")
set(PDFT_CLI_EXE               "${PDFT_PRODUCT_NAME}Cli.exe")

# ---- Registry, tasks, named objects -----------------------------------------------------
set(PDFT_REG_KEY               "Software\\${PDFT_PRODUCT_NAME}")
set(PDFT_TASK_FOLDER           "\\${PDFT_PRODUCT_NAME}")
set(PDFT_TASK_PREFIX           "Check ")
set(PDFT_OBJECT_PREFIX         "${PDFT_PRODUCT_NAME}")    # mutexes, window class, messages
set(PDFT_INNO_APPID            "{B4584E00-A586-4C24-996D-6032BFDEDE2F}")   # reserved for M3
set(PDFT_AUMID                 "${PDFT_PRODUCT_NAME}.Settings")            # reserved (toasts)

# ---- 1.x names: fixed forever (migration and clean-up find 1.x through them) ------------
set(PDFT_LEGACY_ROOT_NAME      "PdfThumbnails")   # %LOCALAPPDATA%\Programs\..., Program Files\...
set(PDFT_LEGACY_TASK           "\\PdfThumbnails auto-repair")
set(PDFT_LEGACY_RUNONCE        "PdfThumbnailsCleanup")
set(PDFT_LEGACY_SETTINGS_ENV   "PDFTHUMBNAILS_SETTINGS")

# ---- Detection only (never part of a product name) --------------------------------------
set(PDFT_PDFCRAFT_PRODUCT_NAME "PdfCraft")
set(PDFT_PDFCRAFT_UPGRADE_CODE "{EA00C367-7270-4C50-9D8B-A7A5C88AD27F}")
set(PDFT_PDFCRAFT_SUBSCRIPTION "<QueryList><Query Id=\"0\" Path=\"Application\"><Select Path=\"Application\">*[System[Provider[@Name='MsiInstaller'] and EventID=1033] and EventData[Data='PdfCraft']]</Select></Query></QueryList>")
set(PDFT_THUMB_SLOT_IID        "{e357fccd-a995-4576-b01f-234630154e96}")   # IThumbnailProvider
set(PDFT_SURROGATE_APPID       "{AB8902B4-09CA-4BB6-B78D-A8F59079A8D5}")

# ---- Generated files --------------------------------------------------------------------
set(PDFT_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/templates")

# pdft_guid_initializer(<out-var> <{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}>)
# Turns the registry form of a GUID into a C++ aggregate initializer.
function(pdft_guid_initializer out guid)
  string(REPEAT "[0-9A-Fa-f]" 4 h4)
  string(REPEAT "[0-9A-Fa-f]" 8 h8)
  string(REPEAT "[0-9A-Fa-f]" 12 h12)
  if(NOT guid MATCHES "^{${h8}-${h4}-${h4}-${h4}-${h12}}$")
    message(FATAL_ERROR "Not a {GUID}: '${guid}'")
  endif()
  string(REGEX REPLACE "[{}-]" "" hex "${guid}")
  string(TOLOWER "${hex}" hex)
  string(SUBSTRING "${hex}" 0 8 data1)
  string(SUBSTRING "${hex}" 8 4 data2)
  string(SUBSTRING "${hex}" 12 4 data3)
  set(bytes "")
  foreach(i RANGE 16 30 2)
    string(SUBSTRING "${hex}" ${i} 2 byte)
    list(APPEND bytes "0x${byte}")
  endforeach()
  list(JOIN bytes ", " bytes)
  set(${out} "{0x${data1}, 0x${data2}, 0x${data3}, {${bytes}}}" PARENT_SCOPE)
endfunction()

# pdft_c_string(<out-var> <text>): escapes <text> for a C/C++ string literal.
function(pdft_c_string out text)
  string(REPLACE "\\" "\\\\" text "${text}")
  string(REPLACE "\"" "\\\"" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# pdft_rc_string(<out-var> <text>): escapes <text> for a resource-script string literal.
function(pdft_rc_string out text)
  string(REPLACE "\\" "\\\\" text "${text}")
  string(REPLACE "\"" "\"\"" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# pdft_generate_project_config(): writes <build>/generated/project_config.h and defines the
# INTERFACE target pdft_config that puts it on the include path. Call after project() and
# TargetArch.cmake (the header records PDFT_ARCH).
function(pdft_generate_project_config)
  foreach(name IN ITEMS PRODUCT_NAME DESCRIPTION COPYRIGHT HANDLER_CLSID HANDLER_CLASS_NAME
                        HANDLER_DLL PDFIUM_DLL SETTINGS_EXE CLI_EXE REG_KEY TASK_FOLDER
                        TASK_PREFIX OBJECT_PREFIX INNO_APPID AUMID LEGACY_ROOT_NAME LEGACY_TASK
                        LEGACY_RUNONCE LEGACY_SETTINGS_ENV PDFCRAFT_PRODUCT_NAME
                        PDFCRAFT_UPGRADE_CODE PDFCRAFT_SUBSCRIPTION THUMB_SLOT_IID
                        SURROGATE_APPID)
    pdft_c_string(PDFT_C_${name} "${PDFT_${name}}")
  endforeach()
  pdft_c_string(PDFT_C_PRODUCT_VERSION "${PROJECT_VERSION}${PDFT_VERSION_SUFFIX}")
  foreach(name IN ITEMS HANDLER_CLSID THUMB_SLOT_IID SURROGATE_APPID PDFCRAFT_UPGRADE_CODE)
    pdft_guid_initializer(PDFT_GUID_${name} "${PDFT_${name}}")
  endforeach()
  set(dir "${CMAKE_BINARY_DIR}/generated")
  configure_file("${PDFT_TEMPLATE_DIR}/project_config.h.in" "${dir}/project_config.h" @ONLY)
  add_library(pdft_config INTERFACE)
  target_include_directories(pdft_config INTERFACE "${dir}")
endfunction()

# pdft_add_version_resource(<target> FILETYPE <VFT_DLL|VFT_APP> DESCRIPTION <text>)
# Generates the target's VERSIONINFO from version.rc.in and adds it as a source. Set the
# target's OUTPUT_NAME first: OriginalFilename and InternalName come from it.
function(pdft_add_version_resource target)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "FILETYPE;DESCRIPTION" "")
  if(NOT arg_FILETYPE MATCHES "^VFT_(DLL|APP)$" OR NOT arg_DESCRIPTION)
    message(FATAL_ERROR "pdft_add_version_resource(${target}): FILETYPE VFT_DLL|VFT_APP and DESCRIPTION are required")
  endif()
  get_target_property(base ${target} OUTPUT_NAME)
  if(NOT base)
    set(base "${target}")
  endif()
  get_target_property(type ${target} TYPE)
  if(type STREQUAL "SHARED_LIBRARY")
    set(file_name "${base}${CMAKE_SHARED_LIBRARY_SUFFIX}")
  else()
    set(file_name "${base}${CMAKE_EXECUTABLE_SUFFIX}")
  endif()
  set(PDFT_RC_FILETYPE "${arg_FILETYPE}")
  pdft_rc_string(PDFT_RC_DESCRIPTION "${arg_DESCRIPTION}")
  pdft_rc_string(PDFT_RC_INTERNAL_NAME "${base}")
  pdft_rc_string(PDFT_RC_ORIGINAL_FILENAME "${file_name}")
  pdft_rc_string(PDFT_RC_PRODUCT_NAME "${PDFT_PRODUCT_NAME}")
  pdft_rc_string(PDFT_RC_COPYRIGHT "${PDFT_COPYRIGHT}")
  pdft_rc_string(PDFT_RC_PRODUCT_VERSION "${PROJECT_VERSION}${PDFT_VERSION_SUFFIX}")
  set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}.version.rc")
  configure_file("${PDFT_TEMPLATE_DIR}/version.rc.in" "${rc}" @ONLY)
  target_sources(${target} PRIVATE "${rc}")
endfunction()

# pdft_add_handler_def(<target>): generates handler.def (LIBRARY = PDFT_HANDLER_DLL) and adds
# it to <target>, whose output file name must equal PDFT_HANDLER_DLL.
function(pdft_add_handler_def target)
  set(def "${CMAKE_CURRENT_BINARY_DIR}/handler.def")
  configure_file("${PDFT_TEMPLATE_DIR}/handler.def.in" "${def}" @ONLY)
  target_sources(${target} PRIVATE "${def}")
endfunction()
