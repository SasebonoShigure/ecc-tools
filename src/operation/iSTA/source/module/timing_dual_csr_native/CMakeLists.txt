add_library(ista_timing_dual_csr_native
    ${CMAKE_CURRENT_SOURCE_DIR}/DualSemanticReadView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NarrowTimingRecordReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrArcGroupResolverValidation.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrArcBijectionInternal.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrAffectedArcIndex.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrCaptureRunInternal.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrDataNetPolicyInternal.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrOutgoingOrderInternal.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrTypedCaptureInternal.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/DualCsrStructuredBatchPreparer.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/DualCsrStructuredBeforeBinding.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/DualCsrStructuredCapturePlan.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/TimingEditDualCsrCaptureEvidenceInternal.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/TimingEditDualCsrDescriptorBuilder.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/DirectCsrBufferEdit.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/TimingEditDualCsrDescriptorStructure.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrPayloadCapture.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrReferenceCapture.cpp
    ${ISTA_SOURCE}/data_manager/adapter/csr/NativeDualCsrParasiticCapture.cpp
)

target_include_directories(ista_timing_dual_csr_native
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ISTA_SOURCE}/data_manager/adapter/csr
)

target_link_libraries(ista_timing_dual_csr_native
    PUBLIC
    ista_timing_dual_csr
    ista_timing_edit_native
    PRIVATE
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_data_manager
)

