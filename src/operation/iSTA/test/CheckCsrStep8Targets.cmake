# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)

set(STEP8_EXPECTED_ORACLE_SOURCES
    real_differential/StaCsrRealOracleProbe.cpp
    real_differential/CsrRealDifferentialOracleEncoding.cpp
    real_differential/CsrRealDifferentialWire.cpp
    real_differential/CsrRealDifferentialProbeSupport.cpp
    real_differential/CsrRealDifferentialScenario.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrNativeSemanticKeyTestSupport.hpp
    CsrNativeTimingOracleTestSupport.hpp)
set(STEP8_EXPECTED_REFERENCE_SOURCES
    real_differential/StaCsrRealReferenceProbe.cpp
    real_differential/CsrRealDifferentialReferenceEncoding.cpp
    real_differential/CsrRealDifferentialNativeReadback.cpp
    real_differential/CsrRealDifferentialNativeNodeReadback.cpp
    real_differential/CsrRealDifferentialNativeArcReadback.cpp
    real_differential/CsrRealDifferentialWire.cpp
    real_differential/CsrRealDifferentialProbeSupport.cpp
    real_differential/CsrRealDifferentialScenario.cpp
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
    CsrReferencePoisonSupport.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp)
set(STEP8_EXPECTED_WIRE_SOURCES
    real_differential/CsrRealDifferentialWireContractTest.cpp
    real_differential/CsrRealDifferentialWire.cpp)
set(STEP8_EXPECTED_CTEST_ROWS
    "ista_csr_real_differential_wire_contract_test=ista_unit"
    "ista_csr_real_differential_controller_validation=ista_unit"
    "ista_csr_step8_source_inventory=ista_unit"
    "ista_csr_real_differential_fresh_process=ista_smoke")
set(STEP8_EXPECTED_PYTHON_SOURCES
    real_differential/real_differential_model.py
    real_differential/real_differential_wire_contract.py
    real_differential/real_differential_contract.py
    real_differential/real_differential_controller.py
    real_differential/real_differential_controller_selftest.py
    reference_input/reference_input_candidate_wire.py
    reference_input/reference_input_recompute_transfer.py)
set(STEP8_EXPECTED_PYTHON_DEPENDENCIES)
foreach(SOURCE IN LISTS STEP8_EXPECTED_PYTHON_SOURCES)
  list(APPEND STEP8_EXPECTED_PYTHON_DEPENDENCIES
       "${CSR_TEST_ROOT}/${SOURCE}")
endforeach()

