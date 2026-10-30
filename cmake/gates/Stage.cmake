# Fixture gate.stage: a fresh `cmake --install` of one component, which the gates then inspect.
#   cmake -DBUILD_DIR=<build> -DSTAGE_DIR=<dir> [-DCONFIG=<config>] [-DCOMPONENT=runtime]
#         -P cmake/gates/Stage.cmake
# The directory is emptied first, so files left from an earlier build can never satisfy a gate.

cmake_minimum_required(VERSION 3.31)
include("${CMAKE_CURRENT_LIST_DIR}/PeTools.cmake")
pdft_require(BUILD_DIR STAGE_DIR)
if(NOT COMPONENT)
  set(COMPONENT runtime)
endif()

file(REMOVE_RECURSE "${STAGE_DIR}")
set(config_args "")
if(CONFIG)
  set(config_args --config "${CONFIG}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${STAGE_DIR}"
          --component "${COMPONENT}" ${config_args}
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  pdft_gate_fail("cmake --install --component ${COMPONENT} failed (${rc})")
endif()
file(GLOB_RECURSE staged RELATIVE "${STAGE_DIR}" "${STAGE_DIR}/*")
list(SORT staged)
list(LENGTH staged count)
pdft_gate_ok("staged ${count} files of component ${COMPONENT} in ${STAGE_DIR}")
foreach(file IN LISTS staged)
  message(STATUS "  ${file}")
endforeach()
