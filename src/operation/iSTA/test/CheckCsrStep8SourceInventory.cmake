# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)
cmake_policy(SET CMP0011 NEW)

set(STEP8_GATE_FILES
    CheckCsrStep8Targets.cmake
    CheckCsrStep8Isolation.cmake
    CheckCsrStep8Workflow.cmake)
foreach(STEP8_GATE IN LISTS STEP8_GATE_FILES)
  set(STEP8_GATE_PATH "${CMAKE_CURRENT_LIST_DIR}/${STEP8_GATE}")
  if(NOT EXISTS "${STEP8_GATE_PATH}")
    message(FATAL_ERROR "missing Step 8 gate: ${STEP8_GATE_PATH}")
  endif()
  file(STRINGS "${STEP8_GATE_PATH}" STEP8_GATE_LINES)
  list(LENGTH STEP8_GATE_LINES STEP8_GATE_LINE_COUNT)
  if(NOT STEP8_GATE_LINE_COUNT LESS 700)
    message(STATUS
            "Step 8 gate advisory line target reached or exceeded: ${STEP8_GATE}=${STEP8_GATE_LINE_COUNT}")
  endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/CheckCsrStep8Targets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CheckCsrStep8Isolation.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/CheckCsrStep8Workflow.cmake")

message(STATUS "CSR Step 8 source, isolation, and workflow inventory passed")
