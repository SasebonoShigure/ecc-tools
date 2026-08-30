# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

cmake_policy(SET CMP0007 NEW)
cmake_policy(SET CMP0012 NEW)

set(STEP7_ADAPTER_SOURCES CsrSessionSnapshotAdapterContractTest.cpp)
set(STEP7_CONTEXT_SOURCES StaCsrSessionContextTest.cpp)
set(STEP7_LIFECYCLE_SOURCES StaCsrSessionLifecycleTest.cpp)
set(STEP7_BINDING_SOURCES StaCsrSnapshotBindingContractTest.cpp)
set(STEP7_STRICT_OPTIONS -Wall -Wextra -Werror -pedantic -ffp-contract=off)

set(STEP8_EXPECTED_CTEST_NAMES
    ista_csr_real_differential_wire_contract_test
    ista_csr_real_differential_controller_validation
    ista_csr_step8_source_inventory
    ista_csr_real_differential_fresh_process)
set(STEP8_EXPECTED_CTEST_ROWS
    "ista_csr_real_differential_wire_contract_test=ista_unit"
    "ista_csr_real_differential_controller_validation=ista_unit"
    "ista_csr_step8_source_inventory=ista_unit"
    "ista_csr_real_differential_fresh_process=ista_smoke")

function(check_step7_catalog UNIT_COUNT PROPERTY_COUNT SMOKE_COUNT ROWS_VARIABLE)
  set(ACTUAL_ROWS ${${ROWS_VARIABLE}})
  set(EXPECTED_ROWS ${STEP8_EXPECTED_CTEST_ROWS})
  list(LENGTH ACTUAL_ROWS ACTUAL_COUNT)
  list(REMOVE_DUPLICATES ACTUAL_ROWS)
  list(LENGTH ACTUAL_ROWS UNIQUE_COUNT)
  list(SORT ACTUAL_ROWS)
  list(SORT EXPECTED_ROWS)
  if(NOT ACTUAL_COUNT EQUAL UNIQUE_COUNT OR NOT "${ACTUAL_ROWS}" STREQUAL "${EXPECTED_ROWS}")
    message(FATAL_ERROR "Step 7 catalog detector rejected Step 8 exclusion set: [${ACTUAL_ROWS}]")
  endif()
  math(EXPR BASELINE_UNIT "${UNIT_COUNT} - 3")
  math(EXPR BASELINE_PROPERTY "${PROPERTY_COUNT} - 0")
  math(EXPR BASELINE_SMOKE "${SMOKE_COUNT} - 1")
  if(NOT BASELINE_UNIT EQUAL 84 OR NOT BASELINE_PROPERTY EQUAL 20
     OR NOT BASELINE_SMOKE EQUAL 6)
    message(FATAL_ERROR
            "Step 7 catalog detector rejected restored baseline: unit=${BASELINE_UNIT} property=${BASELINE_PROPERTY} smoke=${BASELINE_SMOKE}")
  endif()
endfunction()

if(DEFINED CSR_STEP7_CATALOG_DETECTOR_MODE)
  foreach(REQUIRED CSR_STEP7_UNIT_COUNT CSR_STEP7_PROPERTY_COUNT
                   CSR_STEP7_SMOKE_COUNT CSR_STEP7_CATALOG_ROWS_PACKED)
    if(NOT DEFINED ${REQUIRED})
      message(FATAL_ERROR "${REQUIRED} is required")
    endif()
  endforeach()
  string(REPLACE "|" ";" STEP8_DETECTOR_ROWS
         "${CSR_STEP7_CATALOG_ROWS_PACKED}")
  check_step7_catalog("${CSR_STEP7_UNIT_COUNT}" "${CSR_STEP7_PROPERTY_COUNT}"
                      "${CSR_STEP7_SMOKE_COUNT}" STEP8_DETECTOR_ROWS)
  return()
endif()

function(require_step7_file FILE_PATH)
  if(NOT EXISTS "${FILE_PATH}")
    message(FATAL_ERROR "missing Step 7 target file: ${FILE_PATH}")
  endif()
endfunction()

function(read_step7_file OUTPUT FILE_PATH)
  require_step7_file("${FILE_PATH}")
  file(READ "${FILE_PATH}" CONTENT)
  set("${OUTPUT}" "${CONTENT}" PARENT_SCOPE)
endfunction()

