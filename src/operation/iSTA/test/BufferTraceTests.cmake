# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(ISTA_BUFFER_TRACE_TEST_COMPILE_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

function(ista_register_buffer_trace_test TARGET_NAME)
  target_compile_options(${TARGET_NAME}
      PRIVATE
      ${ISTA_BUFFER_TRACE_TEST_COMPILE_OPTIONS}
  )
  add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
  set_tests_properties(${TARGET_NAME}
      PROPERTIES LABELS "ista_unit;ista_buffer"
  )
endfunction()

set(ISTA_BUFFER_CANDIDATE_SELECTOR_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/BufferCandidateSelectorTest.cpp"
)
add_executable(ista_buffer_candidate_selector_test
    ${ISTA_BUFFER_CANDIDATE_SELECTOR_TEST_SOURCES}
)
target_link_libraries(ista_buffer_candidate_selector_test
    PRIVATE
    ista_buffer_trace_contract
)
ista_register_buffer_trace_test(ista_buffer_candidate_selector_test)

set(ISTA_BUFFER_TRACE_CONTRACT_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/BufferTraceContractTest.cpp"
)
add_executable(ista_buffer_trace_contract_test
    ${ISTA_BUFFER_TRACE_CONTRACT_TEST_SOURCES}
)
target_link_libraries(ista_buffer_trace_contract_test
    PRIVATE
    ista_buffer_trace_contract
)
ista_register_buffer_trace_test(ista_buffer_trace_contract_test)
set(ISTA_BUFFER_TRACE_EXTRACTOR_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/BufferTraceObservationExtractorTest.cpp"
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_BUFFER_TRACE_EXTRACTOR_TEST_LINKS
    ista_buffer_trace_runtime
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
add_executable(ista_buffer_trace_observation_extractor_test
    ${ISTA_BUFFER_TRACE_EXTRACTOR_TEST_SOURCES}
)
target_link_libraries(ista_buffer_trace_observation_extractor_test
    PRIVATE
    ${ISTA_BUFFER_TRACE_EXTRACTOR_TEST_LINKS}
)
ista_register_buffer_trace_test(
    ista_buffer_trace_observation_extractor_test)


set(ISTA_BUFFER_TRACE_REPLAYER_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/BufferTraceReplayerTest.cpp"
)
add_executable(ista_buffer_trace_replayer_test
    ${ISTA_BUFFER_TRACE_REPLAYER_TEST_SOURCES}
)
target_link_libraries(ista_buffer_trace_replayer_test
    PRIVATE
    ista_buffer_trace_runtime
)
ista_register_buffer_trace_test(ista_buffer_trace_replayer_test)

set(ISTA_BUFFER_TRACE_PARASITIC_BOUNDARY_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/BufferTraceParasiticBoundaryTest.cpp"
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_BUFFER_TRACE_PARASITIC_BOUNDARY_LINKS
    ista_timing_edit_transaction
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
add_executable(ista_buffer_trace_parasitic_full_boundary
    ${ISTA_BUFFER_TRACE_PARASITIC_BOUNDARY_SOURCES}
)
target_link_libraries(ista_buffer_trace_parasitic_full_boundary
    PRIVATE
    ${ISTA_BUFFER_TRACE_PARASITIC_BOUNDARY_LINKS}
)
ista_register_buffer_trace_test(ista_buffer_trace_parasitic_full_boundary)

foreach(PROPERTY_SEED IN ITEMS 1 7 42 20260831 3735928559)
  add_test(
      NAME ista_buffer_trace_property_seed_${PROPERTY_SEED}
      COMMAND ista_buffer_candidate_selector_test
              --property-seed ${PROPERTY_SEED}
  )
  set_tests_properties(ista_buffer_trace_property_seed_${PROPERTY_SEED}
      PROPERTIES LABELS "ista_unit;ista_buffer;ista_property"
  )
endforeach()

add_test(
    NAME ista_buffer_trace_schema_validation
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/../benchmark/schema/validate_buffer_fixed_trace.py"
    --schema
    "${CMAKE_CURRENT_LIST_DIR}/../benchmark/schema/buffer_fixed_trace_v1.schema.json"
    --manifest
    "${CMAKE_CURRENT_LIST_DIR}/../benchmark/schema/buffer_trace_contract_manifest_v1.json"
    --self-test
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/synthetic_two_batch.json"
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/real_buffer_fanout.json"
)
set_tests_properties(ista_buffer_trace_schema_validation
    PROPERTIES
    LABELS "ista_unit;ista_buffer"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME ista_buffer_trace_fixture_manifest
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/validate_buffer_trace_manifest.py"
    --manifest
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/manifest.json"
)
set_tests_properties(ista_buffer_trace_fixture_manifest
    PROPERTIES
    LABELS "ista_unit;ista_buffer"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

set(ISTA_BUFFER_TRACE_PROBE_SOURCES
    timing_edit/BufferTraceProductionFreshProbe.cpp
    timing_edit/BufferTraceProductionFreshAdapter.cpp
    timing_edit/BufferTraceProductionFreshSupport.cpp
    timing_edit/BufferTraceProductionGainValidator.cpp
    timing_edit/BufferTraceCsrJson.cpp
    timing_edit/BufferTraceReceiptJson.cpp
    timing_edit/oracle/ProductionFreshSessionLoader.cpp
    timing_edit/StaTimingEditSessionProbeAccess.cpp
    timing_edit/StaTimingEditCsrReceiptProbe.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrTimingResultFromNativeOracle.cpp
)
set(ISTA_BUFFER_TRACE_PROBE_LINKS
    ista_buffer_trace_runtime
    ista_timing_edit_transaction
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_interface
    ista_source
    ista_data_manager
    ista_delay_calculator
    idm
)

add_executable(ista_buffer_trace_production_fresh_probe
    ${ISTA_BUFFER_TRACE_PROBE_SOURCES}
)
target_include_directories(ista_buffer_trace_production_fresh_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle"
)
target_link_libraries(ista_buffer_trace_production_fresh_probe PRIVATE
    ${ISTA_BUFFER_TRACE_PROBE_LINKS}
)
target_compile_options(ista_buffer_trace_production_fresh_probe PRIVATE
    ${ISTA_BUFFER_TRACE_TEST_COMPILE_OPTIONS}
)

add_test(
    NAME ista_buffer_trace_one_shot_reuse_reject
    COMMAND
    ista_buffer_trace_production_fresh_probe
    self-test-double-invocation
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/synthetic_two_batch.json"
    --output
    "${CMAKE_CURRENT_BINARY_DIR}/stage4-one-shot-reuse.json"
)
set_tests_properties(ista_buffer_trace_one_shot_reuse_reject
    PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_buffer"
    TIMEOUT 900)

add_test(
    NAME ista_buffer_trace_first_empty_generation
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/buffer_trace_first_empty_test.py"
    --probe "$<TARGET_FILE:ista_buffer_trace_production_fresh_probe>"
    --tech-lef
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout/tech.lef"
    --cell-lef
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout/cell.lef"
    --liberty
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout/stage4.lib"
    --verilog
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_no_gain/design.v"
    --sdc
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_no_gain/design.sdc"
    --top stage4_buffer_no_gain
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/stage4-controller/first-empty"
)
set_tests_properties(ista_buffer_trace_first_empty_generation
    PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_buffer"
    TIMEOUT 900
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME ista_buffer_trace_terminal_empty_generation
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/buffer_trace_terminal_empty_test.py"
    --probe "$<TARGET_FILE:ista_buffer_trace_production_fresh_probe>"
    --base-template
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/templates/synthetic_two_batch.template.json"
    --verilog
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_terminal_empty/design.v"
    --sdc
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_terminal_empty/design.sdc"
    --top stage4_buffer_terminal_empty
    --case-id synthetic_terminal_empty
    --seed 43
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/stage4-controller/terminal-empty"
)
set_tests_properties(ista_buffer_trace_terminal_empty_generation
    PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_buffer"
    TIMEOUT 1200
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME ista_buffer_trace_preloader_rejection
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/buffer_trace_preloader_rejection_test.py"
    --probe "$<TARGET_FILE:ista_buffer_trace_production_fresh_probe>"
    --tech-lef
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout/tech.lef"
    --cell-lef
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout/cell.lef"
    --liberty
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout/stage4.lib"
    --verilog
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout/design.v"
    --sdc
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout/design.sdc"
    --top stage4_buffer_fanout
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/stage4-controller/preloader-rejection"
)
set_tests_properties(ista_buffer_trace_preloader_rejection
    PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_buffer"
    TIMEOUT 900
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME ista_buffer_trace_fresh_process
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/buffer_trace_controller.py"
    --probe "$<TARGET_FILE:ista_buffer_trace_production_fresh_probe>"
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/synthetic_two_batch.json"
    --template
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/templates/synthetic_two_batch.template.json"
    --case-id synthetic_two_batch
    --seed 42
    --repeats 3
    --run-root "${CMAKE_CURRENT_BINARY_DIR}/stage4-controller/synthetic"
)
set_tests_properties(ista_buffer_trace_fresh_process
    PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_smoke;ista_buffer"
    TIMEOUT 1800
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME ista_buffer_trace_source_isolation
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/buffer_trace_controller.py"
    --probe "$<TARGET_FILE:ista_buffer_trace_production_fresh_probe>"
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/real_buffer_fanout.json"
    --template
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/templates/real_buffer_fanout.template.json"
    --case-id real_buffer_fanout
    --seed 0
    --repeats 3
    --run-root "${CMAKE_CURRENT_BINARY_DIR}/stage4-controller/real"
)
set_tests_properties(ista_buffer_trace_source_isolation
    PROPERTIES
    LABELS "ista_integration;ista_smoke;ista_buffer"
    TIMEOUT 1800
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(
    NAME ista_buffer_trace_controller_selftest
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/buffer_trace_controller.py"
    --self-test
)
set_tests_properties(ista_buffer_trace_controller_selftest
    PROPERTIES
    LABELS "ista_unit;ista_buffer"
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)
