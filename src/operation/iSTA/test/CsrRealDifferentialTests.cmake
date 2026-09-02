# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(ISTA_STEP8_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

set(ISTA_STEP8_COMMON_SOURCES
    real_differential/CsrRealDifferentialWire.cpp
    real_differential/CsrRealDifferentialProbeSupport.cpp
    real_differential/CsrRealDifferentialScenario.cpp
)

set(ISTA_STEP8_ORACLE_SOURCES
    real_differential/StaCsrRealOracleProbe.cpp
    real_differential/CsrRealDifferentialOracleEncoding.cpp
    ${ISTA_STEP8_COMMON_SOURCES}
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrNativeSemanticKeyTestSupport.hpp
    CsrNativeTimingOracleTestSupport.hpp
)

set(ISTA_STEP8_REFERENCE_SOURCES
    real_differential/StaCsrRealReferenceProbe.cpp
    real_differential/CsrRealDifferentialReferenceEncoding.cpp
    real_differential/CsrRealDifferentialNativeReadback.cpp
    real_differential/CsrRealDifferentialNativeNodeReadback.cpp
    real_differential/CsrRealDifferentialNativeArcReadback.cpp
    ${ISTA_STEP8_COMMON_SOURCES}
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
    CsrReferencePoisonSupport.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)

set(ISTA_STEP8_WIRE_SOURCES
    real_differential/CsrRealDifferentialWireContractTest.cpp
    real_differential/CsrRealDifferentialWire.cpp
)

set(ISTA_STEP8_PYTHON_SOURCES
    real_differential/real_differential_model.py
    real_differential/real_differential_wire_contract.py
    real_differential/real_differential_contract.py
    real_differential/real_differential_controller.py
    real_differential/real_differential_controller_selftest.py
    reference_input/reference_input_candidate_wire.py
    reference_input/reference_input_recompute_transfer.py
)
set(ISTA_STEP8_PYTHON_DEPENDENCIES)
foreach(STEP8_PYTHON_SOURCE IN LISTS ISTA_STEP8_PYTHON_SOURCES)
  list(APPEND ISTA_STEP8_PYTHON_DEPENDENCIES
       "${CMAKE_CURRENT_LIST_DIR}/${STEP8_PYTHON_SOURCE}")
endforeach()
set(ISTA_STEP8_PROBE_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_interface
    ista_source
    idm
)
set(ISTA_STEP8_WIRE_LINKS
    ista_timing_graph_csr_adapter
)

function(ista_add_step8_target TARGET_NAME SOURCES_VARIABLE LINKS_VARIABLE)
  add_executable("${TARGET_NAME}" ${${SOURCES_VARIABLE}})
  target_include_directories(
      "${TARGET_NAME}"
      PRIVATE
      "${CMAKE_CURRENT_LIST_DIR}"
      "${CMAKE_CURRENT_LIST_DIR}/real_differential"
  )
  target_link_libraries("${TARGET_NAME}" PRIVATE ${${LINKS_VARIABLE}})
  target_compile_options("${TARGET_NAME}" PRIVATE ${ISTA_STEP8_STRICT_OPTIONS})
endfunction()

ista_add_step8_target(
    ista_csr_real_oracle_probe
    ISTA_STEP8_ORACLE_SOURCES
    ISTA_STEP8_PROBE_LINKS
)
ista_add_step8_target(
    ista_csr_real_reference_probe
    ISTA_STEP8_REFERENCE_SOURCES
    ISTA_STEP8_PROBE_LINKS
)
ista_add_step8_target(
    ista_csr_real_differential_wire_contract_test
    ISTA_STEP8_WIRE_SOURCES
    ISTA_STEP8_WIRE_LINKS
)

add_test(
    NAME ista_csr_real_differential_wire_contract_test
    COMMAND ista_csr_real_differential_wire_contract_test
)
set_tests_properties(
    ista_csr_real_differential_wire_contract_test
    PROPERTIES LABELS "ista_unit" TIMEOUT 60
)

add_test(
    NAME ista_csr_real_differential_controller_validation
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/real_differential/real_differential_controller.py"
    --self-test
)
set_tests_properties(
    ista_csr_real_differential_controller_validation
    PROPERTIES
    LABELS "ista_unit"
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    REQUIRED_FILES "${ISTA_STEP8_PYTHON_DEPENDENCIES}"
)

add_test(
    NAME ista_csr_real_differential_fresh_process
    COMMAND
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/real_differential/real_differential_controller.py"
    --oracle-probe
    "$<TARGET_FILE:ista_csr_real_oracle_probe>"
    --reference-probe
    "$<TARGET_FILE:ista_csr_real_reference_probe>"
    --manifest
    "${CMAKE_CURRENT_LIST_DIR}/real_differential/manifest.json"
    --baseline-manifest
    "${CMAKE_CURRENT_LIST_DIR}/baseline/manifest.json"
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/csr-real-differential-runs"
    --repeats
    3
    --timeout-seconds
    120
    --public-negative-self-test
)
set_tests_properties(
    ista_csr_real_differential_fresh_process
    PROPERTIES
    LABELS "ista_smoke"
    TIMEOUT 2400
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    REQUIRED_FILES "${ISTA_STEP8_PYTHON_DEPENDENCIES}"
)
