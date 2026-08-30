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

  get_target_property(ACTUAL_SOURCES "${TARGET_NAME}" SOURCES)
  set(EXPECTED_SOURCES "${SOURCE_NAME}")
  ista_assert_same_inventory(
      ACTUAL_SOURCES EXPECTED_SOURCES "${TARGET_NAME} source inventory"
  )
  ista_get_target_property_or_empty(
      ACTUAL_LINKS "${TARGET_NAME}" LINK_LIBRARIES
  )
  ista_assert_same_inventory(
      ACTUAL_LINKS ${LINKS_VARIABLE} "${TARGET_NAME} direct links"
  )
  ista_get_target_property_or_empty(
      ACTUAL_OPTIONS "${TARGET_NAME}" COMPILE_OPTIONS
  )
  ista_assert_same_inventory(
      ACTUAL_OPTIONS ISTA_STEP7_STRICT_OPTIONS "${TARGET_NAME} compile options"
  )

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
    pybind11::embed
)

ista_add_step7_target(
    ista_csr_session_snapshot_adapter_contract_test
    CsrSessionSnapshotAdapterContractTest.cpp
    ISTA_STEP7_ADAPTER_LINKS
    60
)
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
target_include_directories(
    ista_csr_snapshot_binding_contract_test
    PRIVATE
    "${HOME_ISTA}/source/data_manager/adapter/csr"
    "${HOME_ISTA}/source/module/timing_csr_snapshot"
)

set(ISTA_STEP7_ADAPTER_SOURCES
    CsrSessionSnapshotAdapterContractTest.cpp
)
set(ISTA_STEP7_CONTEXT_SOURCES
    StaCsrSessionContextTest.cpp
)
set(ISTA_STEP7_LIFECYCLE_SOURCES
    StaCsrSessionLifecycleTest.cpp
)
set(ISTA_STEP7_BINDING_SOURCES
    StaCsrSnapshotBindingContractTest.cpp
)
foreach(STEP7_SOURCE_GROUP IN ITEMS ADAPTER CONTEXT LIFECYCLE BINDING)
  string(
      JOIN "|"
      ISTA_STEP7_${STEP7_SOURCE_GROUP}_SOURCES_PACKED
      ${ISTA_STEP7_${STEP7_SOURCE_GROUP}_SOURCES}
  )
endforeach()

add_test(
    NAME ista_csr_step7_source_inventory
    COMMAND
    "${CMAKE_COMMAND}"
    "-DISTA_SOURCE_ROOT=${HOME_ISTA}"
    "-DCSR_TEST_ROOT=${CMAKE_CURRENT_LIST_DIR}"
    "-DCSR_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DNINJA_EXECUTABLE=${CMAKE_MAKE_PROGRAM}"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DNM_EXECUTABLE=${CMAKE_NM}"
    "-DCSR_GENERATOR=${CMAKE_GENERATOR}"
    "-DCSR_ADAPTER_SOURCES_PACKED=${ISTA_STEP7_ADAPTER_SOURCES_PACKED}"
    "-DCSR_CONTEXT_SOURCES_PACKED=${ISTA_STEP7_CONTEXT_SOURCES_PACKED}"
    "-DCSR_LIFECYCLE_SOURCES_PACKED=${ISTA_STEP7_LIFECYCLE_SOURCES_PACKED}"
    "-DCSR_BINDING_SOURCES_PACKED=${ISTA_STEP7_BINDING_SOURCES_PACKED}"
    -P "${CMAKE_CURRENT_LIST_DIR}/CheckCsrStep7SourceInventory.cmake"
)
set_tests_properties(
    ista_csr_step7_source_inventory
    PROPERTIES LABELS "ista_unit" TIMEOUT 120
)

add_test(
    NAME ista_csr_session_controller_validation
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/csr_session/csr_session_controller.py"
    --manifest
    "${CMAKE_CURRENT_LIST_DIR}/baseline/manifest.json"
    --default-compat
    "${CMAKE_CURRENT_LIST_DIR}/projection/projection_default_compat_v1.json"
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
    "${CMAKE_CURRENT_LIST_DIR}/projection/projection_default_compat_v1.json"
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
