# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Licensed under Mulan PSL v2.

set(ISTA_TIMING_EDIT_ORACLE_STRICT_OPTIONS
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)
set(ISTA_TIMING_EDIT_ORACLE_SOURCES
    timing_edit/oracle/IndependentFreshBaselineLoader.cpp
    timing_edit/oracle/IndependentSemanticReplayer.cpp
    timing_edit/oracle/TimingEditOracleCli.cpp
    timing_edit/oracle/IndependentOracleObservation.cpp
    timing_edit/oracle/IndependentFreshOracleProbe.cpp
)
set(ISTA_TIMING_EDIT_ORACLE_LINKS
    ista_timing_edit_contract
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_graph_builder
    ista_delay_calculator
    ista_clock_propagator
    ista_timing_propagator
    ista_timing_analyzer
    ista_interface
    ista_source
    idm
)

add_executable(ista_timing_edit_independent_oracle_probe ${ISTA_TIMING_EDIT_ORACLE_SOURCES})
target_include_directories(
    ista_timing_edit_independent_oracle_probe
    PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle"
)
target_link_libraries(ista_timing_edit_independent_oracle_probe PRIVATE ${ISTA_TIMING_EDIT_ORACLE_LINKS})
target_compile_options(ista_timing_edit_independent_oracle_probe PRIVATE ${ISTA_TIMING_EDIT_ORACLE_STRICT_OPTIONS})

set(ISTA_TIMING_EDIT_PRODUCTION_ORACLE_SOURCES
    timing_edit/StaTimingEditSessionProbeAccess.cpp
    timing_edit/StaTimingEditCsrReceiptProbe.cpp
    timing_edit/oracle/TimingEditOracleCli.cpp
    timing_edit/oracle/IndependentOracleObservation.cpp
    timing_edit/oracle/ProductionFreshSessionLoader.cpp
    timing_edit/oracle/ProductionFreshOracleProbe.cpp
    CsrNativeTimingOracleReader.cpp
    CsrNativeTimingOracleNodeReader.cpp
    CsrNativeTimingOracleArcReader.cpp
    CsrTimingResultFromNativeOracle.cpp
)
set(ISTA_TIMING_EDIT_PRODUCTION_ORACLE_LINKS
    ista_timing_edit_transaction
    ista_timing_edit_native
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_interface
    ista_source
    ista_data_manager
    ista_delay_calculator
    idm
)
add_executable(ista_timing_edit_production_oracle_probe
    ${ISTA_TIMING_EDIT_PRODUCTION_ORACLE_SOURCES})
target_include_directories(ista_timing_edit_production_oracle_probe PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit"
    "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle"
)
target_link_libraries(ista_timing_edit_production_oracle_probe
    PRIVATE ${ISTA_TIMING_EDIT_PRODUCTION_ORACLE_LINKS})
target_compile_options(ista_timing_edit_production_oracle_probe
    PRIVATE ${ISTA_TIMING_EDIT_ORACLE_STRICT_OPTIONS})

get_target_property(ISTA_TIMING_EDIT_ORACLE_ACTUAL_SOURCES ista_timing_edit_independent_oracle_probe SOURCES)
ista_assert_same_inventory(
    ISTA_TIMING_EDIT_ORACLE_ACTUAL_SOURCES
    ISTA_TIMING_EDIT_ORACLE_SOURCES
    "Stage 3 independent oracle source inventory"
)
ista_get_target_property_or_empty(
    ISTA_TIMING_EDIT_ORACLE_ACTUAL_LINKS
    ista_timing_edit_independent_oracle_probe
    LINK_LIBRARIES
)
ista_assert_same_inventory(
    ISTA_TIMING_EDIT_ORACLE_ACTUAL_LINKS
    ISTA_TIMING_EDIT_ORACLE_LINKS
    "Stage 3 independent oracle direct links"
)
ista_get_target_property_or_empty(
    ISTA_TIMING_EDIT_ORACLE_ACTUAL_OPTIONS
    ista_timing_edit_independent_oracle_probe
    COMPILE_OPTIONS
)
ista_assert_same_inventory(
    ISTA_TIMING_EDIT_ORACLE_ACTUAL_OPTIONS
    ISTA_TIMING_EDIT_ORACLE_STRICT_OPTIONS
    "Stage 3 independent oracle compile options"
)

