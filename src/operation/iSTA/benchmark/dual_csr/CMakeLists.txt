if(ISTA_ENABLE_CUDA)
  add_library(ista_cuda_targeted_trace_runtime CudaTargetedTraceReplayer.cpp)
  target_include_directories(ista_cuda_targeted_trace_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ista_cuda_targeted_trace_runtime
      PUBLIC ista_cuda ista_interface ista_buffer_trace_contract ista_timing_edit_native
          ista_timing_graph_csr_adapter ista_timing_dual_csr_native
      PRIVATE ista_timing_database_headers)
  target_compile_options(ista_cuda_targeted_trace_runtime PRIVATE
      -Wall -Wextra -Werror -pedantic -ffp-contract=off)
endif()
