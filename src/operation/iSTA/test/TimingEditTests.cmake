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

function(ista_assert_timing_edit_target_inventory TARGET_NAME SOURCES_VARIABLE LINKS_VARIABLE OPTIONS_VARIABLE)
  get_target_property(ACTUAL_SOURCES "${TARGET_NAME}" SOURCES)
  get_target_property(TARGET_SOURCE_DIRECTORY "${TARGET_NAME}" SOURCE_DIR)
  set(NORMALIZED_SOURCES)
  foreach(SOURCE IN LISTS ACTUAL_SOURCES)
    if(IS_ABSOLUTE "${SOURCE}")
      list(APPEND NORMALIZED_SOURCES "${SOURCE}")
    else()
      get_filename_component(ABSOLUTE_SOURCE "${SOURCE}" ABSOLUTE BASE_DIR "${TARGET_SOURCE_DIRECTORY}")
      list(APPEND NORMALIZED_SOURCES "${ABSOLUTE_SOURCE}")
    endif()
  endforeach()
  set(ACTUAL_SOURCES ${NORMALIZED_SOURCES})
  ista_assert_same_inventory(
      ACTUAL_SOURCES ${SOURCES_VARIABLE} "${TARGET_NAME} source inventory"
  )
  ista_get_target_property_or_empty(ACTUAL_LINKS "${TARGET_NAME}" LINK_LIBRARIES)
  ista_assert_same_inventory(
      ACTUAL_LINKS ${LINKS_VARIABLE} "${TARGET_NAME} direct links"
  )
  ista_get_target_property_or_empty(ACTUAL_OPTIONS "${TARGET_NAME}" COMPILE_OPTIONS)
  ista_assert_same_inventory(
      ACTUAL_OPTIONS ${OPTIONS_VARIABLE} "${TARGET_NAME} compile options"
  )
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
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.hpp"
    ${ISTA_TIMING_EDIT_CSR_RECEIPT_TEST_SUPPORT}
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_TRANSACTION_TEST_LINKS
    ista_timing_edit_transaction
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
    ista_timing_edit_transaction
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

