# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(ISTA_CSR_REFERENCE_PUBLIC_HEADER_SOURCES
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

add_executable(
    ista_csr_reference_public_headers_test
    ${ISTA_CSR_REFERENCE_PUBLIC_HEADER_SOURCES}
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

get_target_property(
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_ACTUAL_SOURCES
    ista_csr_reference_public_headers_test
    SOURCES
)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_ACTUAL_SOURCES
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_SOURCES
    "CSR reference public-header source inventory"
)
ista_get_target_property_or_empty(
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_LINKS
    ista_csr_reference_public_headers_test
    LINK_LIBRARIES
)
set(
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_EXPECTED_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_LINKS
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_EXPECTED_LINKS
    "CSR reference public-header direct links"
)
ista_get_target_property_or_empty(
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_OPTIONS
    ista_csr_reference_public_headers_test
    COMPILE_OPTIONS
)
set(
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_EXPECTED_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_OPTIONS
    ISTA_CSR_REFERENCE_PUBLIC_HEADER_EXPECTED_OPTIONS
    "CSR reference public-header compile options"
)

add_test(
    NAME ista_csr_reference_input_controller_validation
    COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/reference_input_contract.py"
    --probe-schema
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/schema/reference_input_result_v1.schema.json"
    --controller-schema
    "${CMAKE_CURRENT_LIST_DIR}/reference_input/schema/reference_input_controller_v1.schema.json"
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
set(ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_SOURCES
    CsrReferenceFingerprintComponentsTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
)
add_executable(
    ista_csr_reference_fingerprint_components_test
    ${ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_SOURCES}
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
get_target_property(
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_ACTUAL_SOURCES
    ista_csr_reference_fingerprint_components_test
    SOURCES
)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_ACTUAL_SOURCES
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_SOURCES
    "CSR reference fingerprint component source inventory"
)
ista_get_target_property_or_empty(
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_LINKS
    ista_csr_reference_fingerprint_components_test
    LINK_LIBRARIES
)
set(ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_EXPECTED_LINKS ista_timing_csr_core)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_LINKS
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_EXPECTED_LINKS
    "CSR reference fingerprint component direct links"
)
ista_get_target_property_or_empty(
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_OPTIONS
    ista_csr_reference_fingerprint_components_test
    COMPILE_OPTIONS
)
set(ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_EXPECTED_OPTIONS)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_OPTIONS
    ISTA_CSR_REFERENCE_FINGERPRINT_COMPONENT_EXPECTED_OPTIONS
    "CSR reference fingerprint component compile options"
)
function(ista_add_reference_functional TARGET_NAME SOURCES_VARIABLE)
  add_executable("${TARGET_NAME}" ${${SOURCES_VARIABLE}})
  target_include_directories("${TARGET_NAME}" PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries("${TARGET_NAME}" PRIVATE ${ARGN})
  add_test(NAME "${TARGET_NAME}" COMMAND "${TARGET_NAME}")
  set_tests_properties("${TARGET_NAME}" PROPERTIES LABELS "ista_unit" TIMEOUT 60)

  get_target_property(ACTUAL_SOURCES "${TARGET_NAME}" SOURCES)
  ista_assert_same_inventory(
      ACTUAL_SOURCES ${SOURCES_VARIABLE}
      "${TARGET_NAME} source inventory"
  )
  ista_get_target_property_or_empty(ACTUAL_LINKS "${TARGET_NAME}" LINK_LIBRARIES)
  set(EXPECTED_LINKS ${ARGN})
  ista_assert_same_inventory(ACTUAL_LINKS EXPECTED_LINKS "${TARGET_NAME} direct links")
  ista_get_target_property_or_empty(ACTUAL_OPTIONS "${TARGET_NAME}" COMPILE_OPTIONS)
  set(EXPECTED_OPTIONS)
  ista_assert_same_inventory(ACTUAL_OPTIONS EXPECTED_OPTIONS "${TARGET_NAME} compile options")
endfunction()

set(ISTA_CSR_REFERENCE_INPUT_EXPORT_CONTRACT_SOURCES
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
    ista_csr_reference_input_export_contract_test
    ISTA_CSR_REFERENCE_INPUT_EXPORT_CONTRACT_SOURCES
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)

set(ISTA_CSR_REFERENCE_TRANSFER_CONTRACT_SOURCES
    CsrReferenceTransferContractTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)
ista_add_reference_functional(
    ista_csr_reference_transfer_contract_test
    ISTA_CSR_REFERENCE_TRANSFER_CONTRACT_SOURCES
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)

set(ISTA_CSR_REFERENCE_SUPPORT_CONTRACT_SOURCES
    CsrReferenceSupportContractTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)
ista_add_reference_functional(
    ista_csr_reference_support_contract_test
    ISTA_CSR_REFERENCE_SUPPORT_CONTRACT_SOURCES
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)

set(ISTA_CSR_REFERENCE_INPUT_PROPERTY_SOURCES
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
add_executable(
    ista_csr_reference_input_property_test
    ${ISTA_CSR_REFERENCE_INPUT_PROPERTY_SOURCES}
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
get_target_property(
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_ACTUAL_SOURCES
    ista_csr_reference_input_property_test
    SOURCES
)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_ACTUAL_SOURCES
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_SOURCES
    "CSR reference property source inventory"
)
ista_get_target_property_or_empty(
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_LINKS
    ista_csr_reference_input_property_test
    LINK_LIBRARIES
)
set(
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_EXPECTED_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_LINKS
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_EXPECTED_LINKS
    "CSR reference property direct links"
)
ista_get_target_property_or_empty(
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_OPTIONS
    ista_csr_reference_input_property_test
    COMPILE_OPTIONS
)
set(ISTA_CSR_REFERENCE_INPUT_PROPERTY_EXPECTED_OPTIONS)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_OPTIONS
    ISTA_CSR_REFERENCE_INPUT_PROPERTY_EXPECTED_OPTIONS
    "CSR reference property compile options"
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

set(ISTA_CSR_REFERENCE_INPUT_FAILURE_SOURCES
    CsrReferenceInputFailureTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
)
ista_add_reference_functional(
    ista_csr_reference_input_failure_test
    ISTA_CSR_REFERENCE_INPUT_FAILURE_SOURCES
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)

set(ISTA_CSR_REFERENCE_POISON_ISOLATION_SOURCES
    CsrReferencePoisonIsolationTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferencePoisonSupport.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)
ista_add_reference_functional(
    ista_csr_reference_poison_isolation_test
    ISTA_CSR_REFERENCE_POISON_ISOLATION_SOURCES
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)

set(ISTA_CSR_REFERENCE_INPUT_PROBE_SOURCES
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
add_executable(
    ista_csr_reference_input_probe
    ${ISTA_CSR_REFERENCE_INPUT_PROBE_SOURCES}
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
get_target_property(
    ISTA_CSR_REFERENCE_INPUT_PROBE_ACTUAL_SOURCES
    ista_csr_reference_input_probe
    SOURCES
)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_INPUT_PROBE_ACTUAL_SOURCES
    ISTA_CSR_REFERENCE_INPUT_PROBE_SOURCES
    "CSR reference-input probe source inventory"
)
ista_get_target_property_or_empty(
    ISTA_CSR_REFERENCE_INPUT_PROBE_LINKS
    ista_csr_reference_input_probe
    LINK_LIBRARIES
)
set(
    ISTA_CSR_REFERENCE_INPUT_PROBE_EXPECTED_LINKS
    ista_timing_graph_csr_adapter
    ista_interface
    ista_source
    idm
)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_INPUT_PROBE_LINKS
    ISTA_CSR_REFERENCE_INPUT_PROBE_EXPECTED_LINKS
    "CSR reference-input probe direct links"
)
ista_get_target_property_or_empty(
    ISTA_CSR_REFERENCE_INPUT_PROBE_OPTIONS
    ista_csr_reference_input_probe
    COMPILE_OPTIONS
)
set(ISTA_CSR_REFERENCE_INPUT_PROBE_EXPECTED_OPTIONS)
ista_assert_same_inventory(
    ISTA_CSR_REFERENCE_INPUT_PROBE_OPTIONS
    ISTA_CSR_REFERENCE_INPUT_PROBE_EXPECTED_OPTIONS
    "CSR reference-input probe compile options"
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
