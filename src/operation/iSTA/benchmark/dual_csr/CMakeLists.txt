set(ISTA_DUAL_CSR_TRACE_RUNTIME_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/CpuIncrementalLevelizationRunner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTraceReplayer.cpp
)
set(ISTA_DUAL_CSR_TRACE_RUNTIME_LINKS
    ista_timing_dual_csr
    ista_timing_dual_csr_native
    ista_timing_dual_csr_transaction
    ista_buffer_trace_contract
)
set(ISTA_DUAL_CSR_TRACE_RUNTIME_PRIVATE_LINKS
    ista_interface
    ista_timing_analyzer
    ista_delay_calculator
    ista_timing_edit_native
    ista_timing_database_headers
)
set(ISTA_DUAL_CSR_TRACE_RUNTIME_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

add_library(ista_dual_csr_trace_runtime
    ${ISTA_DUAL_CSR_TRACE_RUNTIME_SOURCES}
)
target_include_directories(ista_dual_csr_trace_runtime PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(ista_dual_csr_trace_runtime
    PUBLIC ${ISTA_DUAL_CSR_TRACE_RUNTIME_LINKS}
    PRIVATE ${ISTA_DUAL_CSR_TRACE_RUNTIME_PRIVATE_LINKS}
)
target_compile_features(ista_dual_csr_trace_runtime PUBLIC cxx_std_20)
target_compile_options(ista_dual_csr_trace_runtime PRIVATE
    ${ISTA_DUAL_CSR_TRACE_RUNTIME_OPTIONS}
)

# This production runtime consumes typed ReplayProgram values. Artifact
# materialization, the guarded production loader, JSON, and differential
# comparison remain test-executable-only.

if(ISTA_ENABLE_CUDA)
  add_library(ista_cuda_targeted_trace_runtime CudaTargetedTraceReplayer.cpp)
  target_include_directories(ista_cuda_targeted_trace_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ista_cuda_targeted_trace_runtime PUBLIC ista_cuda ista_interface ista_buffer_trace_contract ista_timing_edit_native ista_timing_graph_csr_adapter ista_timing_dual_csr_native ista_timing_dual_csr_transaction
      PRIVATE ista_timing_database_headers)
  target_compile_options(ista_cuda_targeted_trace_runtime PRIVATE ${ISTA_DUAL_CSR_TRACE_RUNTIME_OPTIONS})
endif()
