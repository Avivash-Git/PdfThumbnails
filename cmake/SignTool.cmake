# Helpers for Sign.cmake and VerifySignatures.cmake (run with `cmake -P`): find SignTool and ask
# it about one file's signature.

include_guard(GLOBAL)

# pdft_find_signtool(<out-var>): $ENV{PDFT_SIGNTOOL} when set, else the x64 signtool.exe of the
# newest Windows SDK (it signs and checks ARM64 files as well).
function(pdft_find_signtool out)
  set(tool "$ENV{PDFT_SIGNTOOL}")
  if(tool STREQUAL "")
    file(GLOB tools "$ENV{SystemDrive}/Program Files (x86)/Windows Kits/10/bin/10.*/x64/signtool.exe")
    list(SORT tools COMPARE NATURAL ORDER DESCENDING)
    list(GET tools 0 tool)
  endif()
  if(NOT tool OR NOT EXISTS "${tool}")
    message(FATAL_ERROR "signtool.exe was not found: install the Windows SDK, or set PDFT_SIGNTOOL "
                        "to its path")
  endif()
  set(${out} "${tool}" PARENT_SCOPE)
endfunction()

# pdft_signature_state(<out-var> <signtool> <file>): "ok" when the file has a signature that
# Windows trusts for code (the Authenticode policy, /pa) and that is timestamped, "untimestamped"
# when it is trusted but has no timestamp, else "none" (no signature, or one Windows does not
# trust). SignTool exits 2 when it only warned, and /tw makes a missing timestamp a warning.
function(pdft_signature_state out signtool file)
  execute_process(COMMAND "${signtool}" verify /pa /tw /q "${file}"
                  RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
  if(rc EQUAL 0)
    set(${out} "ok" PARENT_SCOPE)
  elseif(rc EQUAL 2)
    set(${out} "untimestamped" PARENT_SCOPE)
  else()
    set(${out} "none" PARENT_SCOPE)
  endif()
endfunction()

# pdft_signer_subject(<out-var> <file>): the subject of the certificate that signed the file, or
# "" when it has none.
function(pdft_signer_subject out file)
  file(TO_NATIVE_PATH "${file}" native)
  string(REPLACE "'" "''" quoted "${native}")
  execute_process(
    COMMAND powershell -NoProfile -NonInteractive -Command
            "$s = Get-AuthenticodeSignature -LiteralPath '${quoted}'; if ($s.SignerCertificate) { $s.SignerCertificate.Subject }"
    OUTPUT_VARIABLE subject OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  set(${out} "${subject}" PARENT_SCOPE)
endfunction()

# pdft_pe_files(<out-var> <file or folder>...): the files named, and every .exe and .dll in the
# folders named (with their subfolders), sorted.
function(pdft_pe_files out)
  set(files "")
  foreach(path IN LISTS ARGN)
    file(TO_CMAKE_PATH "${path}" path)
    if(IS_DIRECTORY "${path}")
      file(GLOB_RECURSE found LIST_DIRECTORIES false "${path}/*.exe" "${path}/*.dll")
      list(APPEND files ${found})
    elseif(EXISTS "${path}")
      list(APPEND files "${path}")
    else()
      message(FATAL_ERROR "${path} does not exist")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES files)
  list(SORT files)
  set(${out} "${files}" PARENT_SCOPE)
endfunction()
