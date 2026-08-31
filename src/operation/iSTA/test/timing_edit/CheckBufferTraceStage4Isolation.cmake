cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)
cmake_policy(SET CMP0057 NEW)

# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Licensed under Mulan PSL v2.

foreach(REQUIRED IN ITEMS TEST_ROOT BENCHMARK_ROOT SCHEMA_ROOT BUILD_DIR
                          CONTRACT_ARCHIVE RUNTIME_ARCHIVE NM_EXECUTABLE
                          NINJA_EXECUTABLE BUILD_GENERATOR PROBE_SOURCES_PACKED
                          PROBE_TARGET CONTRACT_CLOSURE_PACKED
                          RUNTIME_CLOSURE_PACKED)
  if(NOT DEFINED ${REQUIRED})
    message(FATAL_ERROR "missing ${REQUIRED}")
  endif()
endforeach()

string(REPLACE "|" ";" CONTRACT_CLOSURE "${CONTRACT_CLOSURE_PACKED}")
string(REPLACE "|" ";" PROBE_SOURCES "${PROBE_SOURCES_PACKED}")
string(REPLACE "|" ";" RUNTIME_CLOSURE "${RUNTIME_CLOSURE_PACKED}")
foreach(FORBIDDEN IN ITEMS ista_interface ista_source idm iZH pybind CUDA)
  if(FORBIDDEN IN_LIST CONTRACT_CLOSURE)
    message(FATAL_ERROR
        "pure Stage 4 contract closure contains forbidden target ${FORBIDDEN}")
  endif()
endforeach()
foreach(FORBIDDEN IN ITEMS
        ProductionFreshSessionLoader
        StaTimingEditSessionProbeAccess
        StaTimingEditCsrReceiptProbe
        CsrNativeTimingOracleReader
        CsrTimingResultFromNativeOracle
        idm
        iZH
        pybind
        CUDA)
  string(JOIN "|" RUNTIME_CLOSURE_TEXT ${RUNTIME_CLOSURE})
  string(FIND "${RUNTIME_CLOSURE_TEXT}" "${FORBIDDEN}" OFFSET)
  if(NOT OFFSET EQUAL -1)
    message(FATAL_ERROR
        "Stage 4 runtime closure contains forbidden dependency ${FORBIDDEN}")
  endif()
endforeach()

set(CONTRACT_SOURCES
    BufferTraceCanonicalEncoding.cpp
    BufferTraceValidationRules.cpp
    BufferTraceJson.cpp
    GreedyBufferCandidateSelector.cpp)