function(step8_require_file FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 8 target file: ${FILE_PATH}")
  endif()
endfunction()

function(step8_read OUTPUT FILE_PATH)
  step8_require_file("${FILE_PATH}")
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()

function(step8_require_count CONTENT_VARIABLE TOKEN EXPECTED DESCRIPTION)
  set(REMAINDER "${${CONTENT_VARIABLE}}")
  set(ACTUAL 0)
  string(LENGTH "${TOKEN}" TOKEN_LENGTH)
  while(TRUE)
    string(FIND "${REMAINDER}" "${TOKEN}" POSITION)
    if(POSITION EQUAL -1)
      break()
    endif()
    math(EXPR OFFSET "${POSITION} + ${TOKEN_LENGTH}")
    string(SUBSTRING "${REMAINDER}" ${OFFSET} -1 REMAINDER)
    math(EXPR ACTUAL "${ACTUAL} + 1")
  endwhile()
  if(NOT ACTUAL EQUAL EXPECTED)
    message(FATAL_ERROR
            "Step 8 target detector rejected ${DESCRIPTION}: actual=${ACTUAL} expected=${EXPECTED}")
  endif()
endfunction()

function(step8_assert_ordered ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  set(ACTUAL ${${ACTUAL_VARIABLE}})
  set(EXPECTED ${${EXPECTED_VARIABLE}})
  list(LENGTH ACTUAL ACTUAL_COUNT)
  list(LENGTH EXPECTED EXPECTED_COUNT)
  set(ACTUAL_UNIQUE ${ACTUAL})
  set(EXPECTED_UNIQUE ${EXPECTED})
  list(REMOVE_DUPLICATES ACTUAL_UNIQUE)
  list(REMOVE_DUPLICATES EXPECTED_UNIQUE)
  list(LENGTH ACTUAL_UNIQUE ACTUAL_UNIQUE_COUNT)
  list(LENGTH EXPECTED_UNIQUE EXPECTED_UNIQUE_COUNT)
  if(NOT ACTUAL_COUNT EQUAL ACTUAL_UNIQUE_COUNT
     OR NOT EXPECTED_COUNT EQUAL EXPECTED_UNIQUE_COUNT
     OR NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
            "Step 8 target detector rejected ${DESCRIPTION}: actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()

function(step8_check_detector REFERENCE_VARIABLE OBJECT_COUNT UNIT_COUNT
         PROPERTY_COUNT SMOKE_COUNT ROWS_VARIABLE)
  step8_assert_ordered(${REFERENCE_VARIABLE}
                       STEP8_EXPECTED_REFERENCE_SOURCES
                       "reference source inventory")
  set(ACTUAL_ROWS ${${ROWS_VARIABLE}})
  set(EXPECTED_ROWS ${STEP8_EXPECTED_CTEST_ROWS})
  list(LENGTH ACTUAL_ROWS ACTUAL_ROW_COUNT)
  list(REMOVE_DUPLICATES ACTUAL_ROWS)
  list(LENGTH ACTUAL_ROWS UNIQUE_ROW_COUNT)
  list(SORT ACTUAL_ROWS)
  list(SORT EXPECTED_ROWS)
  if(NOT ACTUAL_ROW_COUNT EQUAL UNIQUE_ROW_COUNT
     OR NOT "${ACTUAL_ROWS}" STREQUAL "${EXPECTED_ROWS}"
     OR NOT OBJECT_COUNT EQUAL 26
     OR NOT UNIT_COUNT EQUAL 87
     OR NOT PROPERTY_COUNT EQUAL 20
     OR NOT SMOKE_COUNT EQUAL 7)
    message(FATAL_ERROR
            "Step 8 target detector rejected counts/catalog: objects=${OBJECT_COUNT} labels=${UNIT_COUNT}/${PROPERTY_COUNT}/${SMOKE_COUNT} rows=[${ACTUAL_ROWS}]")
  endif()
endfunction()

function(step8_check_python_detector SOURCES_VARIABLE PUBLIC_FLAG_COUNT)
  step8_assert_ordered(${SOURCES_VARIABLE} STEP8_EXPECTED_PYTHON_SOURCES
                       "Python command dependencies")
  if(NOT PUBLIC_FLAG_COUNT EQUAL 1)
    message(FATAL_ERROR
            "Step 8 target detector rejected fresh public flag count: ${PUBLIC_FLAG_COUNT}")
  endif()
endfunction()

if(DEFINED CSR_STEP8_PYTHON_TARGET_DETECTOR_MODE)
  foreach(REQUIRED CSR_STEP8_PYTHON_SOURCES_PACKED
                   CSR_STEP8_PUBLIC_FLAG_COUNT)
    if(NOT DEFINED ${REQUIRED})
      message(FATAL_ERROR "${REQUIRED} is required")
    endif()
  endforeach()
  string(REPLACE "|" ";" DETECTOR_PYTHON
         "${CSR_STEP8_PYTHON_SOURCES_PACKED}")
  step8_check_python_detector(DETECTOR_PYTHON
                              "${CSR_STEP8_PUBLIC_FLAG_COUNT}")
  return()
endif()

if(DEFINED CSR_STEP8_TARGET_DETECTOR_MODE)
  foreach(REQUIRED CSR_STEP8_REFERENCE_PACKED CSR_STEP8_OBJECT_COUNT
                   CSR_STEP8_UNIT_COUNT CSR_STEP8_PROPERTY_COUNT
                   CSR_STEP8_SMOKE_COUNT CSR_STEP8_CTEST_ROWS_PACKED)
    if(NOT DEFINED ${REQUIRED})
      message(FATAL_ERROR "${REQUIRED} is required")
    endif()
  endforeach()
  string(REPLACE "|" ";" DETECTOR_REFERENCE
         "${CSR_STEP8_REFERENCE_PACKED}")
  string(REPLACE "|" ";" DETECTOR_ROWS "${CSR_STEP8_CTEST_ROWS_PACKED}")
  step8_check_detector(
      DETECTOR_REFERENCE "${CSR_STEP8_OBJECT_COUNT}" "${CSR_STEP8_UNIT_COUNT}"
      "${CSR_STEP8_PROPERTY_COUNT}" "${CSR_STEP8_SMOKE_COUNT}" DETECTOR_ROWS)
  return()
endif()

function(step8_run_detector EXPECT_SUCCESS REFERENCE_PACKED OBJECT_COUNT
         UNIT_COUNT PROPERTY_COUNT SMOKE_COUNT ROWS_PACKED)
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -DCSR_STEP8_TARGET_DETECTOR_MODE=TRUE
              "-DCSR_STEP8_REFERENCE_PACKED=${REFERENCE_PACKED}"
              "-DCSR_STEP8_OBJECT_COUNT=${OBJECT_COUNT}"
              "-DCSR_STEP8_UNIT_COUNT=${UNIT_COUNT}"
              "-DCSR_STEP8_PROPERTY_COUNT=${PROPERTY_COUNT}"
              "-DCSR_STEP8_SMOKE_COUNT=${SMOKE_COUNT}"
              "-DCSR_STEP8_CTEST_ROWS_PACKED=${ROWS_PACKED}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 8 target safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 8 target detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 8 target poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
  message(STATUS "Step 8 target detector receipt: objects=${OBJECT_COUNT},labels=${UNIT_COUNT}/${PROPERTY_COUNT}/${SMOKE_COUNT},exit=${RESULT}")
endfunction()

function(step8_run_python_detector EXPECT_SUCCESS SOURCES_PACKED FLAG_COUNT)
  execute_process(
      COMMAND "${CMAKE_COMMAND}"
              -DCSR_STEP8_PYTHON_TARGET_DETECTOR_MODE=TRUE
              "-DCSR_STEP8_PYTHON_SOURCES_PACKED=${SOURCES_PACKED}"
              "-DCSR_STEP8_PUBLIC_FLAG_COUNT=${FLAG_COUNT}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 8 Python target safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 8 target detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 8 Python target poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
  message(STATUS "Step 8 Python target detector receipt: public_flag=${FLAG_COUNT},exit=${RESULT}")
endfunction()

string(JOIN "|" STEP8_SAFE_REFERENCE_PACKED
       ${STEP8_EXPECTED_REFERENCE_SOURCES})
string(JOIN "|" STEP8_SAFE_ROWS_PACKED ${STEP8_EXPECTED_CTEST_ROWS})
string(JOIN "|" STEP8_SAFE_PYTHON_PACKED ${STEP8_EXPECTED_PYTHON_SOURCES})
step8_run_python_detector(TRUE "${STEP8_SAFE_PYTHON_PACKED}" 1)
set(STEP8_MISSING_PYTHON ${STEP8_EXPECTED_PYTHON_SOURCES})
list(REMOVE_ITEM STEP8_MISSING_PYTHON
     real_differential/real_differential_wire_contract.py)
string(JOIN "|" STEP8_MISSING_PYTHON_PACKED ${STEP8_MISSING_PYTHON})
step8_run_python_detector(FALSE "${STEP8_MISSING_PYTHON_PACKED}" 1)
set(STEP8_REORDERED_PYTHON ${STEP8_EXPECTED_PYTHON_SOURCES})
list(REVERSE STEP8_REORDERED_PYTHON)
string(JOIN "|" STEP8_REORDERED_PYTHON_PACKED ${STEP8_REORDERED_PYTHON})
step8_run_python_detector(FALSE "${STEP8_REORDERED_PYTHON_PACKED}" 1)
step8_run_python_detector(FALSE "${STEP8_SAFE_PYTHON_PACKED}" 0)
step8_run_detector(TRUE "${STEP8_SAFE_REFERENCE_PACKED}" 26 87 20 7
                   "${STEP8_SAFE_ROWS_PACKED}")
set(STEP8_MISSING_ENCODER ${STEP8_EXPECTED_REFERENCE_SOURCES})
list(REMOVE_ITEM STEP8_MISSING_ENCODER
     CsrReferenceInputIndependentModelEncoding.cpp)
string(JOIN "|" STEP8_MISSING_ENCODER_PACKED ${STEP8_MISSING_ENCODER})
step8_run_detector(FALSE "${STEP8_MISSING_ENCODER_PACKED}" 25 87 20 7
                   "${STEP8_SAFE_ROWS_PACKED}")
step8_run_detector(FALSE "${STEP8_SAFE_REFERENCE_PACKED}" 27 87 20 7
                   "${STEP8_SAFE_ROWS_PACKED}")
step8_run_detector(
    FALSE "${STEP8_SAFE_REFERENCE_PACKED}" 26 87 20 7
    "ista_csr_real_differential_wire_contract_test=ista_unit|ista_csr_real_differential_controller_validation=ista_unit|ista_csr_step8_source_inventory=ista_unit")
step8_run_detector(FALSE "${STEP8_SAFE_REFERENCE_PACKED}" 26 88 20 7
                   "${STEP8_SAFE_ROWS_PACKED}")

foreach(REQUIRED ISTA_SOURCE_ROOT CSR_TEST_ROOT CSR_BUILD_DIR
                 NINJA_EXECUTABLE CXX_COMPILER NM_EXECUTABLE CSR_GENERATOR
                 STEP8_ORACLE_SOURCES_PACKED STEP8_REFERENCE_SOURCES_PACKED
                 STEP8_WIRE_SOURCES_PACKED STEP8_PYTHON_SOURCES_PACKED PYTHON_EXECUTABLE)
  if(NOT DEFINED ${REQUIRED})
    message(FATAL_ERROR "${REQUIRED} is required")
  endif()
endforeach()

string(REPLACE "|" ";" STEP8_ACTUAL_ORACLE
       "${STEP8_ORACLE_SOURCES_PACKED}")
string(REPLACE "|" ";" STEP8_ACTUAL_REFERENCE
       "${STEP8_REFERENCE_SOURCES_PACKED}")
string(REPLACE "|" ";" STEP8_ACTUAL_WIRE "${STEP8_WIRE_SOURCES_PACKED}")
string(REPLACE "|" ";" STEP8_ACTUAL_PYTHON
       "${STEP8_PYTHON_SOURCES_PACKED}")
step8_assert_ordered(STEP8_ACTUAL_PYTHON STEP8_EXPECTED_PYTHON_SOURCES
                     "Python source property")
step8_assert_ordered(STEP8_ACTUAL_ORACLE STEP8_EXPECTED_ORACLE_SOURCES
                     "oracle source property")
step8_assert_ordered(STEP8_ACTUAL_REFERENCE STEP8_EXPECTED_REFERENCE_SOURCES
                     "reference source property")
step8_assert_ordered(STEP8_ACTUAL_WIRE STEP8_EXPECTED_WIRE_SOURCES
                     "wire source property")

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "Step 8 object isolation requires Ninja: ${CSR_GENERATOR}")
endif()
set(STEP8_TEST_CMAKE "${CSR_TEST_ROOT}/CsrRealDifferentialTests.cmake")
step8_read(STEP8_TEST_CMAKE_CONTENT "${STEP8_TEST_CMAKE}")
set(STEP8_EXPECTED_STRICT_BLOCK [=[set(ISTA_STEP8_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)]=])
set(STEP8_EXPECTED_PROBE_LINK_BLOCK [=[set(ISTA_STEP8_PROBE_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_interface
    ista_source
    idm
)]=])
set(STEP8_EXPECTED_WIRE_LINK_BLOCK [=[set(ISTA_STEP8_WIRE_LINKS
    ista_timing_graph_csr_adapter
)]=])
set(STEP8_EXPECTED_INCLUDE_BLOCK [=[target_include_directories(
      "${TARGET_NAME}"
      PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}"
      "${CMAKE_CURRENT_LIST_DIR}/real_differential"
  )]=])
set(STEP8_EXPECTED_ORACLE_CALL [=[ista_add_step8_target(
    ista_csr_real_oracle_probe
    ISTA_STEP8_ORACLE_SOURCES
    ISTA_STEP8_PROBE_LINKS
)]=])
set(STEP8_EXPECTED_REFERENCE_CALL [=[ista_add_step8_target(
    ista_csr_real_reference_probe
    ISTA_STEP8_REFERENCE_SOURCES
    ISTA_STEP8_PROBE_LINKS
)]=])
set(STEP8_EXPECTED_WIRE_CALL [=[ista_add_step8_target(
    ista_csr_real_differential_wire_contract_test
    ISTA_STEP8_WIRE_SOURCES
    ISTA_STEP8_WIRE_LINKS
)]=])
set(STEP8_EXPECTED_PYTHON_BLOCK [=[set(ISTA_STEP8_PYTHON_SOURCES
    real_differential/real_differential_model.py
    real_differential/real_differential_wire_contract.py
    real_differential/real_differential_contract.py
    real_differential/real_differential_controller.py
    real_differential/real_differential_controller_selftest.py
    reference_input/reference_input_candidate_wire.py
    reference_input/reference_input_recompute_transfer.py
)]=])
foreach(BLOCK_VARIABLE
        STEP8_EXPECTED_STRICT_BLOCK
        STEP8_EXPECTED_PROBE_LINK_BLOCK
        STEP8_EXPECTED_WIRE_LINK_BLOCK
        STEP8_EXPECTED_INCLUDE_BLOCK
        STEP8_EXPECTED_ORACLE_CALL
        STEP8_EXPECTED_REFERENCE_CALL
        STEP8_EXPECTED_PYTHON_BLOCK
        STEP8_EXPECTED_WIRE_CALL)
  step8_require_count(STEP8_TEST_CMAKE_CONTENT "${${BLOCK_VARIABLE}}" 1
                      "exact target block ${BLOCK_VARIABLE}")
endforeach()
foreach(EXACT_TOKEN
        "set(ISTA_STEP8_STRICT_OPTIONS"
        "-ffp-contract=off"
        "set(ISTA_STEP8_PROBE_LINKS"
        "ista_timing_database_headers"
        "set(ISTA_STEP8_WIRE_LINKS"
        "target_include_directories("
        "target_link_libraries("
        "target_compile_options(")
  step8_require_count(STEP8_TEST_CMAKE_CONTENT "${EXACT_TOKEN}" 1
                      "CMake token ${EXACT_TOKEN}")
endforeach()
step8_require_count(STEP8_TEST_CMAKE_CONTENT "ista_add_step8_target(" 3
                    "target helper calls")
foreach(FAST_OPTION -ffast-math -Ofast)
  step8_require_count(STEP8_TEST_CMAKE_CONTENT "${FAST_OPTION}" 0
                      "fast-math option ${FAST_OPTION}")
endforeach()

set(BUILD_NINJA "${CSR_BUILD_DIR}/build.ninja")
set(COMPILE_COMMANDS "${CSR_BUILD_DIR}/compile_commands.json")
step8_read(BUILD_CONTENT "${BUILD_NINJA}")
step8_read(COMPILE_CONTENT "${COMPILE_COMMANDS}")
execute_process(COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t targets all
                RESULT_VARIABLE TARGETS_RESULT OUTPUT_VARIABLE TARGETS_OUTPUT
                ERROR_VARIABLE TARGETS_ERROR)
if(NOT TARGETS_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect Step 8 Ninja targets: ${TARGETS_ERROR}")
endif()
string(REPLACE "
" ";" TARGET_LINES "${TARGETS_OUTPUT}")
file(REAL_PATH "${CSR_TEST_ROOT}" CANONICAL_TEST_ROOT)
file(REAL_PATH "${CSR_BUILD_DIR}" CANONICAL_BUILD_ROOT)
if(NOT CANONICAL_TEST_ROOT STREQUAL CSR_TEST_ROOT
   OR NOT CANONICAL_BUILD_ROOT STREQUAL CSR_BUILD_DIR)
  message(FATAL_ERROR "Step 8 source/build roots must be canonical")
endif()

function(step8_check_target TARGET EXPECTED_VARIABLE EXPECTED_CPP_COUNT)
  set(PREFIX "src/operation/iSTA/test/CMakeFiles/${TARGET}.dir/")
  set(EXPECTED_OBJECTS)
  foreach(SOURCE IN LISTS ${EXPECTED_VARIABLE})
    if(NOT SOURCE MATCHES "[.]cpp$")
      continue()
    endif()
    set(OBJECT "${PREFIX}${SOURCE}.o")
    set(ABSOLUTE_SOURCE "${CSR_TEST_ROOT}/${SOURCE}")
    set(ABSOLUTE_OBJECT "${CSR_BUILD_DIR}/${OBJECT}")
    file(REAL_PATH "${ABSOLUTE_SOURCE}" CANONICAL_SOURCE)
    file(REAL_PATH "${ABSOLUTE_OBJECT}" CANONICAL_OBJECT)
    if(NOT CANONICAL_SOURCE STREQUAL ABSOLUTE_SOURCE
       OR NOT CANONICAL_OBJECT STREQUAL ABSOLUTE_OBJECT)
      message(FATAL_ERROR "non-canonical Step 8 source/object: ${SOURCE} -> ${OBJECT}")
    endif()
    list(APPEND EXPECTED_OBJECTS "${OBJECT}")
    string(FIND "${BUILD_CONTENT}" "build ${OBJECT}:" RULE_POSITION)
    string(FIND "${BUILD_CONTENT}" " ${ABSOLUTE_SOURCE} ||" SOURCE_POSITION)
    string(FIND "${BUILD_CONTENT}" "DEP_FILE = ${OBJECT}.d" DEPFILE_POSITION)
    if(RULE_POSITION EQUAL -1 OR SOURCE_POSITION EQUAL -1
       OR DEPFILE_POSITION EQUAL -1)
      message(FATAL_ERROR "Step 8 Ninja source/depfile mapping differs: ${OBJECT}")
    endif()
    step8_require_count(COMPILE_CONTENT "\"output\": \"${OBJECT}\"" 1
                        "compile command object ${OBJECT}")
    execute_process(
        COMMAND "${NINJA_EXECUTABLE}" -C "${CSR_BUILD_DIR}" -t deps "${OBJECT}"
        RESULT_VARIABLE DEPS_RESULT OUTPUT_VARIABLE DEPS ERROR_VARIABLE DEPS_ERROR)
    if(NOT DEPS_RESULT EQUAL 0 OR NOT DEPS MATCHES "[(]VALID[)]"
       OR NOT DEPS MATCHES "${ABSOLUTE_SOURCE}")
      message(FATAL_ERROR "missing valid Step 8 depfile: ${OBJECT}: ${DEPS_ERROR}")
    endif()
  endforeach()
  list(LENGTH EXPECTED_OBJECTS CPP_COUNT)
  if(NOT CPP_COUNT EQUAL EXPECTED_CPP_COUNT)
    message(FATAL_ERROR
            "Step 8 ${TARGET} cpp count differs: ${CPP_COUNT}/${EXPECTED_CPP_COUNT}")
  endif()
  set(ACTUAL_OBJECTS)
  foreach(TARGET_LINE IN LISTS TARGET_LINES)
    if(TARGET_LINE MATCHES "^(${PREFIX}[^:]+[.]cpp[.]o):")
      list(APPEND ACTUAL_OBJECTS "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  set(EXPECTED_SORTED ${EXPECTED_OBJECTS})
  list(SORT ACTUAL_OBJECTS)
  list(SORT EXPECTED_SORTED)
  if(NOT "${ACTUAL_OBJECTS}" STREQUAL "${EXPECTED_SORTED}")
    message(FATAL_ERROR
            "Step 8 ${TARGET} Ninja objects differ: actual=[${ACTUAL_OBJECTS}] expected=[${EXPECTED_SORTED}]")
  endif()
  set(STEP8_ALL_OBJECTS ${STEP8_ALL_OBJECTS} ${EXPECTED_OBJECTS} PARENT_SCOPE)
endfunction()

set(STEP8_ALL_OBJECTS)
step8_check_target(ista_csr_real_oracle_probe
                   STEP8_EXPECTED_ORACLE_SOURCES 8)
step8_check_target(ista_csr_real_reference_probe
                   STEP8_EXPECTED_REFERENCE_SOURCES 16)
step8_check_target(ista_csr_real_differential_wire_contract_test
                   STEP8_EXPECTED_WIRE_SOURCES 2)
list(LENGTH STEP8_ALL_OBJECTS STEP8_OBJECT_COUNT)

if(NOT DEFINED CMAKE_CTEST_COMMAND OR NOT EXISTS "${CMAKE_CTEST_COMMAND}")
  message(FATAL_ERROR "CMAKE_CTEST_COMMAND is required")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CSR_BUILD_DIR}"
                --show-only=json-v1 RESULT_VARIABLE CTEST_RESULT
                OUTPUT_VARIABLE CTEST_JSON ERROR_VARIABLE CTEST_ERROR)
if(NOT CTEST_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect Step 8 CTests: ${CTEST_ERROR}")
endif()

function(step8_test_property JSON INDEX PROPERTY OUTPUT)
  string(JSON PROPERTY_COUNT LENGTH "${JSON}" tests ${INDEX} properties)
  set(VALUES)
  if(PROPERTY_COUNT GREATER 0)
    math(EXPR PROPERTY_LAST "${PROPERTY_COUNT} - 1")
    foreach(PROPERTY_INDEX RANGE 0 ${PROPERTY_LAST})
      string(JSON NAME GET "${JSON}" tests ${INDEX} properties ${PROPERTY_INDEX} name)
      if(NAME STREQUAL PROPERTY)
        string(JSON VALUE_TYPE TYPE "${JSON}" tests ${INDEX} properties ${PROPERTY_INDEX} value)
        if(VALUE_TYPE STREQUAL "ARRAY")
          string(JSON VALUE_COUNT LENGTH "${JSON}" tests ${INDEX} properties ${PROPERTY_INDEX} value)
          if(VALUE_COUNT GREATER 0)
            math(EXPR VALUE_LAST "${VALUE_COUNT} - 1")
            foreach(VALUE_INDEX RANGE 0 ${VALUE_LAST})
              string(JSON VALUE GET "${JSON}" tests ${INDEX} properties ${PROPERTY_INDEX} value ${VALUE_INDEX})
              list(APPEND VALUES "${VALUE}")
            endforeach()
          endif()
        else()
          string(JSON VALUE GET "${JSON}" tests ${INDEX} properties ${PROPERTY_INDEX} value)
          list(APPEND VALUES "${VALUE}")
        endif()
      endif()
    endforeach()
  endif()
  set("${OUTPUT}" "${VALUES}" PARENT_SCOPE)
endfunction()

function(step8_require_command_at COMMAND_VARIABLE INDEX EXPECTED DESCRIPTION)
  list(GET ${COMMAND_VARIABLE} ${INDEX} ACTUAL)
  if(NOT ACTUAL STREQUAL EXPECTED)
    message(FATAL_ERROR
            "Step 8 CTest command ${DESCRIPTION} differs at ${INDEX}: actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()

function(step8_require_executable_at COMMAND_VARIABLE INDEX NAME_REGEX DESCRIPTION)
  list(GET ${COMMAND_VARIABLE} ${INDEX} ACTUAL)
  get_filename_component(ACTUAL_NAME "${ACTUAL}" NAME)
  if(NOT EXISTS "${ACTUAL}" OR NOT ACTUAL_NAME MATCHES "${NAME_REGEX}")
    message(FATAL_ERROR
            "Step 8 CTest executable ${DESCRIPTION} differs at ${INDEX}: ${ACTUAL}")
  endif()
endfunction()

get_filename_component(STEP8_TOOL_ROOT "${ISTA_SOURCE_ROOT}/../../.." ABSOLUTE)
file(REAL_PATH "${STEP8_TOOL_ROOT}" STEP8_TOOL_ROOT)

set(STEP8_EXPECTED_CTEST_NAMES)
foreach(ROW IN LISTS STEP8_EXPECTED_CTEST_ROWS)
  string(REGEX REPLACE "=.*$" "" NAME "${ROW}")
  list(APPEND STEP8_EXPECTED_CTEST_NAMES "${NAME}")
endforeach()
string(JSON CTEST_COUNT LENGTH "${CTEST_JSON}" tests)
math(EXPR CTEST_LAST "${CTEST_COUNT} - 1")
set(STEP8_ACTUAL_CTEST_ROWS)
set(STEP8_UNIT_COUNT 0)
set(STEP8_PROPERTY_COUNT 0)
set(STEP8_SMOKE_COUNT 0)
foreach(INDEX RANGE 0 ${CTEST_LAST})
  string(JSON NAME GET "${CTEST_JSON}" tests ${INDEX} name)
  step8_test_property("${CTEST_JSON}" ${INDEX} LABELS LABELS)
  foreach(LABEL ista_unit ista_property ista_smoke)
    list(FIND LABELS "${LABEL}" LABEL_INDEX)
    if(NOT LABEL_INDEX EQUAL -1)
      if(LABEL STREQUAL ista_unit)
        math(EXPR STEP8_UNIT_COUNT "${STEP8_UNIT_COUNT} + 1")
      elseif(LABEL STREQUAL ista_property)
        math(EXPR STEP8_PROPERTY_COUNT "${STEP8_PROPERTY_COUNT} + 1")
      else()
        math(EXPR STEP8_SMOKE_COUNT "${STEP8_SMOKE_COUNT} + 1")
      endif()
    endif()
  endforeach()
  list(FIND STEP8_EXPECTED_CTEST_NAMES "${NAME}" STEP8_INDEX)
  if(STEP8_INDEX EQUAL -1)
    continue()
  endif()
  list(LENGTH LABELS LABEL_COUNT)
  if(NOT LABEL_COUNT EQUAL 1)
    message(FATAL_ERROR "Step 8 CTest labels differ: ${NAME}=[${LABELS}]")
  endif()
  list(GET LABELS 0 LABEL)
  list(APPEND STEP8_ACTUAL_CTEST_ROWS "${NAME}=${LABEL}")
  string(JSON COMMAND_COUNT LENGTH "${CTEST_JSON}" tests ${INDEX} command)
  set(COMMANDS)
  math(EXPR COMMAND_LAST "${COMMAND_COUNT} - 1")
  foreach(COMMAND_INDEX RANGE 0 ${COMMAND_LAST})
    string(JSON ITEM GET "${CTEST_JSON}" tests ${INDEX} command ${COMMAND_INDEX})
    list(APPEND COMMANDS "${ITEM}")
  endforeach()
  step8_test_property("${CTEST_JSON}" ${INDEX} TIMEOUT TIMEOUT)
  step8_test_property("${CTEST_JSON}" ${INDEX} ENVIRONMENT ENVIRONMENT)
  step8_test_property("${CTEST_JSON}" ${INDEX} REQUIRED_FILES REQUIRED_FILES)
  if(NAME STREQUAL ista_csr_real_differential_wire_contract_test)
    set(EXPECTED_COMMAND_COUNT 1)
    set(EXPECTED_TIMEOUT 60)
    set(EXPECTED_ENVIRONMENT)
    step8_require_command_at(COMMANDS 0
                             "${STEP8_TOOL_ROOT}/bin/${NAME}" wire-executable)
  elseif(NAME STREQUAL ista_csr_real_differential_controller_validation)
    set(EXPECTED_COMMAND_COUNT 3)
    set(EXPECTED_TIMEOUT 60)
    set(EXPECTED_ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
    step8_require_executable_at(COMMANDS 0 "^python3([.][0-9]+)?$" python)
    step8_require_command_at(
        COMMANDS 1
        "${CSR_TEST_ROOT}/real_differential/real_differential_controller.py"
        controller)
    step8_require_command_at(COMMANDS 2 --self-test self-test)
    step8_assert_ordered(REQUIRED_FILES STEP8_EXPECTED_PYTHON_DEPENDENCIES
                         "controller Python dependencies")
  elseif(NAME STREQUAL ista_csr_step8_source_inventory)
    set(EXPECTED_COMMAND_COUNT 15)
    set(EXPECTED_TIMEOUT 120)
    set(EXPECTED_ENVIRONMENT)
    step8_require_command_at(COMMANDS 0 "${CMAKE_COMMAND}" cmake)
    step8_require_command_at(COMMANDS 1
                             "-DISTA_SOURCE_ROOT=${ISTA_SOURCE_ROOT}" ista-root)
    step8_require_command_at(COMMANDS 2
                             "-DCSR_TEST_ROOT=${CSR_TEST_ROOT}" test-root)
    step8_require_command_at(COMMANDS 3
                             "-DCSR_BUILD_DIR=${CSR_BUILD_DIR}" build-root)
    step8_require_command_at(COMMANDS 4
                             "-DNINJA_EXECUTABLE=${NINJA_EXECUTABLE}" ninja)
    step8_require_command_at(COMMANDS 5
                             "-DCXX_COMPILER=${CXX_COMPILER}" compiler)
    step8_require_command_at(COMMANDS 6
                             "-DNM_EXECUTABLE=${NM_EXECUTABLE}" nm)
    step8_require_command_at(COMMANDS 7
                             "-DCSR_GENERATOR=${CSR_GENERATOR}" generator)
    step8_require_command_at(
        COMMANDS 8 "-DSTEP8_ORACLE_SOURCES_PACKED=${STEP8_ORACLE_SOURCES_PACKED}"
        oracle-sources)
    step8_require_command_at(
        COMMANDS 9
        "-DSTEP8_REFERENCE_SOURCES_PACKED=${STEP8_REFERENCE_SOURCES_PACKED}"
        reference-sources)
    step8_require_command_at(
        COMMANDS 10 "-DSTEP8_WIRE_SOURCES_PACKED=${STEP8_WIRE_SOURCES_PACKED}"
        wire-sources)
    step8_require_command_at(
        COMMANDS 11 "-DSTEP8_PYTHON_SOURCES_PACKED=${STEP8_PYTHON_SOURCES_PACKED}"
        python-sources)
    step8_require_command_at(COMMANDS 12
                             "-DPYTHON_EXECUTABLE=${PYTHON_EXECUTABLE}" python)
    step8_require_command_at(COMMANDS 13 -P script-mode)
    step8_require_command_at(
        COMMANDS 14 "${CSR_TEST_ROOT}/CheckCsrStep8SourceInventory.cmake"
        inventory-script)
  else()
    set(EXPECTED_COMMAND_COUNT 17)
    set(EXPECTED_TIMEOUT 2400)
    set(EXPECTED_ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
    step8_require_executable_at(COMMANDS 0 "^python3([.][0-9]+)?$" python)
    set(EXPECTED_FRESH_COMMAND
        ignored-python
        "${CSR_TEST_ROOT}/real_differential/real_differential_controller.py"
        --oracle-probe "${STEP8_TOOL_ROOT}/bin/ista_csr_real_oracle_probe"
        --reference-probe "${STEP8_TOOL_ROOT}/bin/ista_csr_real_reference_probe"
        --manifest "${CSR_TEST_ROOT}/real_differential/manifest.json"
        --baseline-manifest "${CSR_TEST_ROOT}/baseline/manifest.json"
        --run-root
        "${CSR_BUILD_DIR}/src/operation/iSTA/test/csr-real-differential-runs"
        --repeats 3 --timeout-seconds 120 --public-negative-self-test)
    foreach(COMMAND_INDEX RANGE 1 16)
      list(GET EXPECTED_FRESH_COMMAND ${COMMAND_INDEX} EXPECTED_ITEM)
      step8_require_command_at(COMMANDS ${COMMAND_INDEX} "${EXPECTED_ITEM}"
                               fresh-process)
    endforeach()
    step8_assert_ordered(REQUIRED_FILES STEP8_EXPECTED_PYTHON_DEPENDENCIES
                         "fresh Python dependencies")
  endif()
  if(NOT COMMAND_COUNT EQUAL EXPECTED_COMMAND_COUNT
     OR NOT TIMEOUT EQUAL EXPECTED_TIMEOUT
     OR NOT "${ENVIRONMENT}" STREQUAL "${EXPECTED_ENVIRONMENT}")
    message(FATAL_ERROR
            "Step 8 CTest contract differs: ${NAME} command=${COMMAND_COUNT} timeout=${TIMEOUT} env=[${ENVIRONMENT}]")
  endif()
endforeach()

step8_check_detector(
    STEP8_ACTUAL_REFERENCE "${STEP8_OBJECT_COUNT}" "${STEP8_UNIT_COUNT}"
    "${STEP8_PROPERTY_COUNT}" "${STEP8_SMOKE_COUNT}"
    STEP8_ACTUAL_CTEST_ROWS)
message(STATUS "CSR Step 8 exact target, object, and CTest inventory passed")
