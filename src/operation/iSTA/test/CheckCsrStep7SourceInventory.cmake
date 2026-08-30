# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(STEP7_FOCUSED_GATES
    CheckCsrStep7Targets.cmake
    CheckCsrStep7Dependencies.cmake
    CheckCsrStep7GuardPublish.cmake
    CheckCsrStep7Workflow.cmake)

foreach(FOCUSED_GATE IN LISTS STEP7_FOCUSED_GATES)
  set(FOCUSED_PATH "${CMAKE_CURRENT_LIST_DIR}/${FOCUSED_GATE}")
  if(NOT EXISTS "${FOCUSED_PATH}")
    message(FATAL_ERROR "missing Step 7 focused gate: ${FOCUSED_PATH}")
  endif()
  file(STRINGS "${FOCUSED_PATH}" FOCUSED_LINES)
  list(LENGTH FOCUSED_LINES FOCUSED_LINE_COUNT)
  if(FOCUSED_LINE_COUNT GREATER_EQUAL 700)
    message(STATUS
            "Step 7 focused gate advisory line target reached or exceeded: ${FOCUSED_GATE}: ${FOCUSED_LINE_COUNT}")
  endif()
  file(READ "${FOCUSED_PATH}" FOCUSED_CONTENT)
  string(FIND "${FOCUSED_CONTENT}" "add_test(" CTEST_REGISTRATION)
  if(NOT CTEST_REGISTRATION EQUAL -1)
    message(FATAL_ERROR
            "Step 7 focused gate must not register CTest directly: ${FOCUSED_GATE}")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/CheckCsrStep7Targets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CheckCsrStep7Dependencies.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CheckCsrStep7GuardPublish.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CheckCsrStep7Workflow.cmake")

message(STATUS "CSR Step 7 chained structural gate umbrella passed")
