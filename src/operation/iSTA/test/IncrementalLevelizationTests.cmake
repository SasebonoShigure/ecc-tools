# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(ISTA_STAGE6_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

set(ISTA_STAGE6_EXTERNAL_LEVEL_SOURCES
    timing_incremental_levelization/CsrExternalLevelBackendTest.cpp
    CsrCpuReferenceTestFixture.cpp
)
set(ISTA_STAGE6_EXTERNAL_LEVEL_LINKS ista_timing_csr_core)

add_executable(ista_csr_external_level_backend_test
    ${ISTA_STAGE6_EXTERNAL_LEVEL_SOURCES}
)
target_link_libraries(ista_csr_external_level_backend_test
    PRIVATE
    ${ISTA_STAGE6_EXTERNAL_LEVEL_LINKS}
)
target_compile_options(ista_csr_external_level_backend_test
    PRIVATE
    ${ISTA_STAGE6_STRICT_OPTIONS}
)

add_test(NAME ista_csr_external_level_backend_test
         COMMAND ista_csr_external_level_backend_test)
set_tests_properties(ista_csr_external_level_backend_test PROPERTIES
    LABELS "ista_unit;ista_incremental_levelization;ista_timing_incremental_levelization"
    TIMEOUT 60
)

set(ISTA_STAGE6_RECEIPT_SOURCES
    timing_incremental_levelization/Stage6FunctionalReceiptTest.cpp
    timing_incremental_levelization/Stage6FunctionalReceipt.cpp
)
set(ISTA_STAGE6_RECEIPT_LINKS ista_timing_dual_csr)

add_executable(ista_stage6_functional_receipt_test
    ${ISTA_STAGE6_RECEIPT_SOURCES}
)
target_link_libraries(ista_stage6_functional_receipt_test
    PRIVATE
    ${ISTA_STAGE6_RECEIPT_LINKS}
)
target_compile_features(ista_stage6_functional_receipt_test PRIVATE cxx_std_20)
target_compile_options(ista_stage6_functional_receipt_test
    PRIVATE
    ${ISTA_STAGE6_STRICT_OPTIONS}
)

add_test(NAME ista_stage6_functional_receipt_test
         COMMAND ista_stage6_functional_receipt_test)
set_tests_properties(ista_stage6_functional_receipt_test PROPERTIES
    LABELS "ista_unit;ista_incremental_levelization;ista_timing_incremental_levelization"
    TIMEOUT 60
)

