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

# Functional replay carries no selector receipt; the artifact entry remains strict.
if(ECC_INCREGPUSTA_DEBUG_CHECKS AND ISTA_ENABLE_CUDA)
  add_test(NAME ista_cuda_replay_program_input
      COMMAND "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/timing_edit/test_replay_program_input.py"
      --probe "$<TARGET_FILE:ista_cuda_targeted_trace_probe>"
      --controller "${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr/cuda_targeted_trace_controller.py"
      --trace "${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/synthetic_two_batch.json"
      --run-root "${CMAKE_CURRENT_BINARY_DIR}/cuda-replay-input")
  set_tests_properties(ista_cuda_replay_program_input PROPERTIES
      LABELS "ista_integration;ista_cuda;ista_diagnostic"
      TIMEOUT 360 ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