get_target_property(ISTA_TIMING_EDIT_PRODUCTION_ORACLE_ACTUAL_SOURCES
    ista_timing_edit_production_oracle_probe SOURCES)
ista_assert_same_inventory(
    ISTA_TIMING_EDIT_PRODUCTION_ORACLE_ACTUAL_SOURCES
    ISTA_TIMING_EDIT_PRODUCTION_ORACLE_SOURCES
    "Stage 3 production oracle source inventory"
)
ista_get_target_property_or_empty(
    ISTA_TIMING_EDIT_PRODUCTION_ORACLE_ACTUAL_LINKS
    ista_timing_edit_production_oracle_probe
    LINK_LIBRARIES
)
ista_assert_same_inventory(
    ISTA_TIMING_EDIT_PRODUCTION_ORACLE_ACTUAL_LINKS
    ISTA_TIMING_EDIT_PRODUCTION_ORACLE_LINKS
    "Stage 3 production oracle direct links"
)
ista_get_target_property_or_empty(
    ISTA_TIMING_EDIT_PRODUCTION_ORACLE_ACTUAL_OPTIONS
    ista_timing_edit_production_oracle_probe
    COMPILE_OPTIONS
)
ista_assert_same_inventory(
    ISTA_TIMING_EDIT_PRODUCTION_ORACLE_ACTUAL_OPTIONS
    ISTA_TIMING_EDIT_ORACLE_STRICT_OPTIONS
    "Stage 3 production oracle compile options"
)

