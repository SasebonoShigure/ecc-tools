# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(ISTA_DUAL_CSR_TRACE_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

set(ISTA_DUAL_CSR_FRESH_PROBE_SOURCES
    timing_dual_csr/DualCsrTraceProductionFreshProbe.cpp
    timing_dual_csr/DualCsrTraceProductionFreshAdapter.cpp
    timing_dual_csr/DualCsrTraceProductionTargetedAdapter.cpp
    timing_dual_csr/DualCsrTargetedArtifactSupport.cpp
    timing_dual_csr/DualCsrTargetedPhysicalProjection.cpp
    timing_dual_csr/DualCsrFunctionalReceiptJson.cpp
    timing_dual_csr/DualCsrFunctionalGraphJson.cpp
    timing_dual_csr/DualCsrFunctionalTimingJson.cpp
    timing_dual_csr/DualCsrReceiptJsonCommon.cpp
    timing_dual_csr/DualCsrExactReceiptJson.cpp
    timing_dual_csr/DualCsrExactReceiptSemanticJson.cpp
    timing_dual_csr/DualCsrResourceObservationJson.cpp
    timing_dual_csr/DualCsrSourceIsolationJson.cpp
    timing_edit/BufferTraceProductionFreshAdapter.cpp
    timing_edit/BufferTraceProductionFreshSupport.cpp
    timing_edit/oracle/ProductionFreshSessionLoader.cpp
    timing_edit/StaTimingEditSessionProbeAccess.cpp
    timing_edit/StaTimingEditCsrReceiptProbe.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrTimingResultFromNativeOracle.cpp
)
set(ISTA_DUAL_CSR_FRESH_PROBE_LINKS
    ista_dual_csr_trace_runtime
    ista_timing_dual_csr
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

add_executable(ista_dual_csr_production_fresh_probe
    ${ISTA_DUAL_CSR_FRESH_PROBE_SOURCES}
)
target_include_directories(ista_dual_csr_production_fresh_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle"
    "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr"
    "${CMAKE_CURRENT_LIST_DIR}/../source/module/timing_dual_csr_transaction"
    "${CMAKE_CURRENT_LIST_DIR}/../source/module/timing_dual_csr_native"
)
target_link_libraries(ista_dual_csr_production_fresh_probe PRIVATE
    ${ISTA_DUAL_CSR_FRESH_PROBE_LINKS}
)
target_compile_features(ista_dual_csr_production_fresh_probe PRIVATE cxx_std_20)
target_compile_options(ista_dual_csr_production_fresh_probe PRIVATE
    ${ISTA_DUAL_CSR_TRACE_STRICT_OPTIONS}
)

get_target_property(ISTA_DUAL_CSR_FRESH_ACTUAL_SOURCES
    ista_dual_csr_production_fresh_probe SOURCES)
ista_assert_same_inventory(
    ISTA_DUAL_CSR_FRESH_ACTUAL_SOURCES
    ISTA_DUAL_CSR_FRESH_PROBE_SOURCES
    "Stage 5 production-fresh semantic probe source inventory"
)
ista_get_target_property_or_empty(
    ISTA_DUAL_CSR_FRESH_ACTUAL_LINKS
    ista_dual_csr_production_fresh_probe
    LINK_LIBRARIES
)
ista_assert_same_inventory(
    ISTA_DUAL_CSR_FRESH_ACTUAL_LINKS
    ISTA_DUAL_CSR_FRESH_PROBE_LINKS
    "Stage 5 production-fresh semantic probe direct links"
)
ista_get_target_property_or_empty(
    ISTA_DUAL_CSR_FRESH_ACTUAL_OPTIONS
    ista_dual_csr_production_fresh_probe
    COMPILE_OPTIONS
)
ista_assert_same_inventory(
    ISTA_DUAL_CSR_FRESH_ACTUAL_OPTIONS
    ISTA_DUAL_CSR_TRACE_STRICT_OPTIONS
    "Stage 5 production-fresh semantic probe strict compile options"
)

file(REAL_PATH "${CMAKE_CURRENT_LIST_DIR}/../benchmark/dual_csr"
     ISTA_DUAL_CSR_RUNTIME_SOURCE_DIR)
set(ISTA_DUAL_CSR_RUNTIME_EXPECTED_SOURCES
    "${ISTA_DUAL_CSR_RUNTIME_SOURCE_DIR}/DualCsrTraceBaseline.cpp"
    "${ISTA_DUAL_CSR_RUNTIME_SOURCE_DIR}/DualCsrNativeCandidateCarrier.cpp"
    "${ISTA_DUAL_CSR_RUNTIME_SOURCE_DIR}/DualCsrTraceReplayer.cpp"
)
set(ISTA_DUAL_CSR_RUNTIME_EXPECTED_LINKS
    ista_timing_dual_csr
    ista_timing_dual_csr_native
    ista_timing_dual_csr_transaction
    ista_buffer_trace_contract
    ista_timing_edit_transaction
    ista_timing_database_headers
)
get_target_property(ISTA_DUAL_CSR_RUNTIME_ACTUAL_SOURCES
    ista_dual_csr_trace_runtime SOURCES)
ista_assert_same_inventory(
    ISTA_DUAL_CSR_RUNTIME_ACTUAL_SOURCES
    ISTA_DUAL_CSR_RUNTIME_EXPECTED_SOURCES
    "Stage 5 trace runtime source inventory"
)
ista_get_target_property_or_empty(
    ISTA_DUAL_CSR_RUNTIME_ACTUAL_LINKS
    ista_dual_csr_trace_runtime
    LINK_LIBRARIES
)
ista_assert_same_inventory(
    ISTA_DUAL_CSR_RUNTIME_ACTUAL_LINKS
    ISTA_DUAL_CSR_RUNTIME_EXPECTED_LINKS
    "Stage 5 trace runtime direct links"
)
ista_get_target_property_or_empty(
    ISTA_DUAL_CSR_RUNTIME_ACTUAL_OPTIONS
    ista_dual_csr_trace_runtime
    COMPILE_OPTIONS
)
ista_assert_same_inventory(
    ISTA_DUAL_CSR_RUNTIME_ACTUAL_OPTIONS
    ISTA_DUAL_CSR_TRACE_STRICT_OPTIONS
    "Stage 5 trace runtime strict compile options"
)

add_test(
    NAME ista_dual_csr_trace_runtime_source_owner
    COMMAND "${CMAKE_COMMAND}"
        "-DRUNTIME_DIR=${CMAKE_CURRENT_LIST_DIR}/../benchmark/dual_csr"
        -P
        "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/CheckDualCsrTraceRuntimeOwnership.cmake"
)
set_tests_properties(ista_dual_csr_trace_runtime_source_owner PROPERTIES
    LABELS "ista_unit;ista_dual_csr"
    TIMEOUT 60
)

add_test(
    NAME ista_dual_csr_trace_controller_selftest
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/dual_csr_trace_controller.py"
    --self-test
)
set_tests_properties(ista_dual_csr_trace_controller_selftest PROPERTIES
    LABELS "ista_unit;ista_dual_csr"
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME ista_dual_csr_frozen_synthetic_differential
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/dual_csr_trace_controller.py"
    --probe "$<TARGET_FILE:ista_dual_csr_production_fresh_probe>"
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/synthetic_two_batch.json"
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/stage5-controller/synthetic"
    --timeout-seconds 900
)
set_tests_properties(ista_dual_csr_frozen_synthetic_differential PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_dual_csr"
    TIMEOUT 2400
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME ista_dual_csr_frozen_real_differential
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/dual_csr_trace_controller.py"
    --probe "$<TARGET_FILE:ista_dual_csr_production_fresh_probe>"
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/real_buffer_fanout.json"
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/stage5-controller/real"
    --timeout-seconds 900
)
set_tests_properties(ista_dual_csr_frozen_real_differential PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_dual_csr"
    TIMEOUT 1800
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME ista_dual_csr_release_timing_smoke
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/dual_csr_timing_smoke.py"
    --probe "$<TARGET_FILE:ista_dual_csr_production_fresh_probe>"
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/synthetic_two_batch.json"
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/stage5-timing-smoke/runs"
    --output
    "${CMAKE_CURRENT_BINARY_DIR}/stage5-timing-smoke/timing-smoke.json"
    --build-config "$<CONFIG>"
    --compiler-id "${CMAKE_CXX_COMPILER_ID}"
    --compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
    --warmup-runs 1
    --samples 6
    --timeout-seconds 900
)
set_tests_properties(ista_dual_csr_release_timing_smoke PROPERTIES
    LABELS "ista_smoke;ista_cpu;ista_dual_csr"
    TIMEOUT 7200
    SKIP_RETURN_CODE 77
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)
