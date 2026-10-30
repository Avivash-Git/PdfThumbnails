# unit.samples: the preview samples committed in src/app/res are exactly what the generator
# writes today (D30: generated, then committed so the app needs no generator at build time).
#   cmake -DTHUMBTEST=<thumbtest.exe> -DCOMMITTED_DIR=<src/app/res> -DWORK_DIR=<dir>
#         -P tests/CheckSamples.cmake
# After a deliberate change to the samples: thumbtest makesamples src/app/res

cmake_minimum_required(VERSION 3.31)
foreach(var IN ITEMS THUMBTEST COMMITTED_DIR WORK_DIR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "${CMAKE_SCRIPT_MODE_FILE}: -D${var}=... is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}")
execute_process(COMMAND "${THUMBTEST}" makesamples "${WORK_DIR}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "thumbtest makesamples failed (${rc})")
endif()

set(problems "")
foreach(name IN ITEMS sample-document.pdf sample-slide.pdf)
  if(NOT EXISTS "${COMMITTED_DIR}/${name}")
    list(APPEND problems "${COMMITTED_DIR}/${name} is missing")
    continue()
  endif()
  file(SHA256 "${WORK_DIR}/${name}" generated)
  file(SHA256 "${COMMITTED_DIR}/${name}" committed)
  if(NOT generated STREQUAL committed)
    list(APPEND problems "${name}: the committed file (${committed}) is not what the generator writes (${generated})")
  else()
    message(STATUS "ok: ${name} ${committed}")
  endif()
endforeach()
file(REMOVE_RECURSE "${WORK_DIR}")
if(problems)
  list(JOIN problems "\n  " problems)
  message(FATAL_ERROR "preview samples:\n  ${problems}\n"
                      "Regenerate them with: thumbtest makesamples src/app/res")
endif()
