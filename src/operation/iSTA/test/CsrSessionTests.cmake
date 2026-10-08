# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

find_package(Threads REQUIRED)

set(ISTA_STEP7_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

function(ista_add_step7_target TARGET_NAME SOURCE_NAME LINKS_VARIABLE TIMEOUT_SECONDS)
  add_executable("${TARGET_NAME}" "${SOURCE_NAME}")
  target_include_directories("${TARGET_NAME}" PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries("${TARGET_NAME}" PRIVATE ${${LINKS_VARIABLE}})
  target_compile_options("${TARGET_NAME}" PRIVATE ${ISTA_STEP7_STRICT_OPTIONS})

  add_test(NAME "${TARGET_NAME}" COMMAND "${TARGET_NAME}")
  set_tests_properties(
      "${TARGET_NAME}"
      PROPERTIES
      LABELS "ista_unit"
      TIMEOUT "${TIMEOUT_SECONDS}"
  )
endfunction()

set(ISTA_STEP7_ADAPTER_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
set(ISTA_STEP7_CONTEXT_LINKS
    ista_interface
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
set(ISTA_STEP7_LIFECYCLE_LINKS
    ista_interface
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    Threads::Threads
)
set(ISTA_STEP7_BINDING_LINKS
    py_ista
    ista_timing_csr_snapshot_contract
    pybind11::embed
    ista_timing_database_headers
)

ista_add_step7_target(
    ista_csr_session_snapshot_adapter_contract_test
    CsrSessionSnapshotAdapterContractTest.cpp
    ISTA_STEP7_ADAPTER_LINKS
    60
)
target_sources(ista_csr_session_snapshot_adapter_contract_test PRIVATE AllocationFailureSupport.cpp)
ista_add_step7_target(
    ista_csr_session_context_test
    StaCsrSessionContextTest.cpp
    ISTA_STEP7_CONTEXT_LINKS
    60
)
ista_add_step7_target(
    ista_csr_session_lifecycle_test
    StaCsrSessionLifecycleTest.cpp
    ISTA_STEP7_LIFECYCLE_LINKS
    120
)
ista_add_step7_target(
    ista_csr_snapshot_binding_contract_test
    StaCsrSnapshotBindingContractTest.cpp
    ISTA_STEP7_BINDING_LINKS
    120
)

add_test(
    NAME ista_csr_session_controller_validation
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/csr_session/csr_session_controller.py"
    --manifest
    "${CMAKE_CURRENT_LIST_DIR}/baseline/manifest.json"
    --default-compat
    "${CMAKE_CURRENT_LIST_DIR}/projection/projection_default_compat_v2.json"
    --self-test
)
set_tests_properties(
    ista_csr_session_controller_validation
    PROPERTIES
    LABELS "ista_unit"
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME ista_csr_session_fresh_process
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/csr_session/csr_session_controller.py"
    --probe
    "$<TARGET_FILE:ista_csr_projection_probe>"
    --manifest
    "${CMAKE_CURRENT_LIST_DIR}/baseline/manifest.json"
    --default-compat
    "${CMAKE_CURRENT_LIST_DIR}/projection/projection_default_compat_v2.json"
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/csr-session-runs"
    --repeats
    3
    --timeout-seconds
    75
)
set_tests_properties(
    ista_csr_session_fresh_process
    PROPERTIES
    LABELS "ista_smoke"
    TIMEOUT 1050
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
)
