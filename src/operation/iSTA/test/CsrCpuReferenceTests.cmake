# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(ISTA_STEP5_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)
set(ISTA_STEP5_CORE_LINKS ista_timing_csr_core)

function(ista_add_step5_target TARGET_NAME SOURCES_VARIABLE)
  add_executable("${TARGET_NAME}" ${${SOURCES_VARIABLE}})
  target_link_libraries("${TARGET_NAME}" PRIVATE ista_timing_csr_core)
  target_compile_options("${TARGET_NAME}" PRIVATE ${ISTA_STEP5_STRICT_OPTIONS})

  get_target_property(ACTUAL_SOURCES "${TARGET_NAME}" SOURCES)
  ista_assert_same_inventory(
      ACTUAL_SOURCES ${SOURCES_VARIABLE} "${TARGET_NAME} source inventory"
  )
  ista_get_target_property_or_empty(ACTUAL_LINKS "${TARGET_NAME}" LINK_LIBRARIES)
  ista_assert_same_inventory(
      ACTUAL_LINKS ISTA_STEP5_CORE_LINKS "${TARGET_NAME} direct links"
  )
  ista_get_target_property_or_empty(ACTUAL_OPTIONS "${TARGET_NAME}" COMPILE_OPTIONS)
  ista_assert_same_inventory(
      ACTUAL_OPTIONS ISTA_STEP5_STRICT_OPTIONS "${TARGET_NAME} compile options"
  )
endfunction()

set(ISTA_STEP5_TABLE_SOURCES
    CsrTableLookupContractTest.cpp
)
ista_add_step5_target(
    ista_csr_table_lookup_contract_test
    ISTA_STEP5_TABLE_SOURCES
)

set(ISTA_STEP5_CONTRACT_SOURCES
    CsrCpuReferenceContractTest.cpp
    CsrCpuReferenceOutcomeContract.cpp
    CsrCpuReferenceArcContract.cpp
    CsrCpuReferencePropagationContract.cpp
    CsrCpuReferenceTestFixture.cpp
)
ista_add_step5_target(
    ista_csr_cpu_reference_contract_test
    ISTA_STEP5_CONTRACT_SOURCES
)

set(ISTA_STEP5_PROPERTY_SOURCES
    CsrCpuReferencePropertyTest.cpp
    CsrCpuReferenceTestFixture.cpp
    CsrCpuReferenceSlowOracleTable.cpp
    CsrCpuReferenceSlowOracleTransfer.cpp
    CsrCpuReferenceSlowOraclePropagation.cpp
)
ista_add_step5_target(
    ista_csr_cpu_reference_property_test
    ISTA_STEP5_PROPERTY_SOURCES
)

set(ISTA_STEP5_PUBLIC_HEADER_SOURCES
    CsrCpuReferencePublicHeadersMain.cpp
    public_header/CsrTableLookupHeaderTest.cpp
    public_header/CsrCpuReferenceBackendHeaderTest.cpp
    CsrCpuReferenceHeaderIncludeOrderTest.cpp
    CsrCpuReferenceHeaderReverseIncludeOrderTest.cpp
)
ista_add_step5_target(
    ista_csr_cpu_reference_public_headers_test
    ISTA_STEP5_PUBLIC_HEADER_SOURCES
)

foreach(STEP5_UNIT_TARGET IN ITEMS
        ista_csr_table_lookup_contract_test
        ista_csr_cpu_reference_contract_test
        ista_csr_cpu_reference_public_headers_test)
  add_test(NAME "${STEP5_UNIT_TARGET}" COMMAND "${STEP5_UNIT_TARGET}")
  set_tests_properties(
      "${STEP5_UNIT_TARGET}" PROPERTIES LABELS "ista_unit" TIMEOUT 60
  )
endforeach()

set(ISTA_STEP5_PROPERTY_SEEDS 1 7 42 20260820 0xdeadbeef)
foreach(PROPERTY_SEED IN LISTS ISTA_STEP5_PROPERTY_SEEDS)
  add_test(
      NAME "ista_csr_cpu_reference_property_seed_${PROPERTY_SEED}"
      COMMAND ista_csr_cpu_reference_property_test --seed "${PROPERTY_SEED}"
  )
  set_tests_properties(
      "ista_csr_cpu_reference_property_seed_${PROPERTY_SEED}"
      PROPERTIES LABELS "ista_property" TIMEOUT 60
  )
endforeach()

string(JOIN "|" ISTA_STEP5_TABLE_SOURCES_PACKED ${ISTA_STEP5_TABLE_SOURCES})
string(JOIN "|" ISTA_STEP5_CONTRACT_SOURCES_PACKED ${ISTA_STEP5_CONTRACT_SOURCES})
string(JOIN "|" ISTA_STEP5_PROPERTY_SOURCES_PACKED ${ISTA_STEP5_PROPERTY_SOURCES})
string(
    JOIN "|"
    ISTA_STEP5_PUBLIC_HEADER_SOURCES_PACKED
    ${ISTA_STEP5_PUBLIC_HEADER_SOURCES}
)
add_test(
    NAME ista_csr_step5_source_inventory
    COMMAND
    "${CMAKE_COMMAND}"
    "-DISTA_SOURCE_ROOT=${HOME_ISTA}"
    "-DCSR_TEST_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DCSR_BUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DNINJA_EXECUTABLE=${CMAKE_MAKE_PROGRAM}"
    "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}"
    "-DNM_EXECUTABLE=${CMAKE_NM}"
    "-DCSR_GENERATOR=${CMAKE_GENERATOR}"
    "-DCSR_TABLE_SOURCES_PACKED=${ISTA_STEP5_TABLE_SOURCES_PACKED}"
    "-DCSR_CONTRACT_SOURCES_PACKED=${ISTA_STEP5_CONTRACT_SOURCES_PACKED}"
    "-DCSR_PROPERTY_SOURCES_PACKED=${ISTA_STEP5_PROPERTY_SOURCES_PACKED}"
    "-DCSR_PUBLIC_HEADER_SOURCES_PACKED=${ISTA_STEP5_PUBLIC_HEADER_SOURCES_PACKED}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckCsrStep5SourceInventory.cmake"
)
set_tests_properties(
    ista_csr_step5_source_inventory
    PROPERTIES LABELS "ista_unit" TIMEOUT 60
)
