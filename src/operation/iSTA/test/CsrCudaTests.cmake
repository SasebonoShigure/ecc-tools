# Real-device contracts. CUDA explicitly enabled means device failures are
# failures, not CTest skips. The CPU build never creates this target.
if(ISTA_ENABLE_CUDA)
  add_executable(ista_csr_cuda_contract_test
      CsrCudaBackendContractTest.cpp
      CsrCpuReferenceContractTest.cpp
      CsrCpuReferenceOutcomeContract.cpp
      CsrCpuReferenceArcContract.cpp
      CsrCpuReferencePropagationContract.cpp
      CsrCpuReferenceTopologyContract.cpp
      CsrCpuReferenceDagFixture.cpp
      CsrCpuReferenceTestFixture.cpp)
  target_link_libraries(ista_csr_cuda_contract_test PRIVATE ista_cuda)
  target_compile_options(ista_csr_cuda_contract_test PRIVATE
      -Wall -Wextra -Werror -pedantic -ffp-contract=off)
  add_test(NAME ista_csr_cuda_contract_test COMMAND ista_csr_cuda_contract_test)
  add_test(NAME ista_csr_cuda_packing_test COMMAND ista_csr_cuda_contract_test --packing-only)
  set_tests_properties(ista_csr_cuda_packing_test PROPERTIES LABELS "ista_cuda_host" TIMEOUT 180)
  set_tests_properties(ista_csr_cuda_contract_test PROPERTIES LABELS "ista_cuda" TIMEOUT 180)
endif()

# Independent CPU table oracle is a development diagnostic, never a benchmark.
if(ISTA_ENABLE_CUDA AND ECC_INCREGPUSTA_DEBUG_CHECKS)
  add_executable(ista_csr_cuda_table_contract_test CsrCudaTableContractTest.cu)
  target_link_libraries(ista_csr_cuda_table_contract_test PRIVATE ista_cuda CUDA::cudart)
  set_target_properties(ista_csr_cuda_table_contract_test PROPERTIES
      CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Shared)
  target_compile_options(ista_csr_cuda_table_contract_test PRIVATE
      --fmad=false -Xcompiler=-Wall,-Wextra,-Werror,-ffp-contract=off)
  add_test(NAME ista_csr_cuda_table_contract_test COMMAND ista_csr_cuda_table_contract_test)
  set_tests_properties(ista_csr_cuda_table_contract_test PROPERTIES LABELS "ista_cuda" TIMEOUT 180)
endif()