function(ista_collect_timing_edit_oracle_link_closure ROOT_TARGET OUTPUT_VARIABLE)
  set(QUEUE "${ROOT_TARGET}")
  set(VISITED)
  while(QUEUE)
    list(POP_FRONT QUEUE CURRENT_TARGET)
    if(CURRENT_TARGET IN_LIST VISITED OR NOT TARGET "${CURRENT_TARGET}")
      continue()
    endif()
    list(APPEND VISITED "${CURRENT_TARGET}")
    foreach(PROPERTY_NAME IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(DEPENDENCIES "${CURRENT_TARGET}" "${PROPERTY_NAME}")
      if("${DEPENDENCIES}" MATCHES "-NOTFOUND$")
        set(DEPENDENCIES)
      endif()
      foreach(RAW_DEPENDENCY IN LISTS DEPENDENCIES)
        if("${RAW_DEPENDENCY}" MATCHES "ista_timing_edit_native|ista_timing_edit_transaction")
          message(FATAL_ERROR
              "independent oracle closure contains forbidden dependency ${RAW_DEPENDENCY} via ${CURRENT_TARGET}")
        endif()
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

ista_collect_timing_edit_oracle_link_closure(
    ista_timing_edit_independent_oracle_probe
    ISTA_TIMING_EDIT_ORACLE_CLOSURE
)
foreach(PACKED_VARIABLE IN ITEMS
        ISTA_TIMING_EDIT_ORACLE_SOURCES
        ISTA_TIMING_EDIT_ORACLE_LINKS
        ISTA_TIMING_EDIT_ORACLE_CLOSURE)
  string(JOIN "|" ${PACKED_VARIABLE}_PACKED ${${PACKED_VARIABLE}})
endforeach()

add_test(
    NAME ista_timing_edit_independent_oracle_wrong_sink_selftest
    COMMAND ista_timing_edit_independent_oracle_probe --self-test-wrong-sink
)
set_tests_properties(
    ista_timing_edit_independent_oracle_wrong_sink_selftest
    PROPERTIES LABELS "ista_unit;ista_timing_edit" TIMEOUT 60
)

add_test(
    NAME ista_timing_edit_independent_oracle_source_inventory
    COMMAND
    "${CMAKE_COMMAND}"
    "-DTEST_ROOT=${CMAKE_CURRENT_LIST_DIR}"
    "-DORACLE_SOURCES_PACKED=${ISTA_TIMING_EDIT_ORACLE_SOURCES_PACKED}"
    "-DORACLE_LINKS_PACKED=${ISTA_TIMING_EDIT_ORACLE_LINKS_PACKED}"
    "-DORACLE_CLOSURE_PACKED=${ISTA_TIMING_EDIT_ORACLE_CLOSURE_PACKED}"
    -P "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle/CheckTimingEditOracleIsolation.cmake"
)
set_tests_properties(
    ista_timing_edit_independent_oracle_source_inventory
    PROPERTIES LABELS "ista_unit;ista_timing_edit" TIMEOUT 60
)

add_test(
    NAME ista_timing_edit_fresh_oracle_differential
    COMMAND
    "${CMAKE_COMMAND}"
    "-DORACLE_EXECUTABLE=$<TARGET_FILE:ista_timing_edit_independent_oracle_probe>"
    "-DPRODUCTION_EXECUTABLE=$<TARGET_FILE:ista_timing_edit_production_oracle_probe>"
    "-DPYTHON_EXECUTABLE=${ISTA_TEST_PYTHON_EXECUTABLE}"
    "-DTRACE_VALIDATOR=${CMAKE_CURRENT_LIST_DIR}/../benchmark/schema/validate_timing_edit_trace.py"
    "-DTRACE_SCHEMA=${CMAKE_CURRENT_LIST_DIR}/../benchmark/schema/timing_edit_trace_v1.schema.json"
    "-DTECH_LEF=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0_tech.lef"
    "-DCELL_LEF=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0_cells.lef"
    "-DLIBERTY=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0.lib"
    "-DVERILOG=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/chain.v"
    "-DSDC=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/chain.sdc"
    "-DRUN_ROOT=${CMAKE_CURRENT_BINARY_DIR}/timing-edit-oracle-runs"
    -P "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle/RunFreshOracleDifferential.cmake"
)
set_tests_properties(
    ista_timing_edit_fresh_oracle_differential
    PROPERTIES LABELS "ista_integration;ista_cpu;ista_smoke;ista_timing_edit" TIMEOUT 600 RUN_SERIAL TRUE
)

add_test(
    NAME ista_timing_edit_production_oracle_derived_tamper_rollback
    COMMAND
    "${CMAKE_COMMAND}"
    "-DORACLE_EXECUTABLE=$<TARGET_FILE:ista_timing_edit_independent_oracle_probe>"
    "-DPRODUCTION_EXECUTABLE=$<TARGET_FILE:ista_timing_edit_production_oracle_probe>"
    "-DTECH_LEF=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0_tech.lef"
    "-DCELL_LEF=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0_cells.lef"
    "-DLIBERTY=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/stage0.lib"
    "-DVERILOG=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/chain.v"
    "-DSDC=${CMAKE_CURRENT_LIST_DIR}/baseline/fixtures/chain.sdc"
    "-DRUN_ROOT=${CMAKE_CURRENT_BINARY_DIR}/timing-edit-derived-tamper-run"
    -P "${CMAKE_CURRENT_LIST_DIR}/timing_edit/oracle/RunProductionDerivedTamper.cmake"
)
set_tests_properties(
    ista_timing_edit_production_oracle_derived_tamper_rollback
    PROPERTIES
    LABELS "ista_integration;ista_cpu;ista_smoke;ista_timing_edit"
    TIMEOUT 300
    RUN_SERIAL TRUE
)
