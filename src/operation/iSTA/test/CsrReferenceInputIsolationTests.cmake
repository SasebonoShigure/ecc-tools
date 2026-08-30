# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

function(ista_assert_step4_target TARGET_NAME SOURCES_VARIABLE LINKS_VARIABLE OPTIONS_VARIABLE)
  if(NOT TARGET "${TARGET_NAME}")
    message(FATAL_ERROR "missing frozen Step 4 target: ${TARGET_NAME}")
  endif()
  get_target_property(ACTUAL_SOURCES "${TARGET_NAME}" SOURCES)
  ista_assert_same_inventory(ACTUAL_SOURCES ${SOURCES_VARIABLE} "${TARGET_NAME} independent source inventory")
  ista_get_target_property_or_empty(ACTUAL_LINKS "${TARGET_NAME}" LINK_LIBRARIES)
  ista_assert_same_inventory(ACTUAL_LINKS ${LINKS_VARIABLE} "${TARGET_NAME} independent direct links")
  ista_get_target_property_or_empty(ACTUAL_OPTIONS "${TARGET_NAME}" COMPILE_OPTIONS)
  ista_assert_same_inventory(ACTUAL_OPTIONS ${OPTIONS_VARIABLE} "${TARGET_NAME} independent compile options")
endfunction()

set(ISTA_STEP4_EMPTY)
set(ISTA_STEP4_ADAPTER_LINKS ista_timing_graph_csr_adapter ista_timing_database_headers)
set(ISTA_STEP4_CORE_LINKS ista_timing_csr_core)
set(ISTA_STEP4_PROBE_LINKS ista_timing_graph_csr_adapter ista_interface ista_source idm)
set(ISTA_STEP4_STRICT_OPTIONS -Wall -Wextra -Werror -pedantic)