if(ISTA_ENABLE_CUDA AND ECC_INCREGPUSTA_DEBUG_CHECKS)
  add_test(NAME ista_cuda_native_baseline
      COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/baseline/run_cuda_baseline.py"
      --probe "$<TARGET_FILE:ista_full_baseline_probe>"
      --output-root "${CMAKE_CURRENT_BINARY_DIR}/cuda-baseline-runs")
  set_tests_properties(ista_cuda_native_baseline PROPERTIES
      LABELS "ista_cuda;ista_integration;ista_diagnostic" TIMEOUT 300
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

  add_test(NAME ista_cuda_propagated_clock_trace
      COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/baseline/run_propagated_trace.py"
      --oracle "$<TARGET_FILE:ista_timing_edit_independent_oracle_probe>"
      --cpu-probe "$<TARGET_FILE:ista_stage6_production_fresh_probe>"
      --cuda-full-probe "$<TARGET_FILE:ista_cuda_full_trace_probe>"
      --cuda-incremental-probe "$<TARGET_FILE:ista_cuda_targeted_trace_probe>"
      --run-root "${CMAKE_CURRENT_BINARY_DIR}/propagated-clock-runs")
  set_tests_properties(ista_cuda_propagated_clock_trace PROPERTIES
      LABELS "ista_cuda;ista_integration;ista_diagnostic" TIMEOUT 1500
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()

if(ISTA_ENABLE_CUDA)
  add_executable(ista_cuda_dual_csr_session_test CudaDualCsrSessionTest.cpp CudaDualCsrStorageContract.cpp CudaParallelPatchContract.cpp CsrCpuReferenceTestFixture.cpp CsrCpuReferenceDagFixture.cpp
      CudaDualCsrTestReader.cu CsrCudaSequentialContract.cu CsrCudaRcPropagationTest.cpp timing_edit/BufferTraceCsrJson.cpp)
  target_sources(ista_cuda_dual_csr_session_test PRIVATE AllocationFailureSupport.cpp)
  target_include_directories(ista_cuda_dual_csr_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ista_cuda_dual_csr_session_test PRIVATE ista_cuda CUDA::cudart)
  set_target_properties(ista_cuda_dual_csr_session_test PROPERTIES
      CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Shared)
  target_compile_options(ista_cuda_dual_csr_session_test PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-pedantic;-ffp-contract=off>
      $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false;-Xcompiler=-Wall,-Wextra,-Werror,-ffp-contract=off>)
  add_test(NAME ista_cuda_dual_csr_session_test COMMAND ista_cuda_dual_csr_session_test)
  add_test(NAME ista_cuda_mixed_session_test COMMAND ista_cuda_dual_csr_session_test --mixed)
  set_tests_properties(ista_cuda_mixed_session_test PROPERTIES LABELS "ista_cuda;ista_cuda_mixed" TIMEOUT 180)
  set_tests_properties(ista_cuda_dual_csr_session_test PROPERTIES LABELS "ista_cuda;ista_cuda_targeted" TIMEOUT 180)
endif()

if(ISTA_ENABLE_CUDA AND ECC_INCREGPUSTA_DEBUG_CHECKS)
  add_executable(ista_cuda_dual_csr_hardware_fault_test CudaDualCsrHardwareFaultTest.cu CsrCpuReferenceTestFixture.cpp)
  target_include_directories(ista_cuda_dual_csr_hardware_fault_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ista_cuda_dual_csr_hardware_fault_test PRIVATE ista_cuda CUDA::cudart)
  set_target_properties(ista_cuda_dual_csr_hardware_fault_test PROPERTIES
      CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Shared)
  target_compile_options(ista_cuda_dual_csr_hardware_fault_test PRIVATE
      $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-pedantic;-ffp-contract=off>
      $<$<COMPILE_LANGUAGE:CUDA>:--fmad=false;-Xcompiler=-Wall,-Wextra,-Werror,-ffp-contract=off>)
  foreach(mode allocation-failure debug-context-fault stage-context-fault)
    add_test(NAME ista_cuda_dual_csr_${mode} COMMAND ista_cuda_dual_csr_hardware_fault_test --${mode})
    set_tests_properties(ista_cuda_dual_csr_${mode} PROPERTIES LABELS "ista_cuda;ista_cuda_targeted" TIMEOUT 180)
    if(NOT mode STREQUAL "allocation-failure")
      add_test(NAME ista_cuda_mixed_${mode} COMMAND ista_cuda_dual_csr_hardware_fault_test --${mode} --mixed)
      set_tests_properties(ista_cuda_mixed_${mode} PROPERTIES LABELS "ista_cuda;ista_cuda_mixed" TIMEOUT 180)
    endif()
  endforeach()
endif()

if(ISTA_ENABLE_CUDA)
  add_executable(ista_cuda_incremental_fallback_test CudaIncrementalFallbackTest.cpp CsrCpuReferenceTestFixture.cpp)
  target_include_directories(ista_cuda_incremental_fallback_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(ista_cuda_incremental_fallback_test PRIVATE ista_cuda_targeted_trace_runtime)
  target_compile_options(ista_cuda_incremental_fallback_test PRIVATE -Wall -Wextra -Werror -pedantic -ffp-contract=off)
  add_test(NAME ista_cuda_incremental_fallback_test COMMAND ista_cuda_incremental_fallback_test)
  set_tests_properties(ista_cuda_incremental_fallback_test PROPERTIES LABELS "ista_cuda;ista_cuda_mixed" TIMEOUT 180)
endif()


if(ISTA_ENABLE_CUDA)
  add_executable(ista_csr_cuda_parasitic_model_test CsrCudaParasiticModelTest.cu CsrCudaParasiticPackingTest.cu)
  target_link_libraries(ista_csr_cuda_parasitic_model_test PRIVATE ista_cuda CUDA::cudart)
  set_target_properties(ista_csr_cuda_parasitic_model_test PROPERTIES
      CUDA_STANDARD 20 CUDA_STANDARD_REQUIRED ON CUDA_RUNTIME_LIBRARY Shared)
  target_compile_options(ista_csr_cuda_parasitic_model_test PRIVATE
      --fmad=false -Xcompiler=-Wall,-Wextra,-Werror,-ffp-contract=off)
  add_test(NAME ista_csr_cuda_parasitic_model_test COMMAND ista_csr_cuda_parasitic_model_test)
  set_tests_properties(ista_csr_cuda_parasitic_model_test PROPERTIES LABELS "ista_cuda" TIMEOUT 180)
endif()