set(ISTA_STAGE6_PRODUCTION_FRESH_PROBE_SOURCES
    timing_incremental_levelization/IncrementalLevelizationTraceProductionFreshProbe.cpp
    timing_incremental_levelization/Stage6ProductionTargetedAdapter.cpp
    timing_incremental_levelization/Stage6FunctionalReceipt.cpp
    timing_dual_csr/DualCsrTraceProductionFreshAdapter.cpp
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
set(ISTA_STAGE6_PRODUCTION_FRESH_PROBE_LINKS
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

add_executable(ista_stage6_production_fresh_probe
    ${ISTA_STAGE6_PRODUCTION_FRESH_PROBE_SOURCES}
)
target_include_directories(ista_stage6_production_fresh_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle"
    "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr"
    "${CMAKE_CURRENT_LIST_DIR}/timing_incremental_levelization"
    "${CMAKE_CURRENT_LIST_DIR}/../source/module/timing_dual_csr_transaction"
    "${CMAKE_CURRENT_LIST_DIR}/../source/module/timing_dual_csr_native"
)
target_link_libraries(ista_stage6_production_fresh_probe PRIVATE
    ${ISTA_STAGE6_PRODUCTION_FRESH_PROBE_LINKS}
)
target_compile_features(ista_stage6_production_fresh_probe PRIVATE cxx_std_20)
target_compile_options(ista_stage6_production_fresh_probe PRIVATE
    ${ISTA_STAGE6_STRICT_OPTIONS}
)

add_test(
    NAME ista_stage6_levelization_controller_selftest
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_incremental_levelization/incremental_levelization_controller.py"
    --self-test
)
set_tests_properties(ista_stage6_levelization_controller_selftest PROPERTIES
    LABELS "ista_unit;ista_incremental_levelization;ista_timing_incremental_levelization"
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

# These dependencies reuse the focused operation-guard receipt: full backend
# delta=1, caller-supplied delta=0, and malformed caller-supplied delta=0.
set(ISTA_STAGE6_CONTROLLER_DEPENDENCIES
    "ista_csr_external_level_backend_test;ista_stage6_functional_receipt_test;ista_stage6_levelization_controller_selftest"
)

add_test(
    NAME ista_stage6_frozen_synthetic_differential
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_incremental_levelization/incremental_levelization_controller.py"
    --probe "$<TARGET_FILE:ista_stage6_production_fresh_probe>"
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/synthetic_two_batch.json"
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/stage6-controller/synthetic"
    --timeout-seconds 900
)
set_tests_properties(ista_stage6_frozen_synthetic_differential PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_incremental_levelization;ista_timing_incremental_levelization"
    TIMEOUT 2400
    DEPENDS "${ISTA_STAGE6_CONTROLLER_DEPENDENCIES}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME ista_stage6_frozen_real_differential
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_incremental_levelization/incremental_levelization_controller.py"
    --probe "$<TARGET_FILE:ista_stage6_production_fresh_probe>"
    --trace
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/real_buffer_fanout.json"
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/stage6-controller/real"
    --timeout-seconds 900
)
set_tests_properties(ista_stage6_frozen_real_differential PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_incremental_levelization;ista_timing_incremental_levelization"
    TIMEOUT 1800
    DEPENDS "${ISTA_STAGE6_CONTROLLER_DEPENDENCIES}"
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

set(ISTA_STAGE6_RECOVERY_SOURCES
    timing_dual_csr/DualCsrFunctionalReceiptJson.cpp
    timing_dual_csr/DualCsrFunctionalGraphJson.cpp
    timing_dual_csr/DualCsrFunctionalTimingJson.cpp
    timing_dual_csr/DualCsrReceiptJsonCommon.cpp
    timing_incremental_levelization/DualCsrModeRecoveryReplayTest.cpp
    timing_dual_csr/DualCsrStructuredBatchTestSupport.cpp
    timing_edit/StaTimingEditSessionProbeAccess.cpp
    timing_edit/StaTimingEditCsrReceiptProbe.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrTimingResultFromNativeOracle.cpp
    timing_edit/TimingEditNativeFixture.cpp
    timing_edit/TimingEditTransactionNativeSupport.cpp
)
set(ISTA_STAGE6_RECOVERY_LINKS
    ista_dual_csr_trace_runtime
    ista_timing_edit_transaction
    ista_timing_dual_csr_native
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_interface
    ista_data_manager
    ista_delay_calculator
)

add_executable(ista_stage6_mode_recovery_replay_test
    ${ISTA_STAGE6_RECOVERY_SOURCES}
)
target_include_directories(ista_stage6_mode_recovery_replay_test
    PRIVATE
    ${ISTA_MODULE}/timing_csr_snapshot
    ${ISTA_BENCHMARK}/dual_csr
    ${ISTA_TEST}/timing_dual_csr
    ${ISTA_TEST}/timing_edit
)
target_link_libraries(ista_stage6_mode_recovery_replay_test
    PRIVATE
    ${ISTA_STAGE6_RECOVERY_LINKS}
)
target_compile_features(
    ista_stage6_mode_recovery_replay_test PRIVATE cxx_std_20)
target_compile_options(ista_stage6_mode_recovery_replay_test
    PRIVATE
    ${ISTA_STAGE6_STRICT_OPTIONS}
)

add_test(NAME ista_stage6_mode_recovery_replay_test
         COMMAND ista_stage6_mode_recovery_replay_test)
set_tests_properties(ista_stage6_mode_recovery_replay_test PROPERTIES
    LABELS "ista_unit;ista_incremental_levelization;ista_timing_incremental_levelization;ista_transaction"
    TIMEOUT 120
)

set(ISTA_STAGE6_ACTIVE_PAYLOAD_SOURCES
    timing_incremental_levelization/Stage6ActivePayloadMicrocaseTest.cpp
    timing_incremental_levelization/Stage6FunctionalReceipt.cpp
    timing_dual_csr/DualCsrStructuredBatchTestSupport.cpp
    timing_edit/StaTimingEditSessionProbeAccess.cpp
    timing_edit/StaTimingEditCsrReceiptProbe.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrTimingResultFromNativeOracle.cpp
    timing_edit/TimingEditNativeFixture.cpp
    timing_edit/TimingEditTransactionNativeSupport.cpp
)
set(ISTA_STAGE6_ACTIVE_PAYLOAD_LINKS
    ista_dual_csr_trace_runtime
    ista_timing_edit_transaction
    ista_timing_dual_csr_native
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_interface
    ista_data_manager
    ista_delay_calculator
)

add_executable(ista_stage6_active_payload_microcase_test
    ${ISTA_STAGE6_ACTIVE_PAYLOAD_SOURCES}
)
target_include_directories(ista_stage6_active_payload_microcase_test
    PRIVATE
    ${ISTA_MODULE}/timing_csr_snapshot
    ${ISTA_BENCHMARK}/dual_csr
    ${ISTA_TEST}/timing_dual_csr
    ${ISTA_TEST}/timing_edit
    ${ISTA_TEST}/timing_incremental_levelization
)
target_link_libraries(ista_stage6_active_payload_microcase_test
    PRIVATE
    ${ISTA_STAGE6_ACTIVE_PAYLOAD_LINKS}
)
target_compile_features(
    ista_stage6_active_payload_microcase_test PRIVATE cxx_std_20)
target_compile_options(ista_stage6_active_payload_microcase_test
    PRIVATE
    ${ISTA_STAGE6_STRICT_OPTIONS}
)

add_test(NAME ista_stage6_active_payload_microcase_test
         COMMAND ista_stage6_active_payload_microcase_test)
set_tests_properties(ista_stage6_active_payload_microcase_test PROPERTIES
    LABELS "ista_unit;ista_cpu;ista_incremental_levelization;ista_timing_incremental_levelization;ista_transaction"
    TIMEOUT 120
)