set(ISTA_TIMING_EDIT_RECEIPT_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditReceiptMismatchTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.hpp"
    ${ISTA_TIMING_EDIT_CSR_RECEIPT_TEST_SUPPORT}
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_RECEIPT_TEST_LINKS
    ${ISTA_TIMING_EDIT_TRANSACTION_TEST_LINKS}
)
add_executable(ista_timing_edit_receipt_test
    ${ISTA_TIMING_EDIT_RECEIPT_TEST_SOURCES}
)
target_link_libraries(ista_timing_edit_receipt_test
    PRIVATE
    ${ISTA_TIMING_EDIT_RECEIPT_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_receipt_test)

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
    ${ISTA_TIMING_EDIT_TRANSACTION_FAULT_TEST_SOURCES}
)
target_link_libraries(ista_timing_edit_transaction_fault_test
    PRIVATE
    ${ISTA_TIMING_EDIT_TRANSACTION_FAULT_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_transaction_fault_test)

set(ISTA_TIMING_EDIT_FRESH_RECOVERY_TEST_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/StaTimingEditSessionProbeAccess.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditFreshRecoveryTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/TimingEditTransactionNativeSupport.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle/IndependentOracleObservation.cpp"
    ${ISTA_TIMING_EDIT_CSR_RECEIPT_TEST_SUPPORT}
    ${ISTA_TIMING_EDIT_NATIVE_TEST_SUPPORT}
)
set(ISTA_TIMING_EDIT_FRESH_RECOVERY_TEST_LINKS
    ${ISTA_TIMING_EDIT_TRANSACTION_TEST_LINKS}
)
add_executable(ista_timing_edit_fresh_recovery_test
    ${ISTA_TIMING_EDIT_FRESH_RECOVERY_TEST_SOURCES}
)
target_link_libraries(ista_timing_edit_fresh_recovery_test
    PRIVATE
    ${ISTA_TIMING_EDIT_FRESH_RECOVERY_TEST_LINKS}
)
ista_register_timing_edit_test(ista_timing_edit_fresh_recovery_test)

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

set(ISTA_TIMING_EDIT_CONTRACT_SOURCES
    "${HOME_ISTA}/source/module/timing_edit_contract/TimingEditCanonicalEncoding.cpp"
    "${HOME_ISTA}/source/module/timing_edit_contract/TimingEditPreparedBatchValidation.cpp"
    "${HOME_ISTA}/source/module/timing_edit_contract/TimingEditTrace.cpp"
    "${HOME_ISTA}/source/module/timing_edit_contract/TimingEditTraceJson.cpp"
    "${HOME_ISTA}/source/module/timing_edit_contract/TimingEditValidation.cpp"
)
set(ISTA_TIMING_EDIT_CONTRACT_LINKS ista_timing_csr_core)
set(ISTA_TIMING_EDIT_CONTRACT_OPTIONS -ffp-contract=off)
set(ISTA_TIMING_EDIT_NATIVE_SOURCES
    "${HOME_ISTA}/source/data_manager/adapter/timing_edit/NativeBufferCellValidation.cpp"
    "${HOME_ISTA}/source/data_manager/adapter/timing_edit/NativeDatabaseCandidate.cpp"
    "${HOME_ISTA}/source/data_manager/adapter/timing_edit/NativeDatabaseValidation.cpp"
    "${HOME_ISTA}/source/data_manager/adapter/timing_edit/NativeCandidateExecutor.cpp"
    "${HOME_ISTA}/source/data_manager/adapter/timing_edit/TimingEditResolver.cpp"
    "${HOME_ISTA}/source/data_manager/adapter/timing_edit/NativeDesignEditTranslator.cpp"
    "${HOME_ISTA}/source/data_manager/adapter/timing_edit/NativeTimingEditPreparer.cpp"
    "${HOME_ISTA}/source/data_manager/adapter/timing_edit/NativeTimingEditTraceClosure.cpp"
)
set(ISTA_TIMING_EDIT_NATIVE_LINKS
    ista_timing_edit_contract
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_data_manager
    ista_graph_builder
    ista_delay_calculator
    ista_clock_propagator
    ista_timing_propagator
    ista_timing_analyzer
)
set(ISTA_TIMING_EDIT_EMPTY_OPTIONS)
set(ISTA_TIMING_EDIT_TRANSACTION_SOURCES
    "${HOME_ISTA}/source/module/timing_edit_transaction/CsrFullExactReceipt.cpp"
    "${HOME_ISTA}/source/module/timing_edit_transaction/TimingEditCoordinator.cpp"
)
set(ISTA_TIMING_EDIT_TRANSACTION_LINKS
    ista_timing_edit_contract
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
)

ista_assert_timing_edit_target_inventory(
    ista_timing_edit_contract
    ISTA_TIMING_EDIT_CONTRACT_SOURCES
    ISTA_TIMING_EDIT_CONTRACT_LINKS
    ISTA_TIMING_EDIT_CONTRACT_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_native
    ISTA_TIMING_EDIT_NATIVE_SOURCES
    ISTA_TIMING_EDIT_NATIVE_LINKS
    ISTA_TIMING_EDIT_EMPTY_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_transaction
    ISTA_TIMING_EDIT_TRANSACTION_SOURCES
    ISTA_TIMING_EDIT_TRANSACTION_LINKS
    ISTA_TIMING_EDIT_EMPTY_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_contract_test
    ISTA_TIMING_EDIT_CONTRACT_TEST_SOURCES
    ISTA_TIMING_EDIT_CONTRACT_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_resolver_test
    ISTA_TIMING_EDIT_RESOLVER_TEST_SOURCES
    ISTA_TIMING_EDIT_RESOLVER_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_native_test
    ISTA_TIMING_EDIT_NATIVE_TEST_SOURCES
    ISTA_TIMING_EDIT_NATIVE_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_transaction_test
    ISTA_TIMING_EDIT_TRANSACTION_TEST_SOURCES
    ISTA_TIMING_EDIT_TRANSACTION_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_guard_scope_test
    ISTA_TIMING_EDIT_GUARD_TEST_SOURCES
    ISTA_TIMING_EDIT_GUARD_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_receipt_test
    ISTA_TIMING_EDIT_RECEIPT_TEST_SOURCES
    ISTA_TIMING_EDIT_RECEIPT_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_transaction_fault_test
    ISTA_TIMING_EDIT_TRANSACTION_FAULT_TEST_SOURCES
    ISTA_TIMING_EDIT_TRANSACTION_FAULT_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_fresh_recovery_test
    ISTA_TIMING_EDIT_FRESH_RECOVERY_TEST_SOURCES
    ISTA_TIMING_EDIT_FRESH_RECOVERY_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_dag_property_test
    ISTA_TIMING_EDIT_DAG_PROPERTY_TEST_SOURCES
    ISTA_TIMING_EDIT_DAG_PROPERTY_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)
ista_assert_timing_edit_target_inventory(
    ista_timing_edit_prepare_property_test
    ISTA_TIMING_EDIT_PREPARE_PROPERTY_TEST_SOURCES
    ISTA_TIMING_EDIT_PREPARE_PROPERTY_TEST_LINKS
    ISTA_TIMING_EDIT_TEST_COMPILE_OPTIONS
)

function(ista_collect_timing_edit_link_closure ROOT_TARGET OUTPUT_VARIABLE)
  set(QUEUE "${ROOT_TARGET}")
  set(VISITED)
  while(QUEUE)
    list(POP_FRONT QUEUE CURRENT_TARGET)
    if(CURRENT_TARGET IN_LIST VISITED OR NOT TARGET "${CURRENT_TARGET}")
      continue()
    endif()
    list(APPEND VISITED "${CURRENT_TARGET}")
    foreach(PROPERTY_NAME IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      ista_get_target_property_or_empty(DEPENDENCIES "${CURRENT_TARGET}" "${PROPERTY_NAME}")
      foreach(RAW_DEPENDENCY IN LISTS DEPENDENCIES)
        set(DEPENDENCY "${RAW_DEPENDENCY}")
        if("${DEPENDENCY}" MATCHES "^\\$<LINK_ONLY:([^>]+)>$")
          set(DEPENDENCY "${CMAKE_MATCH_1}")
        endif()
        if(TARGET "${DEPENDENCY}" AND NOT DEPENDENCY IN_LIST VISITED)
          list(APPEND QUEUE "${DEPENDENCY}")
        endif()
      endforeach()
    endforeach()
  endwhile()
  list(SORT VISITED)
  set("${OUTPUT_VARIABLE}" "${VISITED}" PARENT_SCOPE)
endfunction()

ista_collect_timing_edit_link_closure(
    ista_timing_edit_transaction ISTA_TIMING_EDIT_TRANSACTION_CLOSURE
)
string(JOIN "|" ISTA_TIMING_EDIT_TRANSACTION_CLOSURE_PACKED
    ${ISTA_TIMING_EDIT_TRANSACTION_CLOSURE})
add_test(
    NAME ista_timing_edit_source_isolation
    COMMAND
    "${CMAKE_COMMAND}"
    "-DISTA_SOURCE_ROOT=${HOME_ISTA}/source"
    "-DTRANSACTION_CLOSURE_PACKED=${ISTA_TIMING_EDIT_TRANSACTION_CLOSURE_PACKED}"
    -P "${CMAKE_CURRENT_LIST_DIR}/timing_edit/CheckTimingEditSourceIsolation.cmake"
)
set_tests_properties(
    ista_timing_edit_source_isolation
    PROPERTIES LABELS "ista_unit;ista_timing_edit" TIMEOUT 60
)