foreach(SOURCE IN LISTS CONTRACT_SOURCES)
  file(READ "${BENCHMARK_ROOT}/${SOURCE}" CONTENT)
  foreach(FORBIDDEN IN ITEMS Database.hpp STAInterface DataManager idm.h
                             iZH pybind cuda ProductionFreshSessionLoader)
    string(FIND "${CONTENT}" "${FORBIDDEN}" OFFSET)
    if(NOT OFFSET EQUAL -1)
      message(FATAL_ERROR
          "pure contract source ${SOURCE} references forbidden ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

set(RUNTIME_SOURCES
    BufferFixedTraceGenerator.cpp
    BufferReplayContext.cpp
    BufferTraceReplayer.cpp
    FreshBaselineRunner.cpp
    NativeBufferObservationExtractor.cpp)
foreach(SOURCE IN LISTS RUNTIME_SOURCES)
  file(READ "${BENCHMARK_ROOT}/${SOURCE}" CONTENT)
  foreach(FORBIDDEN IN ITEMS
          ProductionFreshSessionLoader
          StaTimingEditSessionProbeAccess
          StaTimingEditCsrReceiptProbe
          CsrNativeTimingOracleReader
          CsrNativeTimingOracleNodeReader
          CsrNativeTimingOracleArcReader
          CsrTimingResultFromNativeOracle
          DataManager.hpp
          idm.h
          iZH
          pybind
          cuda
          izh_fanout_fixer)
    string(FIND "${CONTENT}" "${FORBIDDEN}" OFFSET)
    if(NOT OFFSET EQUAL -1)
      message(FATAL_ERROR
          "runtime source ${SOURCE} references forbidden ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

file(GLOB PRODUCTION_FRESH_SOURCES
    "${TEST_ROOT}/timing_edit/BufferTraceProductionFresh*.cpp")
set(LOADER_CALLS 0)
foreach(SOURCE IN LISTS PRODUCTION_FRESH_SOURCES)
  file(READ "${SOURCE}" CONTENT)
  string(REGEX MATCHALL "ProductionFreshSessionLoader::run\\(" CALLS "${CONTENT}")
  list(LENGTH CALLS CALL_COUNT)
  math(EXPR LOADER_CALLS "${LOADER_CALLS} + ${CALL_COUNT}")
  if(CALL_COUNT GREATER 0
     AND NOT SOURCE MATCHES "BufferTraceProductionFreshAdapter\\.cpp$")
    message(FATAL_ERROR
        "production fresh loader bypass exists in ${SOURCE}")
  endif()
  foreach(FORBIDDEN IN ITEMS create_instance create_net connect_pin
                             disconnect_pin remove_instance remove_net
                             save_def write_def izh_fanout_fixer fix_fanout)
    string(FIND "${CONTENT}" "${FORBIDDEN}" OFFSET)
    if(NOT OFFSET EQUAL -1)
      message(FATAL_ERROR
          "production probe source ${SOURCE} references mutation ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()
if(NOT LOADER_CALLS EQUAL 1)
  message(FATAL_ERROR
      "expected exactly one ProductionFreshSessionLoader::run call, got ${LOADER_CALLS}")
endif()

function(check_exact_directory DIRECTORY)
  set(EXPECTED ${ARGN})
  file(GLOB ENTRIES RELATIVE "${DIRECTORY}" "${DIRECTORY}/*")
  foreach(ENTRY IN LISTS ENTRIES)
    if(IS_DIRECTORY "${DIRECTORY}/${ENTRY}"
       OR IS_SYMLINK "${DIRECTORY}/${ENTRY}")
      message(FATAL_ERROR
          "fixture directory contains non-regular entry: ${DIRECTORY}/${ENTRY}")
    endif()
  endforeach()
  list(SORT ENTRIES)
  list(SORT EXPECTED)
  if(NOT "${ENTRIES}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
        "fixture directory inventory differs: ${DIRECTORY}: ${ENTRIES}")
  endif()
endfunction()

check_exact_directory(
    "${TEST_ROOT}/timing_edit/fixtures/buffer_fanout"
    cell.lef design.sdc design.v stage4.lib tech.lef)
check_exact_directory(
    "${TEST_ROOT}/timing_edit/fixtures/buffer_two_batch"
    design.sdc design.v)
check_exact_directory(
    "${TEST_ROOT}/timing_edit/fixtures/buffer_no_gain"
    design.sdc design.v)
check_exact_directory(
    "${TEST_ROOT}/timing_edit/fixtures/buffer_terminal_empty"
    design.sdc design.v)

file(GLOB_RECURSE BACKUPS "${TEST_ROOT}/*.orig")
if(BACKUPS)
  message(FATAL_ERROR "source-tree backup artifacts remain: ${BACKUPS}")
endif()

file(GLOB_RECURSE GENERATED_ENTRIES LIST_DIRECTORIES true
    "${TEST_ROOT}/*"
    "${SCHEMA_ROOT}/*")
set(GENERATED_PY)
foreach(ENTRY IN LISTS GENERATED_ENTRIES)
  if(ENTRY MATCHES "/__pycache__(/|$)" OR ENTRY MATCHES "[.]pyc$")
    list(APPEND GENERATED_PY "${ENTRY}")
  endif()
endforeach()
if(GENERATED_PY)
  message(FATAL_ERROR
      "generated Python bytecode/cache remains in the source tree: ${GENERATED_PY}")
endif()

set(STAGE4_FILES)
list(APPEND STAGE4_FILES
    "${SCHEMA_ROOT}/validate_buffer_fixed_trace.py"
    "${SCHEMA_ROOT}/buffer_trace_profile_contract.py")
foreach(ROOT IN ITEMS "${BENCHMARK_ROOT}" "${TEST_ROOT}/timing_edit")
  file(GLOB CANDIDATES
      "${ROOT}/BufferTrace*.cpp"
      "${ROOT}/BufferTrace*.hpp"
      "${ROOT}/BufferFixedTraceGenerator.cpp"
      "${ROOT}/BufferFixedTraceGenerator.hpp"
      "${ROOT}/FreshBaselineRunner.cpp"
      "${ROOT}/FreshBaselineRunner.hpp"
      "${ROOT}/GreedyBufferCandidateSelector.cpp"
      "${ROOT}/GreedyBufferCandidateSelector.hpp"
      "${ROOT}/NativeBufferObservationExtractor.cpp"
      "${ROOT}/NativeBufferObservationExtractor.hpp"
      "${ROOT}/buffer_trace_*.py"
      "${ROOT}/validate_buffer_trace_manifest.py")
  list(APPEND STAGE4_FILES ${CANDIDATES})
endforeach()
foreach(FILE IN LISTS STAGE4_FILES)
  file(READ "${FILE}" PHYSICAL_CONTENT)
  if(PHYSICAL_CONTENT STREQUAL "")
    set(LINE_COUNT 0)
  else()
    string(REGEX MATCHALL "\n" LINE_BREAKS "${PHYSICAL_CONTENT}")
    list(LENGTH LINE_BREAKS LINE_COUNT)
    string(REGEX MATCH "\n$" HAS_TERMINAL_NEWLINE "${PHYSICAL_CONTENT}")
    if(NOT HAS_TERMINAL_NEWLINE)
      math(EXPR LINE_COUNT "${LINE_COUNT} + 1")
    endif()
  endif()
  if(LINE_COUNT GREATER_EQUAL 700)
    message(FATAL_ERROR "${FILE} exceeds the 700-line Stage 4 source limit")
  endif()
endforeach()

foreach(ARCHIVE IN ITEMS "${CONTRACT_ARCHIVE}" "${RUNTIME_ARCHIVE}")
  if(NOT EXISTS "${ARCHIVE}")
    message(FATAL_ERROR "Stage 4 archive is absent: ${ARCHIVE}")
  endif()
endforeach()
if(NOT BUILD_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR
      "Stage 4 object/depfile isolation requires Ninja: ${BUILD_GENERATOR}")
endif()
set(BUILD_NINJA "${BUILD_DIR}/build.ninja")
if(NOT EXISTS "${BUILD_NINJA}")
  message(FATAL_ERROR "Stage 4 build.ninja is absent: ${BUILD_NINJA}")
endif()
file(READ "${BUILD_NINJA}" BUILD_CONTENT)
execute_process(
    COMMAND "${NINJA_EXECUTABLE}" -C "${BUILD_DIR}" -t targets all
    RESULT_VARIABLE TARGETS_RESULT
    OUTPUT_VARIABLE TARGETS_OUTPUT
    ERROR_VARIABLE TARGETS_ERROR)
if(NOT TARGETS_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect Stage 4 Ninja targets: ${TARGETS_ERROR}")
endif()
string(REPLACE "\n" ";" TARGET_LINES "${TARGETS_OUTPUT}")
set(PROBE_PREFIX
    "src/operation/iSTA/test/CMakeFiles/${PROBE_TARGET}.dir/")
set(EXPECTED_PROBE_OBJECTS)
set(LOADER_OBJECT_REFERENCES 0)
foreach(SOURCE IN LISTS PROBE_SOURCES)
  if(NOT SOURCE MATCHES "[.]cpp$")
    continue()
  endif()
  set(OBJECT "${PROBE_PREFIX}${SOURCE}.o")
  set(ABSOLUTE_SOURCE "${TEST_ROOT}/${SOURCE}")
  set(ABSOLUTE_OBJECT "${BUILD_DIR}/${OBJECT}")
  if(NOT EXISTS "${ABSOLUTE_SOURCE}" OR NOT EXISTS "${ABSOLUTE_OBJECT}")
    message(FATAL_ERROR
        "Stage 4 source/object mapping is absent: ${SOURCE} -> ${OBJECT}")
  endif()
  list(APPEND EXPECTED_PROBE_OBJECTS "${OBJECT}")
  string(FIND "${BUILD_CONTENT}" "build ${OBJECT}:" RULE_POSITION)
  string(FIND "${BUILD_CONTENT}" " ${ABSOLUTE_SOURCE} ||" SOURCE_POSITION)
  string(FIND "${BUILD_CONTENT}" "DEP_FILE = ${OBJECT}.d" DEPFILE_POSITION)
  if(RULE_POSITION EQUAL -1 OR SOURCE_POSITION EQUAL -1
     OR DEPFILE_POSITION EQUAL -1)
    message(FATAL_ERROR
        "Stage 4 probe Ninja source/depfile mapping differs: ${OBJECT}")
  endif()
  execute_process(
      COMMAND "${NINJA_EXECUTABLE}" -C "${BUILD_DIR}" -t deps "${OBJECT}"
      RESULT_VARIABLE DEPS_RESULT
      OUTPUT_VARIABLE DEPS
      ERROR_VARIABLE DEPS_ERROR)
  if(NOT DEPS_RESULT EQUAL 0 OR NOT DEPS MATCHES "[(]VALID[)]"
     OR NOT DEPS MATCHES "${ABSOLUTE_SOURCE}")
    message(FATAL_ERROR
        "Stage 4 probe depfile is invalid: ${OBJECT}: ${DEPS_ERROR}")
  endif()
  execute_process(
      COMMAND "${NM_EXECUTABLE}" -uC "${ABSOLUTE_OBJECT}"
      RESULT_VARIABLE OBJECT_NM_RESULT
      OUTPUT_VARIABLE OBJECT_SYMBOLS
      ERROR_VARIABLE OBJECT_NM_ERROR)
  if(NOT OBJECT_NM_RESULT EQUAL 0)
    message(FATAL_ERROR
        "nm failed for Stage 4 probe object ${OBJECT}: ${OBJECT_NM_ERROR}")
  endif()
  string(REGEX MATCHALL "ProductionFreshSessionLoader::run"
      OBJECT_LOADER_REFS "${OBJECT_SYMBOLS}")
  list(LENGTH OBJECT_LOADER_REFS OBJECT_LOADER_COUNT)
  math(EXPR LOADER_OBJECT_REFERENCES
      "${LOADER_OBJECT_REFERENCES} + ${OBJECT_LOADER_COUNT}")
  if(OBJECT_LOADER_COUNT GREATER 0
     AND NOT SOURCE MATCHES "BufferTraceProductionFreshAdapter[.]cpp$")
    message(FATAL_ERROR
        "probe object bypasses the unique loader adapter: ${OBJECT}")
  endif()
  foreach(FORBIDDEN_REGEX IN ITEMS
          "Idb[^\n]*::(add_|create|connect|disconnect|remove|save|write)"
          "EngineDB[^\n]*::(add_|create|connect|disconnect|remove|save|write)"
          "izh_fanout_fixer"
          "fix_fanout")
    if(OBJECT_SYMBOLS MATCHES "${FORBIDDEN_REGEX}")
      message(FATAL_ERROR
          "probe object contains forbidden mutation symbol: ${OBJECT}: ${FORBIDDEN_REGEX}")
    endif()
  endforeach()
endforeach()
if(NOT LOADER_OBJECT_REFERENCES EQUAL 1)
  message(FATAL_ERROR
      "probe objects must contain one loader reference, got ${LOADER_OBJECT_REFERENCES}")
endif()
set(ACTUAL_PROBE_OBJECTS)
foreach(TARGET_LINE IN LISTS TARGET_LINES)
  if(TARGET_LINE MATCHES "^(${PROBE_PREFIX}[^:]+[.]cpp[.]o):")
    list(APPEND ACTUAL_PROBE_OBJECTS "${CMAKE_MATCH_1}")
  endif()
endforeach()
list(SORT ACTUAL_PROBE_OBJECTS)
list(SORT EXPECTED_PROBE_OBJECTS)
if(NOT "${ACTUAL_PROBE_OBJECTS}" STREQUAL "${EXPECTED_PROBE_OBJECTS}")
  message(FATAL_ERROR
      "Stage 4 probe object inventory differs: actual=[${ACTUAL_PROBE_OBJECTS}] expected=[${EXPECTED_PROBE_OBJECTS}]")
endif()

foreach(ARCHIVE IN ITEMS "${CONTRACT_ARCHIVE}" "${RUNTIME_ARCHIVE}")
  execute_process(
      COMMAND "${NM_EXECUTABLE}" -C "${ARCHIVE}"
      RESULT_VARIABLE NM_RESULT
      OUTPUT_VARIABLE SYMBOLS
      ERROR_VARIABLE NM_ERROR)
  if(NOT NM_RESULT EQUAL 0)
    message(FATAL_ERROR "nm failed for ${ARCHIVE}: ${NM_ERROR}")
  endif()
  if(ARCHIVE STREQUAL "${CONTRACT_ARCHIVE}")
    set(FORBIDDEN_SYMBOLS Database STAInterface DataManager Idb iZH pybind cuda)
  else()
    set(FORBIDDEN_SYMBOLS
        ProductionFreshSessionLoader
        StaTimingEditSessionProbeAccess
        StaTimingEditCsrReceiptProbe
        CsrNativeTimingOracleReader
        CsrTimingResultFromNativeOracle
        izh_fanout_fixer)
  endif()
  foreach(FORBIDDEN IN LISTS FORBIDDEN_SYMBOLS)
    string(FIND "${SYMBOLS}" "${FORBIDDEN}" OFFSET)
    if(NOT OFFSET EQUAL -1)
      message(FATAL_ERROR
          "${ARCHIVE} contains forbidden symbol ${FORBIDDEN}")
    endif()
  endforeach()
endforeach()

message(STATUS
    "Stage 4 source, fixture, loader, closure, line, and symbol inventories passed")
