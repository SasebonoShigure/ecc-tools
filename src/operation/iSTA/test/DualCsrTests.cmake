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

add_test(NAME ista_incregpusta_performance_orchestration
    COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/test_performance_mode.py")
set_tests_properties(ista_incregpusta_performance_orchestration PROPERTIES
    LABELS "ista_unit;ista_dual_csr" TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

foreach(stage IN ITEMS dual_csr stage6)
  set(probe "ista_${stage}_production_fresh_probe")
  if(ECC_INCREGPUSTA_DEBUG_CHECKS)
    set(test_name "ista_${stage}_performance_rejects_on")
    add_test(NAME "${test_name}"
        COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/test_performance_mode.py"
        --reject-probe "$<TARGET_FILE:${probe}>")
  else()
    set(test_name "ista_${stage}_release_timing_smoke")
    add_test(NAME "${test_name}"
        COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
        "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/dual_csr_timing_smoke.py"
        --probe "$<TARGET_FILE:${probe}>"
        --trace "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/synthetic_two_batch.json"
        --run-root "${CMAKE_CURRENT_BINARY_DIR}/${stage}-timing-smoke/runs"
        --output "${CMAKE_CURRENT_BINARY_DIR}/${stage}-timing-smoke/timing-smoke.json"
        --warmup-runs 1 --samples 6 --timeout-seconds 900)
  endif()
  set_tests_properties("${test_name}" PROPERTIES
      LABELS "ista_smoke;ista_cpu;ista_dual_csr" TIMEOUT 7200
      SKIP_RETURN_CODE 77 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endforeach()

if(ISTA_ENABLE_CUDA)
  # Explicit acceptance output can inspect OFF/ON state without enabling
  # diagnostics in production or ordinary performance runs.
  target_sources(ista_cuda_dual_csr_session_test PRIVATE
      timing_dual_csr/DualCsrTargetedArtifactSupport.cpp
      timing_edit/oracle/ProductionFreshSessionLoader.cpp
      timing_edit/StaTimingEditSessionProbeAccess.cpp)
  target_include_directories(ista_cuda_dual_csr_session_test PRIVATE
      ${CMAKE_CURRENT_LIST_DIR}/timing_edit
      ${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle)
  target_link_libraries(ista_cuda_dual_csr_session_test PRIVATE
      ista_cuda_targeted_trace_runtime ${ISTA_TIMING_EDIT_PRODUCTION_ORACLE_LINKS})
  set(ISTA_CUDA_TRACE_PROBE_SOURCES ${ISTA_DUAL_CSR_FRESH_PROBE_SOURCES})
  list(REMOVE_ITEM ISTA_CUDA_TRACE_PROBE_SOURCES timing_dual_csr/DualCsrTraceProductionFreshProbe.cpp)
  add_executable(ista_cuda_full_trace_probe CudaFullTraceProbe.cpp ${ISTA_CUDA_TRACE_PROBE_SOURCES})
  target_include_directories(ista_cuda_full_trace_probe PRIVATE
      ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/timing_edit
      ${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle ${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr)
  target_link_libraries(ista_cuda_full_trace_probe PRIVATE ista_cuda_full_trace_runtime ${ISTA_DUAL_CSR_FRESH_PROBE_LINKS})
  target_compile_options(ista_cuda_full_trace_probe PRIVATE ${ISTA_DUAL_CSR_TRACE_STRICT_OPTIONS})
endif()

if(ISTA_ENABLE_CUDA)
  foreach(trace IN ITEMS synthetic_two_batch real_buffer_fanout)
    if(ECC_INCREGPUSTA_DEBUG_CHECKS)
      set(cuda_trace_mode)
    else()
      set(cuda_trace_mode --performance --warmup-runs 1 --samples 3)
    endif()
    add_test(NAME ista_cuda_full_trace_${trace}
      COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/cuda_full_trace_controller.py"
      --probe "$<TARGET_FILE:ista_cuda_full_trace_probe>"
      --trace "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/${trace}.json"
      --run-root "${CMAKE_CURRENT_BINARY_DIR}/cuda-full-traces/${trace}"
      ${cuda_trace_mode})
    set_tests_properties(ista_cuda_full_trace_${trace} PROPERTIES
      LABELS "ista_cuda;ista_integration" TIMEOUT 2400 SKIP_RETURN_CODE 77
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  endforeach()
endif()

if(ISTA_ENABLE_CUDA)
  add_executable(ista_cuda_targeted_trace_probe CudaTargetedTraceProbe.cpp
      timing_dual_csr/CudaTargetedTimingProjection.cpp
      timing_dual_csr/CudaTargetedDeviceComparison.cpp ${ISTA_CUDA_TRACE_PROBE_SOURCES})
  target_include_directories(ista_cuda_targeted_trace_probe PRIVATE
      ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/timing_edit
      ${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle ${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr)
  target_link_libraries(ista_cuda_targeted_trace_probe PRIVATE ista_cuda_targeted_trace_runtime ${ISTA_DUAL_CSR_FRESH_PROBE_LINKS})
  target_compile_options(ista_cuda_targeted_trace_probe PRIVATE ${ISTA_DUAL_CSR_TRACE_STRICT_OPTIONS})
  foreach(trace IN ITEMS synthetic_two_batch real_buffer_fanout)
    if(ECC_INCREGPUSTA_DEBUG_CHECKS)
      set(cuda_targeted_mode --full-probe "$<TARGET_FILE:ista_cuda_full_trace_probe>")
    else()
      set(cuda_targeted_mode --performance --warmup-runs 1 --samples 3)
    endif()
    add_test(NAME ista_cuda_targeted_trace_${trace}
      COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/cuda_targeted_trace_controller.py"
      --probe "$<TARGET_FILE:ista_cuda_targeted_trace_probe>"
      --trace "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/${trace}.json"
      --run-root "${CMAKE_CURRENT_BINARY_DIR}/cuda-targeted-traces/${trace}"
      ${cuda_targeted_mode})
    set_tests_properties(ista_cuda_targeted_trace_${trace} PROPERTIES
      LABELS "ista_cuda;ista_cuda_targeted;ista_integration" TIMEOUT 2400 SKIP_RETURN_CODE 77
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    add_test(NAME ista_cuda_incremental_trace_${trace}
      COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/cuda_targeted_trace_controller.py"
      --strategy incremental
      --probe "$<TARGET_FILE:ista_cuda_targeted_trace_probe>"
      --trace "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/${trace}.json"
      --run-root "${CMAKE_CURRENT_BINARY_DIR}/cuda-incremental-traces/${trace}"
      ${cuda_targeted_mode})
    set_tests_properties(ista_cuda_incremental_trace_${trace} PROPERTIES
      LABELS "ista_cuda;ista_cuda_mixed;ista_integration" TIMEOUT 2400 SKIP_RETURN_CODE 77
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
  endforeach()
endif()
