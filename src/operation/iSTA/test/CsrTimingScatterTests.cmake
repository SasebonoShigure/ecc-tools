# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Copyright (c) 2023-2025 Institute of Computing Technology, Chinese Academy of Sciences
# Copyright (c) 2023-2025 Beijing Institute of Open Source Chip
# Licensed under Mulan PSL v2.

set(ISTA_STEP6_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)
set(ISTA_STEP6_FUNCTIONAL_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)
set(ISTA_STEP6_RERUN_LINKS
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_source
    idm
)

function(ista_add_step6_target TARGET_NAME SOURCES_VARIABLE LINKS_VARIABLE)
  add_executable("${TARGET_NAME}" ${${SOURCES_VARIABLE}})
  target_include_directories("${TARGET_NAME}" PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
  target_link_libraries("${TARGET_NAME}" PRIVATE ${${LINKS_VARIABLE}})
  target_compile_options("${TARGET_NAME}" PRIVATE ${ISTA_STEP6_STRICT_OPTIONS})
endfunction()

set(ISTA_STEP6_COMMON_SOURCES
    CsrNativeSemanticKeyTestSupport.hpp
    CsrNativeTimingOracleTestSupport.hpp
    CsrTimingScatterTestFixture.hpp
    CsrReferenceInputTestFixture.cpp
    CsrReferenceInputNativeFixture.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrNativeTimingOracleIndependentEncoding.cpp
    CsrTimingScatterTestFixture.cpp
)

set(ISTA_STEP6_NATIVE_ORACLE_SOURCES
    ${ISTA_STEP6_COMMON_SOURCES}
    CsrNativeTimingOracleContractTest.cpp
    real_differential/CsrRealDifferentialArcIdentityTest.cpp
    real_differential/CsrRealDifferentialOracleEncoding.cpp
    real_differential/CsrRealDifferentialWire.cpp
    CsrNativeGraphRawProjector.cpp
)
ista_add_step6_target(
    ista_csr_native_timing_oracle_contract_test
    ISTA_STEP6_NATIVE_ORACLE_SOURCES
    ISTA_STEP6_FUNCTIONAL_LINKS
)

set(ISTA_STEP6_SCATTER_CONTRACT_SOURCES
    ${ISTA_STEP6_COMMON_SOURCES}
    CsrTimingResultFromNativeOracle.cpp
    CsrTimingScatterContractTest.cpp
    CsrTimingScatterStaleContract.cpp
    CsrResolvedTimingScatterContract.cpp
)
ista_add_step6_target(
    ista_csr_timing_scatter_contract_test
    ISTA_STEP6_SCATTER_CONTRACT_SOURCES
    ISTA_STEP6_FUNCTIONAL_LINKS
)
target_link_libraries(ista_csr_timing_scatter_contract_test PRIVATE ista_timing_edit_native)

set(ISTA_STEP6_SCATTER_ATOMICITY_SOURCES
    ${ISTA_STEP6_COMMON_SOURCES}
    CsrTimingResultFromNativeOracle.cpp
    CsrTimingScatterAtomicityContract.cpp
)
ista_add_step6_target(
    ista_csr_timing_scatter_atomicity_test
    ISTA_STEP6_SCATTER_ATOMICITY_SOURCES
    ISTA_STEP6_FUNCTIONAL_LINKS
)

set(ISTA_STEP6_SCATTER_PROPERTY_SOURCES
    ${ISTA_STEP6_COMMON_SOURCES}
    CsrTimingResultFromNativeOracle.cpp
    CsrTimingScatterPropertyTest.cpp
)
ista_add_step6_target(
    ista_csr_timing_scatter_property_test
    ISTA_STEP6_SCATTER_PROPERTY_SOURCES
    ISTA_STEP6_FUNCTIONAL_LINKS
)

set(ISTA_STEP6_PUBLIC_HEADER_SOURCES
    CsrTimingScatterPublicHeadersMain.cpp
    CsrTimingScatterHeaderIncludeOrderTest.cpp
    CsrTimingScatterHeaderReverseIncludeOrderTest.cpp
    public_header/CsrNativeTimingOracleHeaderTest.cpp
    public_header/TimingResultScatterHeaderTest.cpp
)
ista_add_step6_target(
    ista_csr_timing_scatter_public_headers_test
    ISTA_STEP6_PUBLIC_HEADER_SOURCES
    ISTA_STEP6_FUNCTIONAL_LINKS
)

set(ISTA_STEP6_RERUN_SOURCES
    ${ISTA_STEP6_COMMON_SOURCES}
    CsrTimingResultFromNativeOracle.cpp
    timing_scatter/StaCsrTimingScatterRerunProbe.cpp
)
ista_add_step6_target(
    ista_csr_timing_scatter_rerun_probe
    ISTA_STEP6_RERUN_SOURCES
    ISTA_STEP6_RERUN_LINKS
)

foreach(STEP6_UNIT_TARGET IN ITEMS
        ista_csr_native_timing_oracle_contract_test
        ista_csr_timing_scatter_contract_test
        ista_csr_timing_scatter_atomicity_test
        ista_csr_timing_scatter_public_headers_test)
  add_test(NAME "${STEP6_UNIT_TARGET}" COMMAND "${STEP6_UNIT_TARGET}")
  set_tests_properties(
      "${STEP6_UNIT_TARGET}" PROPERTIES LABELS "ista_unit" TIMEOUT 60
  )
endforeach()

set(ISTA_STEP6_PROPERTY_SEEDS 1 7 42 20260820 0xdeadbeef)
foreach(PROPERTY_SEED IN LISTS ISTA_STEP6_PROPERTY_SEEDS)
  add_test(
      NAME "ista_csr_timing_scatter_property_seed_${PROPERTY_SEED}"
      COMMAND ista_csr_timing_scatter_property_test --seed "${PROPERTY_SEED}"
  )
  set_tests_properties(
      "ista_csr_timing_scatter_property_seed_${PROPERTY_SEED}"
      PROPERTIES LABELS "ista_property" TIMEOUT 60
  )
endforeach()

add_test(
    NAME ista_csr_timing_scatter_rerun
    COMMAND
    "${CMAKE_COMMAND}" -E env
    "PYTHONDONTWRITEBYTECODE=1"
    "${ISTA_TEST_PYTHON_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_scatter/timing_scatter_controller.py"
    --probe "$<TARGET_FILE:ista_csr_timing_scatter_rerun_probe>"
)
set_tests_properties(
    ista_csr_timing_scatter_rerun
    PROPERTIES LABELS "ista_smoke" TIMEOUT 300
)

add_executable(ista_csr_report_contract_test CsrReportContractTest.cpp
    ${ISTA_STEP6_COMMON_SOURCES} CsrTimingResultFromNativeOracle.cpp
    timing_dual_csr/DualCsrTargetedArtifactSupport.cpp
    timing_edit/oracle/ProductionFreshSessionLoader.cpp
    timing_edit/StaTimingEditSessionProbeAccess.cpp)
target_include_directories(ista_csr_report_contract_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/timing_edit
    ${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle ${CMAKE_CURRENT_LIST_DIR}/timing_dual_csr)
target_link_libraries(ista_csr_report_contract_test PRIVATE
    ${ISTA_DUAL_CSR_FRESH_PROBE_LINKS} ista_timing_analyzer ista_timing_reporter)
target_compile_options(ista_csr_report_contract_test PRIVATE ${ISTA_DUAL_CSR_TRACE_STRICT_OPTIONS})
foreach(trace IN ITEMS synthetic_two_batch real_buffer_fanout)
  add_test(NAME ista_csr_report_${trace} COMMAND ista_csr_report_contract_test
      ${CMAKE_CURRENT_LIST_DIR}/timing_edit/traces/${trace}.json)
  set_tests_properties(ista_csr_report_${trace} PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/timing_edit"
      LABELS "ista_cpu;ista_integration" TIMEOUT 300 RUN_SERIAL TRUE)
endforeach()