function(assert_step7_inventory ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  set(ACTUAL ${${ACTUAL_VARIABLE}})
  set(EXPECTED ${${EXPECTED_VARIABLE}})
  list(LENGTH ACTUAL ACTUAL_COUNT)
  list(LENGTH EXPECTED EXPECTED_COUNT)
  list(REMOVE_DUPLICATES ACTUAL)
  list(REMOVE_DUPLICATES EXPECTED)
  list(LENGTH ACTUAL ACTUAL_UNIQUE)
  list(LENGTH EXPECTED EXPECTED_UNIQUE)
  list(SORT ACTUAL)
  list(SORT EXPECTED)
  if(NOT ACTUAL_COUNT EQUAL ACTUAL_UNIQUE
     OR NOT EXPECTED_COUNT EQUAL EXPECTED_UNIQUE
     OR NOT "${ACTUAL}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
            "Step 7 target detector rejected ${DESCRIPTION}: actual=[${ACTUAL}] expected=[${EXPECTED}]")
  endif()
endfunction()

function(unpack_step7_sources PACKED_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
  string(REPLACE "|" ";" ACTUAL "${${PACKED_VARIABLE}}")
  assert_step7_inventory(ACTUAL ${EXPECTED_VARIABLE} "${DESCRIPTION}")
endfunction()

function(check_step7_packed_sources)
  unpack_step7_sources(CSR_ADAPTER_SOURCES_PACKED STEP7_ADAPTER_SOURCES "adapter sources")
  unpack_step7_sources(CSR_CONTEXT_SOURCES_PACKED STEP7_CONTEXT_SOURCES "context sources")
  unpack_step7_sources(CSR_LIFECYCLE_SOURCES_PACKED STEP7_LIFECYCLE_SOURCES "lifecycle sources")
  unpack_step7_sources(CSR_BINDING_SOURCES_PACKED STEP7_BINDING_SOURCES "binding sources")
endfunction()

if(DEFINED CSR_STEP7_TARGET_DETECTOR_MODE)
  check_step7_packed_sources()
  return()
endif()

foreach(REQUIRED_VARIABLE
        ISTA_SOURCE_ROOT CSR_TEST_ROOT CSR_BUILD_DIR NINJA_EXECUTABLE
        CSR_GENERATOR CSR_ADAPTER_SOURCES_PACKED CSR_CONTEXT_SOURCES_PACKED
        CSR_LIFECYCLE_SOURCES_PACKED CSR_BINDING_SOURCES_PACKED)
  if(NOT DEFINED ${REQUIRED_VARIABLE})
    message(FATAL_ERROR "${REQUIRED_VARIABLE} is required")
  endif()
endforeach()

function(run_step7_target_detector EXPECT_SUCCESS ADAPTER CONTEXT LIFECYCLE BINDING)
  execute_process(
      COMMAND "${CMAKE_COMMAND}"
              -DCSR_STEP7_TARGET_DETECTOR_MODE=TRUE
              "-DCSR_ADAPTER_SOURCES_PACKED=${ADAPTER}"
              "-DCSR_CONTEXT_SOURCES_PACKED=${CONTEXT}"
              "-DCSR_LIFECYCLE_SOURCES_PACKED=${LIFECYCLE}"
              "-DCSR_BINDING_SOURCES_PACKED=${BINDING}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 7 target safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 7 target detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 7 target poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()

function(run_step7_catalog_detector EXPECT_SUCCESS UNIT PROPERTY SMOKE ROWS)
  execute_process(
      COMMAND "${CMAKE_COMMAND}" -DCSR_STEP7_CATALOG_DETECTOR_MODE=TRUE
              "-DCSR_STEP7_UNIT_COUNT=${UNIT}"
              "-DCSR_STEP7_PROPERTY_COUNT=${PROPERTY}"
              "-DCSR_STEP7_SMOKE_COUNT=${SMOKE}"
              "-DCSR_STEP7_CATALOG_ROWS_PACKED=${ROWS}"
              -P "${CMAKE_CURRENT_LIST_FILE}"
      RESULT_VARIABLE RESULT OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
  if(EXPECT_SUCCESS)
    if(NOT RESULT EQUAL 0)
      message(FATAL_ERROR "Step 7 catalog safe self-test failed: ${OUT}${ERR}")
    endif()
  else()
    string(FIND "${ERR}" "Step 7 catalog detector rejected" DIAGNOSTIC)
    if(NOT RESULT EQUAL 1 OR DIAGNOSTIC EQUAL -1)
      message(FATAL_ERROR "Step 7 catalog poison self-test failed: ${OUT}${ERR}")
    endif()
  endif()
endfunction()

set(STEP8_SAFE_ROWS
    "ista_csr_real_differential_wire_contract_test=ista_unit|ista_csr_real_differential_controller_validation=ista_unit|ista_csr_step8_source_inventory=ista_unit|ista_csr_real_differential_fresh_process=ista_smoke")
run_step7_catalog_detector(TRUE 87 20 7 "${STEP8_SAFE_ROWS}")
run_step7_catalog_detector(
    FALSE 87 20 7
    "ista_csr_real_differential_wire_contract_test=ista_unit|ista_csr_real_differential_controller_validation=ista_unit|ista_csr_step8_source_inventory=ista_unit")
run_step7_catalog_detector(
    FALSE 87 20 7
    "ista_csr_real_differential_wire_contract_test=ista_smoke|ista_csr_real_differential_controller_validation=ista_unit|ista_csr_step8_source_inventory=ista_unit|ista_csr_real_differential_fresh_process=ista_smoke")
run_step7_catalog_detector(FALSE 87 20 7 "${STEP8_SAFE_ROWS}|ista_csr_step8_extra=ista_unit")
run_step7_catalog_detector(FALSE 88 20 7 "${STEP8_SAFE_ROWS}")

run_step7_target_detector(
    TRUE
    CsrSessionSnapshotAdapterContractTest.cpp
    StaCsrSessionContextTest.cpp
    StaCsrSessionLifecycleTest.cpp
    StaCsrSnapshotBindingContractTest.cpp)
run_step7_target_detector(
    FALSE
    StaCsrSessionContextTest.cpp
    CsrSessionSnapshotAdapterContractTest.cpp
    StaCsrSessionLifecycleTest.cpp
    StaCsrSnapshotBindingContractTest.cpp)
check_step7_packed_sources()

function(extract_step7_block CONTENT_VARIABLE START_TOKEN OUTPUT)
  string(FIND "${${CONTENT_VARIABLE}}" "${START_TOKEN}" START)
  if(START EQUAL -1)
    message(FATAL_ERROR "missing Step 7 CMake block: ${START_TOKEN}")
  endif()
  string(SUBSTRING "${${CONTENT_VARIABLE}}" ${START} -1 TAIL)
  string(REGEX REPLACE "\n[ \t]+\\)" "\n)" TAIL "${TAIL}")
  string(FIND "${TAIL}" "\n)" END)
  if(END EQUAL -1)
    message(FATAL_ERROR "unterminated Step 7 CMake block: ${START_TOKEN}")
  endif()
  math(EXPR LENGTH "${END} + 2")
  string(SUBSTRING "${TAIL}" 0 ${LENGTH} BLOCK)
  set("${OUTPUT}" "${BLOCK}" PARENT_SCOPE)
endfunction()

function(extract_step7_sources CONTENT_VARIABLE START_TOKEN OUTPUT)
  extract_step7_block(${CONTENT_VARIABLE} "${START_TOKEN}" BLOCK)
  string(REGEX MATCHALL "[A-Za-z0-9_./]+\\.(cpp|hpp)" MATCHES "${BLOCK}")
  set(SOURCES)
  foreach(MATCH IN LISTS MATCHES)
    get_filename_component(NAME "${MATCH}" NAME)
    list(APPEND SOURCES "${NAME}")
  endforeach()
  set("${OUTPUT}" "${SOURCES}" PARENT_SCOPE)
endfunction()

function(check_step7_source_block CONTENT_VARIABLE START_TOKEN EXPECTED_VARIABLE DESCRIPTION)
  extract_step7_sources(${CONTENT_VARIABLE} "${START_TOKEN}" ACTUAL)
  assert_step7_inventory(ACTUAL ${EXPECTED_VARIABLE} "${DESCRIPTION}")
endfunction()

function(check_step7_link_tokens CONTENT_VARIABLE TARGET EXPECTED_VARIABLE)
  extract_step7_block(${CONTENT_VARIABLE} "target_link_libraries(${TARGET}" BLOCK)
  string(REGEX MATCHALL
         "(ista_[A-Za-z0-9_]+|py_ista|pybind11::[A-Za-z0-9_]+|Threads::[A-Za-z0-9_]+|tool_manager|tcl_util|ecc_logger|idm|liberty|spef|PUBLIC|PRIVATE|INTERFACE)"
         TOKENS "${BLOCK}")
  set(EXPECTED ${${EXPECTED_VARIABLE}})
  if(NOT "${TOKENS}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR
            "Step 7 direct-link inventory differs for ${TARGET}: actual=[${TOKENS}] expected=[${EXPECTED}]")
  endif()
endfunction()

set(CORE_CMAKE "${ISTA_SOURCE_ROOT}/source/module/timing_csr/CMakeLists.txt")
set(SNAPSHOT_CMAKE "${ISTA_SOURCE_ROOT}/source/module/timing_csr_snapshot/CMakeLists.txt")
set(ADAPTER_CMAKE "${ISTA_SOURCE_ROOT}/source/data_manager/adapter/csr/CMakeLists.txt")
set(INTERFACE_CMAKE "${ISTA_SOURCE_ROOT}/interface/CMakeLists.txt")
get_filename_component(CHILD_SRC_ROOT "${ISTA_SOURCE_ROOT}/../.." ABSOLUTE)
set(PY_CMAKE "${CHILD_SRC_ROOT}/interface/python/py_ista/CMakeLists.txt")
set(TEST_CMAKE "${CSR_TEST_ROOT}/CsrSessionTests.cmake")
set(TOP_TEST_CMAKE "${CSR_TEST_ROOT}/CMakeLists.txt")
read_step7_file(CORE_CONTENT "${CORE_CMAKE}")
read_step7_file(SNAPSHOT_CONTENT "${SNAPSHOT_CMAKE}")
read_step7_file(ADAPTER_CONTENT "${ADAPTER_CMAKE}")
read_step7_file(INTERFACE_CONTENT "${INTERFACE_CMAKE}")
read_step7_file(PY_CONTENT "${PY_CMAKE}")
read_step7_file(TEST_CONTENT "${TEST_CMAKE}")
read_step7_file(TOP_TEST_CONTENT "${TOP_TEST_CMAKE}")

set(EXPECTED_CORE_SOURCES
    TimingSemanticKey.cpp CsrFingerprint.cpp CsrReferenceCanonicalEncoder.cpp
    CsrReferenceFingerprintPayload.cpp CsrReferenceValidator.cpp
    CsrSnapshotIdentityValidator.cpp CsrFifoLevelization.cpp
    CsrReferenceInputValidator.cpp CsrReferenceConstraintValidator.cpp
    CsrReferenceNativeModelValidator.cpp CsrTimingResultValidator.cpp
    CsrTimingResultArcValidator.cpp CsrTableLookup.cpp
    CsrCpuReferenceBackend.cpp CsrCpuReferenceResultInitialization.cpp
    CsrCpuReferenceArcTransfer.cpp CsrCpuReferenceForwardPropagation.cpp
    CsrCpuReferenceRequiredPropagation.cpp)
set(EXPECTED_SNAPSHOT_SOURCES
    CsrSnapshotStorage.cpp CsrSnapshotObservationValidator.cpp
    CsrSnapshotObservationTransition.cpp)
set(EXPECTED_ADAPTER_PRODUCTION_SOURCES
    CsrGraphEncoding.cpp CsrGraphValidator.cpp TimingGraphCsrAdapter.cpp
    TimingGraphCsrArcExport.cpp TimingGraphCsrAdjacency.cpp NativeGraphProjection.cpp
    CsrReferenceInputAdapter.cpp TimingResultScatter.cpp
    TimingResultScatterValidation.cpp TimingResultScatterPreparation.cpp
    TimingResultScatterPublish.cpp CsrConstraintExport.cpp CsrClockSeedExport.cpp
    CsrTimingCheckExport.cpp CsrLibertyExport.cpp CsrLoadExport.cpp
    CsrParasiticExport.cpp CsrSessionSnapshotAdapter.cpp)
set(EXPECTED_INTERFACE_SOURCES
    IncrementalStaSession.cpp StaExecutionContract.cpp StaLifecycleGuard.cpp
    StaProductionLifecycle.cpp StaCsrSessionContext.cpp STAInterface.cpp
    STAInterfaceOperations.cpp)
set(EXPECTED_PY_SOURCES py_ista.cpp py_ista_utils.cpp)
set(EXPECTED_PROJECTION_SOURCES
    StaCsrProjectionProbe.cpp CsrNativeSemanticKeyTestSupport.hpp
    CsrNativeGraphRawProjector.cpp CsrNativeGraphOracleReader.cpp
    CsrNativeGraphIndependentEncoding.cpp)

check_step7_source_block(CORE_CONTENT "add_library(ista_timing_csr_core"
                         EXPECTED_CORE_SOURCES "core exact 18 sources")
check_step7_source_block(SNAPSHOT_CONTENT "add_library(ista_timing_csr_snapshot_contract"
                         EXPECTED_SNAPSHOT_SOURCES "snapshot exact 3 sources")
check_step7_source_block(ADAPTER_CONTENT "set(ISTA_TIMING_GRAPH_CSR_ADAPTER_SOURCES"
                         EXPECTED_ADAPTER_PRODUCTION_SOURCES "adapter exact 18 sources")
check_step7_source_block(INTERFACE_CONTENT "add_library(ista_interface"
                         EXPECTED_INTERFACE_SOURCES "interface exact 7 sources")
check_step7_source_block(PY_CONTENT "add_library(py_ista"
                         EXPECTED_PY_SOURCES "py_ista exact 2 sources")
check_step7_source_block(TOP_TEST_CONTENT "add_executable(ista_csr_projection_probe"
                         EXPECTED_PROJECTION_SOURCES "projection probe exact 5 sources")

string(FIND "${CORE_CONTENT}" "target_link_libraries(ista_timing_csr_core" CORE_LINK)
if(NOT CORE_LINK EQUAL -1)
  message(FATAL_ERROR "ista_timing_csr_core direct links must remain empty")
endif()
set(EXPECTED_SNAPSHOT_LINKS
    ista_timing_csr_snapshot_contract PUBLIC ista_timing_csr_core)
set(EXPECTED_ADAPTER_LINKS
    ista_timing_graph_csr_adapter PUBLIC ista_timing_csr_core
    ista_timing_csr_snapshot_contract PRIVATE ista_timing_database_headers)
set(EXPECTED_INTERFACE_LINKS
    ista_interface PRIVATE ista_source ista_timing_graph_csr_adapter idm liberty spef)
set(EXPECTED_PY_LINKS
    py_ista PUBLIC tool_manager ista_interface tcl_util pybind11::pybind11 ecc_logger)
set(EXPECTED_PROJECTION_LINKS
    ista_csr_projection_probe PRIVATE ista_timing_graph_csr_adapter
    ista_interface ista_source idm)
check_step7_link_tokens(SNAPSHOT_CONTENT ista_timing_csr_snapshot_contract EXPECTED_SNAPSHOT_LINKS)
check_step7_link_tokens(ADAPTER_CONTENT ista_timing_graph_csr_adapter EXPECTED_ADAPTER_LINKS)
check_step7_link_tokens(INTERFACE_CONTENT ista_interface EXPECTED_INTERFACE_LINKS)
check_step7_link_tokens(PY_CONTENT py_ista EXPECTED_PY_LINKS)
check_step7_link_tokens(TOP_TEST_CONTENT ista_csr_projection_probe EXPECTED_PROJECTION_LINKS)
set(EXPECTED_STEP7_ADAPTER_TEST_LINKS
    ista_timing_graph_csr_adapter ista_timing_database_headers)
set(EXPECTED_STEP7_CONTEXT_TEST_LINKS
    ista_interface ista_timing_graph_csr_adapter ista_timing_database_headers)
set(EXPECTED_STEP7_LIFECYCLE_TEST_LINKS
    ista_interface ista_timing_graph_csr_adapter ista_timing_database_headers
    Threads::Threads)
set(EXPECTED_STEP7_BINDING_TEST_LINKS py_ista pybind11::embed)

function(check_step7_named_link_set SET_NAME EXPECTED_VARIABLE)
  extract_step7_block(TEST_CONTENT "set(${SET_NAME}" LINK_SET_BLOCK)
  string(REGEX MATCHALL
         "(ista_[A-Za-z0-9_]+|py_ista|pybind11::[A-Za-z0-9_]+|Threads::Threads)"
         ACTUAL_LINKS "${LINK_SET_BLOCK}")
  set(EXPECTED_LINKS ${${EXPECTED_VARIABLE}})
  if(NOT "${ACTUAL_LINKS}" STREQUAL "${EXPECTED_LINKS}")
    message(FATAL_ERROR
            "Step 7 test link set differs for ${SET_NAME}: actual=[${ACTUAL_LINKS}] expected=[${EXPECTED_LINKS}]")
  endif()
endfunction()

function(check_step7_unit_call TARGET SOURCE LINK_VARIABLE TIMEOUT)
  string(REGEX MATCH
         "ista_add_step7_target\\([ \t\r\n]+${TARGET}[^)]*\\)"
         CALL_BLOCK "${TEST_CONTENT}")
  if("${CALL_BLOCK}" STREQUAL "")
    message(FATAL_ERROR "missing Step 7 unit target call: ${TARGET}")
  endif()
  string(REGEX REPLACE "[ \t\r\n]+" ";" NORMALIZED_CALL "${CALL_BLOCK}")
  set(EXPECTED_CALL
      "ista_add_step7_target(;${TARGET};${SOURCE};${LINK_VARIABLE};${TIMEOUT};)")
  if(NOT "${NORMALIZED_CALL}" STREQUAL "${EXPECTED_CALL}")
    message(FATAL_ERROR
            "Step 7 unit target call differs for ${TARGET}: [${NORMALIZED_CALL}]")
  endif()
endfunction()

check_step7_named_link_set(
    ISTA_STEP7_ADAPTER_LINKS EXPECTED_STEP7_ADAPTER_TEST_LINKS)
check_step7_named_link_set(
    ISTA_STEP7_CONTEXT_LINKS EXPECTED_STEP7_CONTEXT_TEST_LINKS)
check_step7_named_link_set(
    ISTA_STEP7_LIFECYCLE_LINKS EXPECTED_STEP7_LIFECYCLE_TEST_LINKS)
check_step7_named_link_set(
    ISTA_STEP7_BINDING_LINKS EXPECTED_STEP7_BINDING_TEST_LINKS)
check_step7_unit_call(
    ista_csr_session_snapshot_adapter_contract_test
    CsrSessionSnapshotAdapterContractTest.cpp ISTA_STEP7_ADAPTER_LINKS 60)
check_step7_unit_call(
    ista_csr_session_context_test
    StaCsrSessionContextTest.cpp ISTA_STEP7_CONTEXT_LINKS 60)
check_step7_unit_call(
    ista_csr_session_lifecycle_test
    StaCsrSessionLifecycleTest.cpp ISTA_STEP7_LIFECYCLE_LINKS 120)
check_step7_unit_call(
    ista_csr_snapshot_binding_contract_test
    StaCsrSnapshotBindingContractTest.cpp ISTA_STEP7_BINDING_LINKS 120)

extract_step7_block(
    TEST_CONTENT
    "target_include_directories(\n    ista_csr_snapshot_binding_contract_test"
    BINDING_INCLUDE_BLOCK)
string(REPLACE "\"" "" BINDING_INCLUDE_BLOCK "${BINDING_INCLUDE_BLOCK}")
string(REGEX REPLACE
       "[ \t\r\n]+" ";" BINDING_INCLUDE_NORMALIZED "${BINDING_INCLUDE_BLOCK}")
set(EXPECTED_BINDING_INCLUDE_NORMALIZED
    [=[target_include_directories(;ista_csr_snapshot_binding_contract_test;PRIVATE;${HOME_ISTA}/source/data_manager/adapter/csr;${HOME_ISTA}/source/module/timing_csr_snapshot;)]=])
if(NOT "${BINDING_INCLUDE_NORMALIZED}"
       STREQUAL "${EXPECTED_BINDING_INCLUDE_NORMALIZED}")
  message(FATAL_ERROR
          "Step 7 binding PRIVATE include inventory differs: [${BINDING_INCLUDE_NORMALIZED}]")
endif()

extract_step7_block(TEST_CONTENT "set(ISTA_STEP7_STRICT_OPTIONS" OPTIONS_BLOCK)
string(REGEX MATCHALL "-[A-Za-z0-9=-]+" ACTUAL_OPTIONS "${OPTIONS_BLOCK}")
if(NOT "${ACTUAL_OPTIONS}" STREQUAL "${STEP7_STRICT_OPTIONS}")
  message(FATAL_ERROR "Step 7 exact compile options differ: [${ACTUAL_OPTIONS}]")
endif()
foreach(TARGET
        ista_csr_session_snapshot_adapter_contract_test
        ista_csr_session_context_test
        ista_csr_session_lifecycle_test
        ista_csr_snapshot_binding_contract_test)
  string(REGEX MATCHALL "${TARGET}" TARGET_MATCHES "${TEST_CONTENT}")
  list(LENGTH TARGET_MATCHES TARGET_COUNT)
  set(EXPECTED_TARGET_COUNT 1)
  if("${TARGET}" STREQUAL ista_csr_snapshot_binding_contract_test)
    set(EXPECTED_TARGET_COUNT 2)
  endif()
  if(NOT TARGET_COUNT EQUAL EXPECTED_TARGET_COUNT)
    message(FATAL_ERROR "Step 7 unit target is not fully declared: ${TARGET}")
  endif()
endforeach()

if(NOT CSR_GENERATOR MATCHES "^Ninja")
  message(FATAL_ERROR "Step 7 target inventory requires Ninja")
endif()
require_step7_file("${NINJA_EXECUTABLE}")
require_step7_file("${CSR_BUILD_DIR}/build.ninja")
if(NOT DEFINED CMAKE_CTEST_COMMAND OR NOT EXISTS "${CMAKE_CTEST_COMMAND}")
  message(FATAL_ERROR "CMAKE_CTEST_COMMAND is required")
endif()
execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${CSR_BUILD_DIR}" --show-only=json-v1
    RESULT_VARIABLE CTEST_RESULT OUTPUT_VARIABLE CTEST_JSON ERROR_VARIABLE CTEST_ERROR)
if(NOT CTEST_RESULT EQUAL 0)
  message(FATAL_ERROR "cannot inspect Step 7 CTests: ${CTEST_ERROR}")
endif()

function(step7_test_property JSON INDEX PROPERTY OUTPUT)
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

function(require_step7_command_item COMMAND_VARIABLE REGEX DESCRIPTION)
  set(COUNT 0)
  foreach(ITEM IN LISTS ${COMMAND_VARIABLE})
    if(ITEM MATCHES "${REGEX}")
      math(EXPR COUNT "${COUNT} + 1")
    endif()
  endforeach()
  if(NOT COUNT EQUAL 1)
    message(FATAL_ERROR "Step 7 CTest command ${DESCRIPTION} count differs: ${COUNT}")
  endif()
endfunction()

set(EXPECTED_STEP7_CTESTS
    ista_csr_session_snapshot_adapter_contract_test
    ista_csr_session_context_test
    ista_csr_session_lifecycle_test
    ista_csr_snapshot_binding_contract_test
    ista_csr_step7_source_inventory
    ista_csr_session_controller_validation
    ista_csr_session_fresh_process)
string(JSON CTEST_COUNT LENGTH "${CTEST_JSON}" tests)
if(CTEST_COUNT EQUAL 0)
  message(FATAL_ERROR "Step 7 CTest inventory is empty")
endif()
math(EXPR CTEST_LAST "${CTEST_COUNT} - 1")
set(ACTUAL_STEP7_CTESTS)
set(ACTUAL_STEP8_CTEST_ROWS)
set(STEP7_ista_unit_COUNT 0)
set(STEP7_ista_property_COUNT 0)
set(STEP7_ista_smoke_COUNT 0)
foreach(INDEX RANGE 0 ${CTEST_LAST})
  string(JSON NAME GET "${CTEST_JSON}" tests ${INDEX} name)
  step7_test_property("${CTEST_JSON}" ${INDEX} LABELS ALL_LABELS)
  list(FIND STEP8_EXPECTED_CTEST_NAMES "${NAME}" STEP8_CTEST_INDEX)
  if(NOT STEP8_CTEST_INDEX EQUAL -1)
    list(LENGTH ALL_LABELS STEP8_LABEL_COUNT)
    if(NOT STEP8_LABEL_COUNT EQUAL 1)
      message(FATAL_ERROR "Step 7 catalog detector rejected Step 8 labels: ${NAME}=[${ALL_LABELS}]")
    endif()
    list(GET ALL_LABELS 0 STEP8_LABEL)
    list(APPEND ACTUAL_STEP8_CTEST_ROWS "${NAME}=${STEP8_LABEL}")
  endif()
  foreach(GLOBAL_LABEL ista_unit ista_property ista_smoke)
    list(FIND ALL_LABELS "${GLOBAL_LABEL}" GLOBAL_LABEL_INDEX)
    if(NOT GLOBAL_LABEL_INDEX EQUAL -1)
      set(LABEL_COUNT_VARIABLE "STEP7_${GLOBAL_LABEL}_COUNT")
      math(EXPR ${LABEL_COUNT_VARIABLE} "${${LABEL_COUNT_VARIABLE}} + 1")
    endif()
  endforeach()
  list(FIND EXPECTED_STEP7_CTESTS "${NAME}" STEP7_INDEX)
  if(STEP7_INDEX EQUAL -1)
    continue()
  endif()
  list(APPEND ACTUAL_STEP7_CTESTS "${NAME}")
  string(JSON COMMAND_COUNT LENGTH "${CTEST_JSON}" tests ${INDEX} command)
  set(COMMANDS)
  math(EXPR COMMAND_LAST "${COMMAND_COUNT} - 1")
  foreach(COMMAND_INDEX RANGE 0 ${COMMAND_LAST})
    string(JSON ITEM GET "${CTEST_JSON}" tests ${INDEX} command ${COMMAND_INDEX})
    list(APPEND COMMANDS "${ITEM}")
  endforeach()
  step7_test_property("${CTEST_JSON}" ${INDEX} LABELS LABELS)
  step7_test_property("${CTEST_JSON}" ${INDEX} TIMEOUT TIMEOUT)
  step7_test_property("${CTEST_JSON}" ${INDEX} ENVIRONMENT ENVIRONMENT)
  if(NAME MATCHES "(_adapter_contract_test|_context_test)$")
    set(EXPECTED_TIMEOUT 60)
    set(EXPECTED_ENVIRONMENT)
    if(NOT COMMAND_COUNT EQUAL 1)
      message(FATAL_ERROR "Step 7 unit command must be singleton: ${NAME}")
    endif()
    list(GET COMMANDS 0 EXECUTABLE)
    get_filename_component(EXECUTABLE_NAME "${EXECUTABLE}" NAME)
    if(NOT EXECUTABLE_NAME STREQUAL NAME)
      message(FATAL_ERROR "Step 7 unit executable mismatch: ${NAME}: ${EXECUTABLE_NAME}")
    endif()
  elseif(NAME MATCHES "(_lifecycle_test|_binding_contract_test)$")
    set(EXPECTED_TIMEOUT 120)
    set(EXPECTED_ENVIRONMENT)
    if(NOT COMMAND_COUNT EQUAL 1)
      message(FATAL_ERROR "Step 7 unit command must be singleton: ${NAME}")
    endif()
    list(GET COMMANDS 0 EXECUTABLE)
    get_filename_component(EXECUTABLE_NAME "${EXECUTABLE}" NAME)
    if(NOT EXECUTABLE_NAME STREQUAL NAME)
      message(FATAL_ERROR "Step 7 unit executable mismatch: ${NAME}: ${EXECUTABLE_NAME}")
    endif()
  elseif(NAME STREQUAL ista_csr_step7_source_inventory)
    set(EXPECTED_TIMEOUT 120)
    set(EXPECTED_ENVIRONMENT)
    if(NOT COMMAND_COUNT EQUAL 14)
      message(FATAL_ERROR "Step 7 inventory CTest command must have 14 arguments")
    endif()
    foreach(PREFIX
            "^-DISTA_SOURCE_ROOT=" "^-DCSR_TEST_ROOT=" "^-DCSR_BUILD_DIR="
            "^-DNINJA_EXECUTABLE=" "^-DCXX_COMPILER=" "^-DNM_EXECUTABLE="
            "^-DCSR_GENERATOR=" "^-DCSR_ADAPTER_SOURCES_PACKED="
            "^-DCSR_CONTEXT_SOURCES_PACKED=" "^-DCSR_LIFECYCLE_SOURCES_PACKED="
            "^-DCSR_BINDING_SOURCES_PACKED=" "^-P$"
            "CheckCsrStep7SourceInventory\\.cmake$")
      require_step7_command_item(COMMANDS "${PREFIX}" "${PREFIX}")
    endforeach()
  elseif(NAME STREQUAL ista_csr_session_controller_validation)
    set(EXPECTED_TIMEOUT 60)
    set(EXPECTED_ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
    if(NOT COMMAND_COUNT EQUAL 7)
      message(FATAL_ERROR "Step 7 controller validation command must have 7 arguments")
    endif()
    foreach(PATTERN
            "csr_session_controller\\.py$" "^--manifest$" "baseline/manifest\\.json$"
            "^--default-compat$" "projection_default_compat_v1\\.json$" "^--self-test$")
      require_step7_command_item(COMMANDS "${PATTERN}" "${PATTERN}")
    endforeach()
  else()
    set(EXPECTED_TIMEOUT 1050)
    set(EXPECTED_ENVIRONMENT PYTHONDONTWRITEBYTECODE=1)
    if(NOT COMMAND_COUNT EQUAL 14)
      message(FATAL_ERROR "Step 7 fresh-process command must have 14 arguments")
    endif()
    foreach(PATTERN
            "csr_session_controller\\.py$" "^--probe$" "ista_csr_projection_probe$"
            "^--manifest$" "baseline/manifest\\.json$" "^--default-compat$"
            "projection_default_compat_v1\\.json$" "^--run-root$"
            "csr-session-runs$" "^--repeats$" "^3$" "^--timeout-seconds$" "^75$")
      require_step7_command_item(COMMANDS "${PATTERN}" "${PATTERN}")
    endforeach()
  endif()
  if(NOT "${LABELS}" STREQUAL "ista_unit" AND NOT NAME STREQUAL ista_csr_session_fresh_process)
    message(FATAL_ERROR "Step 7 unit label differs: ${NAME}: [${LABELS}]")
  elseif(NAME STREQUAL ista_csr_session_fresh_process AND NOT "${LABELS}" STREQUAL "ista_smoke")
    message(FATAL_ERROR "Step 7 smoke label differs: [${LABELS}]")
  endif()
  if(NOT TIMEOUT EQUAL EXPECTED_TIMEOUT
     OR NOT "${ENVIRONMENT}" STREQUAL "${EXPECTED_ENVIRONMENT}")
    message(FATAL_ERROR
            "Step 7 CTest properties differ: ${NAME}: timeout=[${TIMEOUT}] env=[${ENVIRONMENT}]")
  endif()
endforeach()
check_step7_catalog("${STEP7_ista_unit_COUNT}" "${STEP7_ista_property_COUNT}"
                    "${STEP7_ista_smoke_COUNT}" ACTUAL_STEP8_CTEST_ROWS)
assert_step7_inventory(ACTUAL_STEP7_CTESTS EXPECTED_STEP7_CTESTS "exact 6 unit + 1 smoke CTests")

message(STATUS "CSR Step 7 exact target graph and CTest inventory passed")
