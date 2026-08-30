# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

foreach(REQUIRED_VARIABLE
        CSR_SNAPSHOT_ALLOWED_FILES_PACKED
        CSR_SNAPSHOT_ACTUAL_SOURCES_PACKED
        CSR_SNAPSHOT_EXPECTED_SOURCES_PACKED
        CSR_SNAPSHOT_EXPECTED_OBJECTS_PACKED
        CSR_SNAPSHOT_OBJECT_DIR
        CSR_BUILD_DIR
        NINJA_EXECUTABLE
        ISTA_SOURCE_ROOT
        CSR_GENERATOR)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "CSR snapshot compiler-dependency isolation requires a Ninja generator")
endif()
if(NOT EXISTS "${NINJA_EXECUTABLE}")
  message(FATAL_ERROR "Ninja executable is missing: ${NINJA_EXECUTABLE}")
endif()

foreach(PACKED_NAME
        CSR_SNAPSHOT_ALLOWED_FILES
        CSR_SNAPSHOT_ACTUAL_SOURCES
        CSR_SNAPSHOT_EXPECTED_SOURCES
        CSR_SNAPSHOT_EXPECTED_OBJECTS)
  string(REPLACE "|" ";" ${PACKED_NAME} "${${PACKED_NAME}_PACKED}")
endforeach()

function(assert_unique LIST_VARIABLE DESCRIPTION)
  set(SEEN)
  foreach(ITEM IN LISTS ${LIST_VARIABLE})
    list(FIND SEEN "${ITEM}" SEEN_INDEX)
    if(NOT SEEN_INDEX EQUAL -1)
      message(FATAL_ERROR "duplicate ${DESCRIPTION}: ${ITEM}")
    endif()
    list(APPEND SEEN "${ITEM}")
  endforeach()
endfunction()

