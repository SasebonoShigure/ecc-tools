# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Licensed under Mulan PSL v2.

set(ISTA_TIMING_EDIT_ORACLE_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)
set(ISTA_TIMING_EDIT_ORACLE_SOURCES
    timing_edit/oracle/IndependentFreshBaselineLoader.cpp
    timing_edit/oracle/IndependentSemanticReplayer.cpp
    timing_edit/oracle/TimingEditOracleCli.cpp
    timing_edit/oracle/IndependentOracleObservation.cpp
    timing_edit/oracle/IndependentFreshOracleProbe.cpp
)
set(ISTA_TIMING_EDIT_ORACLE_LINKS
    ista_timing_edit_contract
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_graph_builder
    ista_delay_calculator
    ista_clock_propagator
    ista_timing_propagator
    ista_timing_analyzer
    ista_interface
    ista_source
    idm
)

add_executable(ista_timing_edit_independent_oracle_probe ${ISTA_TIMING_EDIT_ORACLE_SOURCES})
target_include_directories(
    ista_timing_edit_independent_oracle_probe
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle"
)
target_link_libraries(ista_timing_edit_independent_oracle_probe PRIVATE ${ISTA_TIMING_EDIT_ORACLE_LINKS})
target_compile_options(ista_timing_edit_independent_oracle_probe PRIVATE ${ISTA_TIMING_EDIT_ORACLE_STRICT_OPTIONS})

set(ISTA_TIMING_EDIT_PRODUCTION_ORACLE_SOURCES
    timing_edit/StaTimingEditSessionProbeAccess.cpp
    timing_edit/StaTimingEditCsrReceiptProbe.cpp
    timing_edit/oracle/TimingEditOracleCli.cpp
    timing_edit/oracle/IndependentOracleObservation.cpp
    timing_edit/oracle/ProductionFreshSessionLoader.cpp
    timing_edit/oracle/ProductionFreshOracleProbe.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrTimingResultFromNativeOracle.cpp
)
set(ISTA_TIMING_EDIT_PRODUCTION_ORACLE_LINKS
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
add_executable(ista_timing_edit_production_oracle_probe
    ${ISTA_TIMING_EDIT_PRODUCTION_ORACLE_SOURCES})
target_include_directories(ista_timing_edit_production_oracle_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle"
)
target_link_libraries(ista_timing_edit_production_oracle_probe
    PRIVATE ${ISTA_TIMING_EDIT_PRODUCTION_ORACLE_LINKS})
target_compile_options(ista_timing_edit_production_oracle_probe
    PRIVATE ${ISTA_TIMING_EDIT_ORACLE_STRICT_OPTIONS})

add_test(
    NAME ista_timing_edit_independent_oracle_wrong_sink_selftest
    COMMAND ista_timing_edit_independent_oracle_probe --self-test-wrong-sink
)
set_tests_properties(
    ista_timing_edit_independent_oracle_wrong_sink_selftest
    PROPERTIES LABELS "ista_unit;ista_timing_edit" TIMEOUT 60
)

add_test(
    NAME ista_timing_edit_fresh_oracle_differential
    COMMAND
    "${CMAKE_COMMAND}"
    "-DORACLE_EXECUTABLE=$<TARGET_FILE:ista_timing_edit_independent_oracle_probe>"
    "-DPRODUCTION_EXECUTABLE=$<TARGET_FILE:ista_timing_edit_production_oracle_probe>"
    "-DPYTHON_EXECUTABLE=${ISTA_TEST_PYTHON_EXECUTABLE}"
    "-DTRACE_VALIDATOR=${CMAKE_CURRENT_LIST_DIR}/../benchmark/schema/validate_timing_edit_trace.py"
    "-DTRACE_SCHEMA=${CMAKE_CURRENT_LIST_DIR}/../benchmark/schema/timing_edit_trace_v1.schema.json"
    "-DTECH_LEF=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0_tech.lef"
    "-DCELL_LEF=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0_cells.lef"
    "-DLIBERTY=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0.lib"
    "-DVERILOG=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/chain.v"
    "-DSDC=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/chain.sdc"
    "-DRUN_ROOT=${CMAKE_CURRENT_BINARY_DIR}/timing-edit-oracle-runs"
    -P "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle/RunFreshOracleDifferential.cmake"
)
set_tests_properties(
    ista_timing_edit_fresh_oracle_differential
    PROPERTIES LABELS "ista_integration;ista_cpu;ista_smoke;ista_timing_edit" TIMEOUT 600 RUN_SERIAL TRUE
)

add_test(
    NAME ista_timing_edit_production_oracle_derived_tamper_rollback
    COMMAND
    "${CMAKE_COMMAND}"
    "-DORACLE_EXECUTABLE=$<TARGET_FILE:ista_timing_edit_independent_oracle_probe>"
    "-DPRODUCTION_EXECUTABLE=$<TARGET_FILE:ista_timing_edit_production_oracle_probe>"
    "-DTECH_LEF=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0_tech.lef"
    "-DCELL_LEF=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0_cells.lef"
    "-DLIBERTY=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0.lib"
    "-DVERILOG=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/chain.v"
    "-DSDC=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/chain.sdc"
    "-DRUN_ROOT=${CMAKE_CURRENT_BINARY_DIR}/timing-edit-derived-tamper-run"
    -P "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle/RunProductionDerivedTamper.cmake"
)
set_tests_properties(
    ista_timing_edit_production_oracle_derived_tamper_rollback
    PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_smoke;ista_timing_edit"
    TIMEOUT 300
    RUN_SERIAL TRUE
)

add_test(
    NAME ista_timing_edit_oracle_rejects_non_buffer_selection
    COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle/test_trace_creation_inputs.py"
    --probe "$<TARGET_FILE:ista_timing_edit_independent_oracle_probe>"
    --fixture-dir "${CMAKE_CURRENT_LIST_DIR}/timing_edit/fixtures/buffer_fanout"
    --run-root "${CMAKE_CURRENT_BINARY_DIR}/non-buffer-creation"
)
set_tests_properties(ista_timing_edit_oracle_rejects_non_buffer_selection PROPERTIES
    LABELS "ista_timing_edit;ista_diagnostic;ista_integration" TIMEOUT 90)
