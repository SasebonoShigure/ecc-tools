# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

function(ista_register_timing_edit_test TARGET_NAME)
  target_compile_options(${TARGET_NAME}
      PRIVATE
      ${ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS}
  )
  add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
  set_tests_properties(${TARGET_NAME} PROPERTIES LABELS "ista_unit;ista_timing_edit")
endfunction()

set(ISTA_TIMING_EDIT_CONTRACT_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditContractTest.cpp"
)
set(ISTA_TIMING_EDIT_CONTRACT_TEST_LINKS ista_timing_edit_contract)
add_executable(ista_timing_edit_contract_test ${ISTA_TIMING_EDIT_CONTRACT_TEST_SOURCES})
target_link_libraries(ista_timing_edit_contract_test
    PRIVATE
    ${ISTA_TIMING_EDIT_CONTRACT_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_contract_test)

set(ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditNativeFixture.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditNativeFixture.hpp"
)

set(ISTA_TIMING_EDIT_RESOLVER_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditResolverTest.cpp"
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_RESOLVER_TEST_LINKS
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
add_executable(ista_timing_edit_resolver_test ${ISTA_TIMING_EDIT_RESOLVER_TEST_SOURCES})
target_sources(ista_timing_edit_resolver_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/AllocationFailureSupport.cpp")
target_link_libraries(ista_timing_edit_resolver_test
    PRIVATE
    ${ISTA_TIMING_EDIT_RESOLVER_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_resolver_test)

set(ISTA_TIMING_EDIT_NATIVE_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditNativeTest.cpp"
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_NATIVE_TEST_LINKS
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
add_executable(ista_timing_edit_native_test ${ISTA_TIMING_EDIT_NATIVE_TEST_SOURCES})
target_link_libraries(ista_timing_edit_native_test
    PRIVATE
    ${ISTA_TIMING_EDIT_NATIVE_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_native_test)

set(ISTA_TIMING_EDIT_CSR_RECEIPT_TEST_SUPPORT
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditCsrReceiptProbe.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/CsrNativeTimingOracleReader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/CsrNativeTimingOracleNodeReader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/CsrNativeTimingOracleArcReader.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/CsrTimingResultFromNativeOracle.cpp"
)

set(ISTA_TIMING_EDIT_TRANSACTION_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditRuntimeContract.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.hpp"
    ${ISTA_TIMING_EDIT_CSR_RECEIPT_TEST_SUPPORT}
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_TRANSACTION_TEST_LINKS
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_interface
    ista_data_manager
    ista_delay_calculator
)
add_executable(ista_timing_edit_transaction_test ${ISTA_TIMING_EDIT_TRANSACTION_TEST_SOURCES})
target_link_libraries(ista_timing_edit_transaction_test
    PRIVATE
    ${ISTA_TIMING_EDIT_TRANSACTION_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_transaction_test)

set(ISTA_TIMING_EDIT_GUARD_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditGuardScopeTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.hpp"
    ${ISTA_TIMING_EDIT_CSR_RECEIPT_TEST_SUPPORT}
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_GUARD_TEST_LINKS
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_interface
    ista_data_manager
    ista_delay_calculator
)
add_executable(ista_timing_edit_guard_scope_test
    ${ISTA_TIMING_EDIT_GUARD_TEST_SOURCES}
)
target_link_libraries(ista_timing_edit_guard_scope_test
    PRIVATE
    ${ISTA_TIMING_EDIT_GUARD_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_guard_scope_test)

set(ISTA_TIMING_EDIT_TRANSACTION_FAULT_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionFaultTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.hpp"
    ${ISTA_TIMING_EDIT_CSR_RECEIPT_TEST_SUPPORT}
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_TRANSACTION_FAULT_TEST_LINKS
    ${ISTA_TIMING_EDIT_TRANSACTION_TEST_LINKS}
)
add_executable(ista_timing_edit_transaction_fault_test
    "${CMAKE_CURRENT_LIST_DIR}/AllocationFailureSupport.cpp"
    ${ISTA_TIMING_EDIT_TRANSACTION_FAULT_TEST_SOURCES}
)
target_link_libraries(ista_timing_edit_transaction_fault_test
    PRIVATE
    ${ISTA_TIMING_EDIT_TRANSACTION_FAULT_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_transaction_fault_test)

set(ISTA_TIMING_EDIT_DAG_PROPERTY_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditDagPropertyTest.cpp"
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_DAG_PROPERTY_TEST_LINKS
    ${ISTA_TIMING_EDIT_NATIVE_TEST_LINKS}
    ista_data_manager
    ista_graph_builder
    ista_delay_calculator
)
add_executable(ista_timing_edit_dag_property_test
    ${ISTA_TIMING_EDIT_DAG_PROPERTY_TEST_SOURCES}
)
target_link_libraries(ista_timing_edit_dag_property_test
    PRIVATE
    ${ISTA_TIMING_EDIT_DAG_PROPERTY_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_dag_property_test)
set_tests_properties(ista_timing_edit_dag_property_test
    PROPERTIES LABELS "ista_unit;ista_property;ista_timing_edit"
)

set(ISTA_TIMING_EDIT_PREPARE_PROPERTY_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditPreparePropertyTest.cpp"
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_PREPARE_PROPERTY_TEST_LINKS
    ${ISTA_TIMING_EDIT_NATIVE_TEST_LINKS}
)
add_executable(ista_timing_edit_prepare_property_test
    ${ISTA_TIMING_EDIT_PREPARE_PROPERTY_TEST_SOURCES}
)
target_link_libraries(ista_timing_edit_prepare_property_test
    PRIVATE
    ${ISTA_TIMING_EDIT_PREPARE_PROPERTY_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_prepare_property_test)
set_tests_properties(ista_timing_edit_prepare_property_test
    PROPERTIES LABELS "ista_unit;ista_property;ista_timing_edit"
)

add_executable(ista_native_targeted_edit_test
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/NativeTargetedEditTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/AllocationFailureSupport.cpp"
    ${ISTA_TIMING_EDIT_CSR_RECEIPT_TEST_SUPPORT}
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
target_link_libraries(ista_native_targeted_edit_test PRIVATE
    ${ISTA_TIMING_EDIT_TRANSACTION_TEST_LINKS} ista_graph_builder)
ista_register_timing_edit_test(ista_native_targeted_edit_test)