function(assert_same_inventory ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  assert_unique(${ACTUAL_VARIABLE} "actual ${DESCRIPTION}")
  assert_unique(${EXPECTED_VARIABLE} "expected ${DESCRIPTION}")
  set(ACTUAL_SORTED ${${ACTUAL_VARIABLE}})
  set(EXPECTED_SORTED ${${EXPECTED_VARIABLE}})
  list(SORT ACTUAL_SORTED)
  list(SORT EXPECTED_SORTED)
  if(NOT "${ACTUAL_SORTED}" STREQUAL "${EXPECTED_SORTED}")
    message(
        FATAL_ERROR
        "${DESCRIPTION} inventory mismatch; actual=[${ACTUAL_SORTED}] expected=[${EXPECTED_SORTED}]"
    )
  endif()
endfunction()

assert_same_inventory(
    CSR_SNAPSHOT_ACTUAL_SOURCES CSR_SNAPSHOT_EXPECTED_SOURCES "CSR snapshot source"
)

execute_process(
    COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t targets all
    RESULT_VARIABLE TARGETS_RESULT
    OUTPUT_VARIABLE TARGETS_OUTPUT
    ERROR_VARIABLE TARGETS_ERROR
)
if(NOT TARGETS_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot read Ninja target inventory: ${TARGETS_ERROR}")
endif()
set(CSR_SNAPSHOT_ACTUAL_OBJECTS)
string(REPLACE "\n" ";" TARGET_LINES "${TARGETS_OUTPUT}")
foreach(TARGET_LINE IN LISTS TARGET_LINES)
  if(TARGET_LINE MATCHES "^([^:]+\\.cpp\\.o):")
    set(CANDIDATE "${CMAKE_MATCH_1}")
    string(FIND "${CANDIDATE}" "${CSR_SNAPSHOT_OBJECT_DIR}/" OBJECT_PREFIX)
    if(OBJECT_PREFIX EQUAL 0)
      list(APPEND CSR_SNAPSHOT_ACTUAL_OBJECTS "${CANDIDATE}")
    endif()
  endif()
endforeach()
assert_same_inventory(
    CSR_SNAPSHOT_ACTUAL_OBJECTS CSR_SNAPSHOT_EXPECTED_OBJECTS "CSR snapshot object"
)

list(LENGTH CSR_SNAPSHOT_EXPECTED_SOURCES SOURCE_COUNT)
list(LENGTH CSR_SNAPSHOT_EXPECTED_OBJECTS OBJECT_COUNT)
if(NOT SOURCE_COUNT EQUAL 3 OR NOT OBJECT_COUNT EQUAL 3)
  message(FATAL_ERROR "CSR snapshot inventory must contain exactly three sources and objects")
endif()
if(NOT SOURCE_COUNT EQUAL OBJECT_COUNT)
  message(FATAL_ERROR "CSR snapshot source/object counts differ: ${SOURCE_COUNT} vs ${OBJECT_COUNT}")
endif()

foreach(SOURCE_FILE IN LISTS CSR_SNAPSHOT_EXPECTED_SOURCES)
  if(NOT EXISTS "${SOURCE_FILE}")
    message(FATAL_ERROR "missing CSR snapshot source: ${SOURCE_FILE}")
  endif()
  get_filename_component(SOURCE_NAME "${SOURCE_FILE}" NAME)
  set(MATCH_COUNT 0)
  foreach(OBJECT_FILE IN LISTS CSR_SNAPSHOT_EXPECTED_OBJECTS)
    string(FIND "${OBJECT_FILE}" "/${SOURCE_NAME}.o" OBJECT_SUFFIX REVERSE)
    if(NOT OBJECT_SUFFIX EQUAL -1)
      math(EXPR MATCH_COUNT "${MATCH_COUNT} + 1")
    endif()
  endforeach()
  if(NOT MATCH_COUNT EQUAL 1)
    message(FATAL_ERROR "source must map to exactly one Ninja object: ${SOURCE_FILE}")
  endif()
endforeach()

set(PROHIBITED_DEPENDENCY_FRAGMENTS
    "/data_manager/advance/"
    "/data_manager/adapter/csr/CsrGraph"
    "/data_manager/adapter/csr/NativeGraph"
    "/data_manager/adapter/csr/TimingGraphCsr"
    "/interface/"
    "/binding/"
    "/module/graph_builder/"
    "/module/clock_propagator/"
    "/module/delay_calculator/"
    "/module/timing_propagator/"
    "/module/timing_analyzer/"
    "/backend/"
    "/scatter/"
    "/CsrNativeTimingOracle.hpp"
    "/CsrNativeSemanticKeyTestSupport.hpp"
)

foreach(SNAPSHOT_SOURCE IN LISTS CSR_SNAPSHOT_EXPECTED_SOURCES)
  file(READ "${SNAPSHOT_SOURCE}" SNAPSHOT_CONTENT)
  foreach(TEST_ONLY_HEADER IN ITEMS
          CsrNativeTimingOracle.hpp CsrNativeSemanticKeyTestSupport.hpp)
    string(FIND "${SNAPSHOT_CONTENT}" "${TEST_ONLY_HEADER}" HEADER_POSITION)
    if(NOT HEADER_POSITION EQUAL -1)
      message(FATAL_ERROR
              "test-only CSR header leaked into snapshot source ${SNAPSHOT_SOURCE}: ${TEST_ONLY_HEADER}")
    endif()
  endforeach()
endforeach()

foreach(SNAPSHOT_OBJECT IN LISTS CSR_SNAPSHOT_EXPECTED_OBJECTS)
  execute_process(
      COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${SNAPSHOT_OBJECT}"
      RESULT_VARIABLE DEPS_RESULT
      OUTPUT_VARIABLE DEPS_OUTPUT
      ERROR_VARIABLE DEPS_ERROR
  )
  if(NOT DEPS_RESULT EQUAL 0)
    message(FATAL_ERROR "cannot read compiler dependencies for ${SNAPSHOT_OBJECT}: ${DEPS_ERROR}")
  endif()
  string(FIND "${DEPS_OUTPUT}" "(VALID)" VALID_POSITION)
  if(VALID_POSITION EQUAL -1)
    message(FATAL_ERROR "compiler dependencies are missing or stale for ${SNAPSHOT_OBJECT}")
  endif()
  string(REPLACE "\n" ";" DEPENDENCY_LINES "${DEPS_OUTPUT}")
  foreach(DEPENDENCY_LINE IN LISTS DEPENDENCY_LINES)
    string(STRIP "${DEPENDENCY_LINE}" DEPENDENCY)
    string(FIND "${DEPENDENCY}" "${ISTA_SOURCE_ROOT}/" SOURCE_PREFIX)
    if(SOURCE_PREFIX EQUAL 0)
      foreach(PROHIBITED_FRAGMENT IN LISTS PROHIBITED_DEPENDENCY_FRAGMENTS)
        string(FIND "${DEPENDENCY}" "${PROHIBITED_FRAGMENT}" PROHIBITED_POSITION)
        if(NOT PROHIBITED_POSITION EQUAL -1)
          message(
              FATAL_ERROR
              "CSR snapshot dependency is prohibited in ${SNAPSHOT_OBJECT}: ${DEPENDENCY}"
          )
        endif()
      endforeach()
      list(FIND CSR_SNAPSHOT_ALLOWED_FILES "${DEPENDENCY}" ALLOWED_INDEX)
      if(ALLOWED_INDEX EQUAL -1)
        message(
            FATAL_ERROR
            "CSR snapshot compiler dependency escapes the exact allow-list in ${SNAPSHOT_OBJECT}: ${DEPENDENCY}"
        )
      endif()
    endif()
  endforeach()
endforeach()

message(STATUS "CSR snapshot inventory, target, and compiler-dependency isolation passed")
