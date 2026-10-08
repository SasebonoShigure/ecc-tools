# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

add_executable(
    ista_csr_reference_public_headers_test
    CsrReferencePublicHeadersMain.cpp
    public_header/CsrReferencePrimitivesHeaderTest.cpp
    public_header/CsrReferenceNativeModelsHeaderTest.cpp
    public_header/CsrReferenceTypesHeaderTest.cpp
    public_header/CsrReferenceCanonicalEncoderHeaderTest.cpp
    public_header/CsrReferenceFingerprintPayloadHeaderTest.cpp
    public_header/CsrFingerprintHeaderTest.cpp
    public_header/CsrReferenceValidatorHeaderTest.cpp
    public_header/TimingGraphCsrAdapterHeaderTest.cpp
    CsrReferenceHeaderIncludeOrderTest.cpp
    CsrReferenceInputRawProjectionHeaderTest.cpp
)
target_include_directories(
    ista_csr_reference_public_headers_test
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
)
target_link_libraries(
    ista_csr_reference_public_headers_test
    PRIVATE
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
target_compile_options(
    ista_csr_reference_public_headers_test
    PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
)
add_test(
    NAME ista_csr_reference_public_headers_test
    COMMAND ista_csr_reference_public_headers_test
)
set_tests_properties(
    ista_csr_reference_public_headers_test
    PROPERTIES
    LABELS "ista_unit"
    TIMEOUT 60
)

add_test(
    NAME ista_csr_reference_input_controller_validation
    COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/reference_input_contract.py"
    --probe-schema
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/schema/reference_input_result_v2.schema.json"
    --controller-schema
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/schema/reference_input_controller_v2.schema.json"
    --self-test
)
set_tests_properties(
    ista_csr_reference_input_controller_validation
    PROPERTIES
    LABELS "ista_unit"
    TIMEOUT 60
)

add_test(
    NAME ista_csr_reference_input_controller_selftest
    COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/reference_input_controller_selftest.py"
    --controller
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/reference_input_controller.py"
    --manifest
    "${CMAKE_CURRENT_LIST_DIR}/baseline/manifest.json"
)
set_tests_properties(
    ista_csr_reference_input_controller_selftest
    PROPERTIES
    LABELS "ista_unit"
    TIMEOUT 60
)
add_executable(
    ista_csr_reference_fingerprint_components_test
    CsrReferenceFingerprintComponentsTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
)
target_include_directories(
    ista_csr_reference_fingerprint_components_test
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
)
target_link_libraries(
    ista_csr_reference_fingerprint_components_test
    PRIVATE
    ista_timing_csr_core
)
add_test(
    NAME ista_csr_reference_fingerprint_components_test
    COMMAND ista_csr_reference_fingerprint_components_test
)
set_tests_properties(
    ista_csr_reference_fingerprint_components_test
    PROPERTIES
    LABELS "ista_unit"
    TIMEOUT 60
)
function(ista_add_reference_functional TARGET_NAME)
  add_executable("${TARGET_NAME}" ${ARGN})
  target_include_directories("${TARGET_NAME}" PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries(
      "${TARGET_NAME}"
      PRIVATE
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
  )
  add_test(NAME "${TARGET_NAME}" COMMAND "${TARGET_NAME}")
  set_tests_properties("${TARGET_NAME}" PROPERTIES LABELS "ista_unit" TIMEOUT 60)
endfunction()

ista_add_reference_functional(
    ista_csr_reference_input_export_contract_test
    CsrReferenceInputExportContractTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)

ista_add_reference_functional(
    ista_csr_reference_transfer_contract_test
    CsrReferenceTransferContractTest.cpp
    CsrReferenceParasiticBindingTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)

ista_add_reference_functional(
    ista_csr_reference_support_contract_test
    CsrReferenceSupportContractTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)

add_executable(
    ista_csr_reference_input_property_test
    CsrReferenceInputPropertyTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)
target_include_directories(
    ista_csr_reference_input_property_test
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
)
target_link_libraries(
    ista_csr_reference_input_property_test
    PRIVATE
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
set(ISTA_CSR_REFERENCE_INPUT_PROPERTY_SEEDS 1 7 42 20260820 0xdeadbeef)
foreach(PROPERTY_SEED IN LISTS ISTA_CSR_REFERENCE_INPUT_PROPERTY_SEEDS)
  add_test(
      NAME "ista_csr_reference_input_property_seed_${PROPERTY_SEED}"
      COMMAND ista_csr_reference_input_property_test --seed "${PROPERTY_SEED}"
  )
  set_tests_properties(
      "ista_csr_reference_input_property_seed_${PROPERTY_SEED}"
      PROPERTIES
      LABELS "ista_property"
      TIMEOUT 60
  )
endforeach()

ista_add_reference_functional(
    ista_csr_reference_input_failure_test
    CsrReferenceInputFailureTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
)

ista_add_reference_functional(
    ista_csr_reference_poison_isolation_test
    CsrReferencePoisonIsolationTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferencePoisonSupport.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)

add_executable(
    ista_csr_reference_input_probe
    reference_input/StaCsrReferenceInputProbe.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferencePoisonSupport.cpp
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)
target_include_directories(
    ista_csr_reference_input_probe
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
)
target_link_libraries(
    ista_csr_reference_input_probe
    PRIVATE
    ista_timing_graph_csr_adapter
    ista_interface
    ista_source
    idm
)
add_test(
    NAME ista_csr_reference_input_native_overlap
    COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/reference_input_overlap_controller.py"
    --probe
    "$<TARGET_FILE:ista_csr_reference_input_probe>"
)
set_tests_properties(
    ista_csr_reference_input_native_overlap
    PROPERTIES
    LABELS "ista_unit"
    TIMEOUT 180
)

add_test(
    NAME ista_csr_reference_input_fresh_process
    COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/reference_input_controller.py"
    --manifest
    "${CMAKE_CURRENT_LIST_DIR}/baseline/manifest.json"
    --probe
    "$<TARGET_FILE:ista_csr_reference_input_probe>"
    --run-root
    "${CMAKE_CURRENT_BINARY_DIR}/reference_input_runs"
    --repeats
    3
    --timeout-seconds
    120
)
set_tests_properties(
    ista_csr_reference_input_fresh_process
    PROPERTIES
    LABELS "ista_smoke"
    TIMEOUT 300
)
