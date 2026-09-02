add_library(ista_timing_dual_csr_transaction
    ${CMAKE_CURRENT_SOURCE_DIR}/CommittedDualCsrAnalysisState.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrModeRecoveryReplay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrRecoveryRebind.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrStageFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTransactionCoordinator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrExactReceipt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/LevelizationExecutionTrace.cpp
)

target_include_directories(ista_timing_dual_csr_transaction
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_timing_dual_csr_transaction
    PUBLIC
    ista_timing_dual_csr
    ista_timing_dual_csr_native
    ista_timing_edit_contract
    ista_timing_incremental_levelization
    PRIVATE
    ista_timing_edit_transaction
)

target_compile_features(ista_timing_dual_csr_transaction PUBLIC cxx_std_20)
target_compile_options(ista_timing_dual_csr_transaction PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

if(BUILD_TESTING)
    add_executable(ista_timing_dual_csr_transaction_test
        ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTransactionCoordinatorTest.cpp
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
    target_include_directories(ista_timing_dual_csr_transaction_test
        PRIVATE
        ${ISTA_MODULE}/timing_csr_snapshot
        ${ISTA_TEST}/timing_dual_csr
        ${ISTA_TEST}/timing_edit
    )
    target_link_libraries(ista_timing_dual_csr_transaction_test
        PRIVATE
        ista_timing_dual_csr_transaction
        ista_timing_edit_transaction
        ista_timing_dual_csr_native
        ista_timing_edit_native
        ista_timing_graph_csr_adapter
        ista_timing_database_headers
        ista_interface
        ista_data_manager
        ista_delay_calculator
    )
    target_compile_features(
        ista_timing_dual_csr_transaction_test PRIVATE cxx_std_20)
    target_compile_options(ista_timing_dual_csr_transaction_test PRIVATE
        -Wall
        -Wextra
        -Werror
        -pedantic
        -ffp-contract=off
    )
    add_test(NAME ista_timing_dual_csr_transaction_test
             COMMAND ista_timing_dual_csr_transaction_test)
    set_tests_properties(ista_timing_dual_csr_transaction_test PROPERTIES
        LABELS "ista_unit;ista_timing_dual_csr;ista_transaction;ista_incremental_levelization;ista_timing_incremental_levelization"
    )
endif()
