# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

  add_executable(ista_csr_core_contract_test
      CsrCoreContractTest.cpp
  )
  target_link_libraries(ista_csr_core_contract_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_core_contract_test COMMAND ista_csr_core_contract_test)
  set_tests_properties(ista_csr_core_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_fingerprint_contract_test
      CsrFingerprintContractTest.cpp
  )
  target_link_libraries(ista_csr_fingerprint_contract_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_fingerprint_contract_test COMMAND ista_csr_fingerprint_contract_test)
  set_tests_properties(ista_csr_fingerprint_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_fingerprint_mutation_test
      CsrFingerprintMutationTest.cpp
  )
  target_link_libraries(ista_csr_fingerprint_mutation_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_fingerprint_mutation_test COMMAND ista_csr_fingerprint_mutation_test)
  set_tests_properties(ista_csr_fingerprint_mutation_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_fingerprint_deep_mutation_test
      CsrFingerprintDeepMutationTest.cpp
  )
  target_link_libraries(ista_csr_fingerprint_deep_mutation_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_fingerprint_deep_mutation_test COMMAND ista_csr_fingerprint_deep_mutation_test)
  set_tests_properties(ista_csr_fingerprint_deep_mutation_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_metadata_isolation_test
      CsrMetadataIsolationTest.cpp
  )
  target_link_libraries(ista_csr_metadata_isolation_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_metadata_isolation_test COMMAND ista_csr_metadata_isolation_test)
  set_tests_properties(ista_csr_metadata_isolation_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_snapshot_identity_contract_test
      CsrSnapshotIdentityContractTest.cpp
  )
  target_link_libraries(ista_csr_snapshot_identity_contract_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_snapshot_identity_contract_test COMMAND ista_csr_snapshot_identity_contract_test)
  set_tests_properties(ista_csr_snapshot_identity_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_fifo_levelization_contract_test
      CsrFifoLevelizationContractTest.cpp
  )
  target_link_libraries(ista_csr_fifo_levelization_contract_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_fifo_levelization_contract_test COMMAND ista_csr_fifo_levelization_contract_test)
  set_tests_properties(ista_csr_fifo_levelization_contract_test PROPERTIES LABELS "ista_unit")

  set(ISTA_CSR_CORE_NARROW_TEST_TARGETS
      ista_csr_snapshot_identity_contract_test
      ista_csr_fifo_levelization_contract_test
  )
  set(ISTA_CSR_EXPECTED_CORE_NARROW_LINKS ista_timing_csr_core)
  foreach(CORE_TARGET IN LISTS ISTA_CSR_CORE_NARROW_TEST_TARGETS)
    ista_get_target_property_or_empty(CORE_LINKS "${CORE_TARGET}" LINK_LIBRARIES)
    ista_assert_same_inventory(CORE_LINKS ISTA_CSR_EXPECTED_CORE_NARROW_LINKS "${CORE_TARGET} direct links")
  endforeach()

  set(ISTA_CSR_SNAPSHOT_STORAGE_TEST_SOURCES
      CsrSnapshotStorageContractTest.cpp
      CsrSnapshotTestFixture.cpp
      CsrReferenceInputStorageOracle.cpp
  )
  add_executable(ista_csr_snapshot_storage_contract_test
      ${ISTA_CSR_SNAPSHOT_STORAGE_TEST_SOURCES}
  )
  get_target_property(
      ISTA_CSR_SNAPSHOT_STORAGE_TEST_ACTUAL_SOURCES
      ista_csr_snapshot_storage_contract_test
      SOURCES
  )
  ista_assert_same_inventory(
      ISTA_CSR_SNAPSHOT_STORAGE_TEST_ACTUAL_SOURCES
      ISTA_CSR_SNAPSHOT_STORAGE_TEST_SOURCES
      "CSR snapshot storage test source inventory"
  )
  target_link_libraries(ista_csr_snapshot_storage_contract_test
      PRIVATE
      ista_timing_csr_snapshot_contract
  )
  add_test(NAME ista_csr_snapshot_storage_contract_test COMMAND ista_csr_snapshot_storage_contract_test)
  set_tests_properties(ista_csr_snapshot_storage_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_snapshot_observation_contract_test
      CsrSnapshotObservationContractTest.cpp
      CsrSnapshotTestFixture.cpp
  )
  target_link_libraries(ista_csr_snapshot_observation_contract_test
      PRIVATE
      ista_timing_csr_snapshot_contract
  )
  add_test(NAME ista_csr_snapshot_observation_contract_test COMMAND ista_csr_snapshot_observation_contract_test)
  set_tests_properties(ista_csr_snapshot_observation_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_snapshot_transition_contract_test
      CsrSnapshotTransitionContractTest.cpp
      CsrSnapshotTestFixture.cpp
  )
  target_link_libraries(ista_csr_snapshot_transition_contract_test
      PRIVATE
      ista_timing_csr_snapshot_contract
  )
  add_test(NAME ista_csr_snapshot_transition_contract_test COMMAND ista_csr_snapshot_transition_contract_test)
  set_tests_properties(ista_csr_snapshot_transition_contract_test PROPERTIES LABELS "ista_unit")

  set(ISTA_CSR_SNAPSHOT_NARROW_TEST_TARGETS
      ista_csr_snapshot_storage_contract_test
      ista_csr_snapshot_observation_contract_test
      ista_csr_snapshot_transition_contract_test
  )
  set(ISTA_CSR_EXPECTED_SNAPSHOT_NARROW_LINKS ista_timing_csr_snapshot_contract)
  foreach(SNAPSHOT_TARGET IN LISTS ISTA_CSR_SNAPSHOT_NARROW_TEST_TARGETS)
    ista_get_target_property_or_empty(SNAPSHOT_LINKS "${SNAPSHOT_TARGET}" LINK_LIBRARIES)
    ista_assert_same_inventory(SNAPSHOT_LINKS ISTA_CSR_EXPECTED_SNAPSHOT_NARROW_LINKS "${SNAPSHOT_TARGET} direct links")
  endforeach()

  add_test(
      NAME ista_csr_snapshot_compiled_contract_parity
      COMMAND
      "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/schema/validate_csr_snapshot_compiled_contracts.py"
      --manifest "${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/schema/csr_snapshot_contract_manifest_v1.json"
      --executable "identity=$<TARGET_FILE:ista_csr_snapshot_identity_contract_test>"
      --executable "fifo=$<TARGET_FILE:ista_csr_fifo_levelization_contract_test>"
      --executable "storage=$<TARGET_FILE:ista_csr_snapshot_storage_contract_test>"
      --executable "observation=$<TARGET_FILE:ista_csr_snapshot_observation_contract_test>"
      --executable "transition=$<TARGET_FILE:ista_csr_snapshot_transition_contract_test>"
  )
  set_tests_properties(
      ista_csr_snapshot_compiled_contract_parity
      PROPERTIES
      LABELS "ista_unit"
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
  )

  add_executable(ista_csr_validator_contract_test
      CsrValidatorContractTest.cpp
  )
  target_link_libraries(ista_csr_validator_contract_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_validator_contract_test COMMAND ista_csr_validator_contract_test)
  set_tests_properties(ista_csr_validator_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_timing_result_validator_test
      CsrTimingResultValidatorTest.cpp
  )
  target_link_libraries(ista_csr_timing_result_validator_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_timing_result_validator_test COMMAND ista_csr_timing_result_validator_test)
  set_tests_properties(ista_csr_timing_result_validator_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_outcome_validator_test
      CsrOutcomeValidatorTest.cpp
  )
  target_link_libraries(ista_csr_outcome_validator_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_outcome_validator_test COMMAND ista_csr_outcome_validator_test)
  set_tests_properties(ista_csr_outcome_validator_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_unsupported_reason_validator_test
      CsrUnsupportedReasonValidatorTest.cpp
  )
  target_link_libraries(ista_csr_unsupported_reason_validator_test PRIVATE ista_timing_csr_core)
  add_test(NAME ista_csr_unsupported_reason_validator_test COMMAND ista_csr_unsupported_reason_validator_test)
  set_tests_properties(ista_csr_unsupported_reason_validator_test PROPERTIES LABELS "ista_unit")

  set(ISTA_CSR_NATIVE_GRAPH_SUPPORT_SOURCES
      CsrNativeSemanticKeyTestSupport.hpp
      CsrNativeGraphFixture.cpp
      CsrNativeGraphRawProjector.cpp
      CsrNativeGraphOracleReader.cpp
      CsrNativeGraphIndependentEncoding.cpp
  )

  add_executable(ista_csr_graph_export_contract_test
      CsrGraphExportContractTest.cpp
      ${ISTA_CSR_NATIVE_GRAPH_SUPPORT_SOURCES}
  )
  target_link_libraries(ista_csr_graph_export_contract_test
      PRIVATE
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
  )
  add_test(NAME ista_csr_graph_export_contract_test COMMAND ista_csr_graph_export_contract_test)
  set_tests_properties(ista_csr_graph_export_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_graph_export_failure_test
      CsrGraphExportFailureTest.cpp
      CsrNativeGraphFixture.cpp
  )
  target_link_libraries(ista_csr_graph_export_failure_test
      PRIVATE
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
  )
  add_test(NAME ista_csr_graph_export_failure_test COMMAND ista_csr_graph_export_failure_test)
  set_tests_properties(ista_csr_graph_export_failure_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_graph_encoding_contract_test
      CsrGraphEncodingContractTest.cpp
      CsrNativeGraphWireDecoder.cpp
      ${ISTA_CSR_NATIVE_GRAPH_SUPPORT_SOURCES}
  )
  target_link_libraries(ista_csr_graph_encoding_contract_test
      PRIVATE
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
  )
  add_test(NAME ista_csr_graph_encoding_contract_test COMMAND ista_csr_graph_encoding_contract_test)
  set_tests_properties(ista_csr_graph_encoding_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_csr_graph_property_test
      CsrGraphPropertyTest.cpp
      ${ISTA_CSR_NATIVE_GRAPH_SUPPORT_SOURCES}
  )
  target_link_libraries(ista_csr_graph_property_test
      PRIVATE
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
  )
  set(ISTA_CSR_GRAPH_PROPERTY_SEEDS 1 7 42 20260820 0xdeadbeef)
  foreach(PROPERTY_SEED IN LISTS ISTA_CSR_GRAPH_PROPERTY_SEEDS)
    add_test(
        NAME "ista_csr_graph_property_seed_${PROPERTY_SEED}"
        COMMAND ista_csr_graph_property_test --seed "${PROPERTY_SEED}"
    )
    set_tests_properties(
        "ista_csr_graph_property_seed_${PROPERTY_SEED}"
        PROPERTIES
        LABELS "ista_property"
        TIMEOUT 60
    )
  endforeach()

  set(ISTA_CSR_NARROW_TEST_TARGETS
      ista_csr_graph_export_contract_test
      ista_csr_graph_export_failure_test
      ista_csr_graph_encoding_contract_test
      ista_csr_graph_property_test
  )
  set(ISTA_CSR_EXPECTED_NARROW_LINKS
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
  )
  foreach(NARROW_TARGET IN LISTS ISTA_CSR_NARROW_TEST_TARGETS)
    ista_get_target_property_or_empty(NARROW_LINKS "${NARROW_TARGET}" LINK_LIBRARIES)
    ista_assert_same_inventory(NARROW_LINKS ISTA_CSR_EXPECTED_NARROW_LINKS "${NARROW_TARGET} direct links")
  endforeach()