set(ISTA_STEP4_PUBLIC_HEADER_SOURCES
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
ista_assert_step4_target(
    ista_csr_reference_public_headers_test
    ISTA_STEP4_PUBLIC_HEADER_SOURCES
    ISTA_STEP4_ADAPTER_LINKS
    ISTA_STEP4_STRICT_OPTIONS
)

set(ISTA_STEP4_EXPORT_SOURCES
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
ista_assert_step4_target(
    ista_csr_reference_input_export_contract_test
    ISTA_STEP4_EXPORT_SOURCES
    ISTA_STEP4_ADAPTER_LINKS
    ISTA_STEP4_EMPTY
)

set(ISTA_STEP4_FAILURE_SOURCES
    CsrReferenceInputFailureTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
)
ista_assert_step4_target(
    ista_csr_reference_input_failure_test
    ISTA_STEP4_FAILURE_SOURCES
    ISTA_STEP4_ADAPTER_LINKS
    ISTA_STEP4_EMPTY
)

set(ISTA_STEP4_TRANSFER_SOURCES
    CsrReferenceTransferContractTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)
ista_assert_step4_target(
    ista_csr_reference_transfer_contract_test
    ISTA_STEP4_TRANSFER_SOURCES
    ISTA_STEP4_ADAPTER_LINKS
    ISTA_STEP4_EMPTY
)

set(ISTA_STEP4_SUPPORT_SOURCES
    CsrReferenceSupportContractTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)
ista_assert_step4_target(
    ista_csr_reference_support_contract_test
    ISTA_STEP4_SUPPORT_SOURCES
    ISTA_STEP4_ADAPTER_LINKS
    ISTA_STEP4_EMPTY
)

set(ISTA_STEP4_FINGERPRINT_SOURCES
    CsrReferenceFingerprintComponentsTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputIndependentEncoding.cpp
    CsrReferenceInputIndependentGraphEncoding.cpp
    CsrReferenceInputIndependentModelEncoding.cpp
)
ista_assert_step4_target(
    ista_csr_reference_fingerprint_components_test
    ISTA_STEP4_FINGERPRINT_SOURCES
    ISTA_STEP4_CORE_LINKS
    ISTA_STEP4_EMPTY
)

set(ISTA_STEP4_POISON_SOURCES
    CsrReferencePoisonIsolationTest.cpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrReferencePoisonSupport.cpp
    CsrReferenceInputRawProjector.cpp
    CsrReferenceInputRawConstraintProjector.cpp
    CsrReferenceInputRawLibraryProjector.cpp
    CsrReferenceInputRawParasiticProjector.cpp
)
ista_assert_step4_target(
    ista_csr_reference_poison_isolation_test
    ISTA_STEP4_POISON_SOURCES
    ISTA_STEP4_ADAPTER_LINKS
    ISTA_STEP4_EMPTY
)

set(ISTA_STEP4_PROPERTY_SOURCES
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
ista_assert_step4_target(
    ista_csr_reference_input_property_test
    ISTA_STEP4_PROPERTY_SOURCES
    ISTA_STEP4_ADAPTER_LINKS
    ISTA_STEP4_EMPTY
)

set(ISTA_STEP4_PROBE_SOURCES
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
ista_assert_step4_target(
    ista_csr_reference_input_probe
    ISTA_STEP4_PROBE_SOURCES
    ISTA_STEP4_PROBE_LINKS
    ISTA_STEP4_EMPTY
)

set(ISTA_STEP4_TARGET_SOURCE_VARIABLES
    "ista_csr_reference_public_headers_test|ISTA_STEP4_PUBLIC_HEADER_SOURCES"
    "ista_csr_reference_input_export_contract_test|ISTA_STEP4_EXPORT_SOURCES"
    "ista_csr_reference_input_failure_test|ISTA_STEP4_FAILURE_SOURCES"
    "ista_csr_reference_transfer_contract_test|ISTA_STEP4_TRANSFER_SOURCES"
    "ista_csr_reference_support_contract_test|ISTA_STEP4_SUPPORT_SOURCES"
    "ista_csr_reference_fingerprint_components_test|ISTA_STEP4_FINGERPRINT_SOURCES"
    "ista_csr_reference_poison_isolation_test|ISTA_STEP4_POISON_SOURCES"
    "ista_csr_reference_input_property_test|ISTA_STEP4_PROPERTY_SOURCES"
    "ista_csr_reference_input_probe|ISTA_STEP4_PROBE_SOURCES"
)
set(ISTA_STEP4_TARGET_SOURCE_ROWS)
foreach(TARGET_SOURCE_VARIABLE IN LISTS ISTA_STEP4_TARGET_SOURCE_VARIABLES)
  string(REPLACE "|" ";" TARGET_SOURCE_PARTS "${TARGET_SOURCE_VARIABLE}")
  list(GET TARGET_SOURCE_PARTS 0 STEP4_TARGET)
  list(GET TARGET_SOURCE_PARTS 1 STEP4_SOURCE_VARIABLE)
  string(JOIN "," STEP4_TARGET_SOURCES ${${STEP4_SOURCE_VARIABLE}})
  list(APPEND ISTA_STEP4_TARGET_SOURCE_ROWS "${STEP4_TARGET}=${STEP4_TARGET_SOURCES}")
endforeach()
string(JOIN "|" ISTA_STEP4_TARGET_SOURCE_ROWS_PACKED ${ISTA_STEP4_TARGET_SOURCE_ROWS})

add_test(
    NAME ista_csr_step4_source_inventory
    COMMAND
    "${CMAKE_COMMAND}"
    "-DISTA_SOURCE_ROOT=${HOME_ISTA}"
    "-DCSR_TEST_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCsrStep4SourceInventory.cmake"
)
set_tests_properties(
    ista_csr_step4_source_inventory
    PROPERTIES LABELS "ista_unit" TIMEOUT 60
)

add_test(
    NAME ista_csr_reference_input_isolation
    COMMAND
    "${CMAKE_COMMAND}"
    "-DISTA_SOURCE_ROOT=${HOME_ISTA}"
    "-DCSR_TEST_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DCSR_STEP4_TARGET_SOURCE_ROWS_PACKED=${ISTA_STEP4_TARGET_SOURCE_ROWS_PACKED}"
    "-DCSR_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DNINJA_EXECUTABLE=${CMAKE_MAKE_PROGRAM}"
    "-DCSR_GENERATOR=${CMAKE_GENERATOR}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCsrReferenceInputIsolation.cmake"
)
set_tests_properties(
    ista_csr_reference_input_isolation
    PROPERTIES LABELS "ista_unit" TIMEOUT 60
)