target_compile_features(ista_timing_dual_csr_native PUBLIC cxx_std_20)
target_compile_options(ista_timing_dual_csr_native
    PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

if(BUILD_TESTING)
  add_executable(ista_timing_dual_csr_native_test
      ${CMAKE_CURRENT_SOURCE_DIR}/DualSemanticReadViewTest.cpp
  )
  target_link_libraries(ista_timing_dual_csr_native_test
      PRIVATE
      ista_timing_dual_csr_native
  )
  target_compile_options(ista_timing_dual_csr_native_test PRIVATE
      -Wall
      -Wextra
      -Werror
      -pedantic
      -ffp-contract=off
  )
  add_test(NAME ista_timing_dual_csr_native_test
           COMMAND ista_timing_dual_csr_native_test)
  add_executable(ista_timing_narrow_record_reader_test
      ${CMAKE_CURRENT_SOURCE_DIR}/NarrowTimingRecordReaderTest.cpp
      ${ISTA_SOURCE}/../test/timing_edit/TimingEditNativeFixture.cpp
  )
  target_include_directories(ista_timing_narrow_record_reader_test
      PRIVATE
      ${ISTA_SOURCE}/../test/timing_edit
  )
  target_link_libraries(ista_timing_narrow_record_reader_test
      PRIVATE
      ista_timing_dual_csr_native
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
  )
  target_compile_options(ista_timing_narrow_record_reader_test PRIVATE -Wall -Wextra -Werror -pedantic -ffp-contract=off)
  add_test(NAME ista_timing_narrow_record_reader_test COMMAND ista_timing_narrow_record_reader_test)

  add_executable(ista_timing_committed_peer_resolution_test
      ${CMAKE_CURRENT_SOURCE_DIR}/CommittedPeerResolutionTest.cpp
      ${ISTA_SOURCE}/../test/timing_edit/TimingEditNativeFixture.cpp
  )
  target_include_directories(ista_timing_committed_peer_resolution_test
      PRIVATE
      ${ISTA_SOURCE}/../test/timing_edit
  )
  target_link_libraries(ista_timing_committed_peer_resolution_test
      PRIVATE
      ista_timing_dual_csr_native
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
  )
  target_compile_options(ista_timing_committed_peer_resolution_test PRIVATE
      -Wall -Wextra -Werror -pedantic -ffp-contract=off
  )
  add_test(NAME ista_timing_committed_peer_resolution_test
           COMMAND ista_timing_committed_peer_resolution_test)

  set(ISTA_DUAL_CSR_STRUCTURED_TEST_SUPPORT
      ${ISTA_TEST}/timing_dual_csr/DualCsrStructuredBatchTestSupport.cpp
      ${ISTA_TEST}/timing_edit/StaTimingEditSessionProbeAccess.cpp
      ${ISTA_TEST}/timing_edit/StaTimingEditCsrReceiptProbe.cpp
      ${ISTA_TEST}/CsrNativeTimingOracleReader.cpp
      ${ISTA_TEST}/CsrNativeTimingOracleNodeReader.cpp
      ${ISTA_TEST}/CsrNativeTimingOracleArcReader.cpp
      ${ISTA_TEST}/CsrTimingResultFromNativeOracle.cpp
      ${ISTA_TEST}/timing_edit/TimingEditNativeFixture.cpp
      ${ISTA_TEST}/timing_edit/TimingEditTransactionNativeSupport.cpp
  )
  add_executable(ista_timing_dual_csr_structured_batch_test
      ${ISTA_TEST}/timing_dual_csr/DualCsrStructuredBatchTest.cpp
      ${ISTA_DUAL_CSR_STRUCTURED_TEST_SUPPORT}
  )
  target_include_directories(ista_timing_dual_csr_structured_batch_test
      PRIVATE
      ${ISTA_MODULE}/timing_csr_snapshot
      ${ISTA_TEST}/timing_dual_csr
      ${ISTA_TEST}/timing_edit
  )
  target_link_libraries(ista_timing_dual_csr_structured_batch_test
      PRIVATE
      ista_timing_dual_csr_native
      ista_timing_edit_native
      ista_timing_graph_csr_adapter
      ista_timing_database_headers
      ista_interface
      ista_data_manager
      ista_delay_calculator
  )
  target_compile_features(ista_timing_dual_csr_structured_batch_test
      PRIVATE cxx_std_20)
  target_compile_options(ista_timing_dual_csr_structured_batch_test PRIVATE
      -Wall -Wextra -Werror -pedantic -ffp-contract=off)
  add_test(NAME ista_timing_dual_csr_structured_batch_test
           COMMAND ista_timing_dual_csr_structured_batch_test)
  set_tests_properties(ista_timing_dual_csr_structured_batch_test
      PROPERTIES LABELS "ista_unit;ista_timing_dual_csr")

  add_executable(ista_timing_direct_csr_edit_test
      ${ISTA_TEST}/timing_dual_csr/DirectCsrBufferEditTest.cpp
      ${ISTA_DUAL_CSR_STRUCTURED_TEST_SUPPORT})
  target_include_directories(ista_timing_direct_csr_edit_test PRIVATE
      ${ISTA_MODULE}/graph_builder
      ${ISTA_MODULE}/timing_csr_snapshot
      ${ISTA_TEST}/timing_dual_csr ${ISTA_TEST}/timing_edit)
  target_link_libraries(ista_timing_direct_csr_edit_test PRIVATE
      ista_timing_dual_csr_native ista_timing_graph_csr_adapter
      ista_interface ista_data_manager ista_delay_calculator)
  target_compile_options(ista_timing_direct_csr_edit_test PRIVATE
      -Wall -Wextra -Werror -pedantic -ffp-contract=off)
  add_test(NAME ista_timing_direct_csr_edit_test COMMAND ista_timing_direct_csr_edit_test)
  set_tests_properties(ista_timing_direct_csr_edit_test PROPERTIES LABELS "ista_unit;ista_timing_dual_csr")

  add_executable(ista_timing_dual_csr_parallel_deactivation_receipt_test
      ${ISTA_TEST}/timing_dual_csr/DualCsrParallelDeactivationReceiptTest.cpp
  )
  target_include_directories(
      ista_timing_dual_csr_parallel_deactivation_receipt_test
      PRIVATE
      ${ISTA_TEST}
      ${ISTA_TEST}/timing_dual_csr)
  target_link_libraries(
      ista_timing_dual_csr_parallel_deactivation_receipt_test
      PRIVATE ista_timing_dual_csr_native)
  target_compile_features(
      ista_timing_dual_csr_parallel_deactivation_receipt_test
      PRIVATE cxx_std_20)
  target_compile_options(
      ista_timing_dual_csr_parallel_deactivation_receipt_test PRIVATE
      -Wall -Wextra -Werror -pedantic -ffp-contract=off)
  add_test(NAME ista_timing_dual_csr_parallel_deactivation_receipt_test
           COMMAND ista_timing_dual_csr_parallel_deactivation_receipt_test)
  set_tests_properties(
      ista_timing_dual_csr_parallel_deactivation_receipt_test
      PROPERTIES LABELS "ista_unit;ista_timing_dual_csr")
endif()

if(BUILD_TESTING)
  ista_diagnostic_tests(
      ista_timing_narrow_record_reader_test
      ista_timing_dual_csr_structured_batch_test
  )
endif()

if(BUILD_TESTING)
  target_sources(ista_timing_dual_csr_structured_batch_test PRIVATE
      ${ISTA_TEST}/AllocationFailureSupport.cpp)
endif()
