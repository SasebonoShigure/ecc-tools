# Copyright (c) 2023-2025 Peng Cheng Laboratory
# Licensed under Mulan PSL v2.

foreach(REQUIRED IN ITEMS TEST_ROOT ORACLE_SOURCES_PACKED ORACLE_LINKS_PACKED ORACLE_CLOSURE_PACKED)
  if(NOT DEFINED ${REQUIRED})
    message(FATAL_ERROR "missing ${REQUIRED}")
  endif()
endforeach()

string(REPLACE "|" ";" ACTUAL_SOURCES "${ORACLE_SOURCES_PACKED}")
string(REPLACE "|" ";" ACTUAL_LINKS "${ORACLE_LINKS_PACKED}")
string(REPLACE "|" ";" ACTUAL_CLOSURE "${ORACLE_CLOSURE_PACKED}")
set(EXPECTED_SOURCES
  timing_edit/oracle/IndependentFreshBaselineLoader.cpp
  timing_edit/oracle/IndependentSemanticReplayer.cpp
  timing_edit/oracle/TimingEditOracleCli.cpp
  timing_edit/oracle/IndependentOracleObservation.cpp
  timing_edit/oracle/IndependentFreshOracleProbe.cpp
)
set(EXPECTED_LINKS
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

list(SORT ACTUAL_SOURCES)
list(SORT EXPECTED_SOURCES)
list(SORT ACTUAL_LINKS)
list(SORT EXPECTED_LINKS)
if(NOT "${ACTUAL_SOURCES}" STREQUAL "${EXPECTED_SOURCES}")
  message(FATAL_ERROR "independent oracle source inventory differs: ${ACTUAL_SOURCES}")
endif()
if(NOT "${ACTUAL_LINKS}" STREQUAL "${EXPECTED_LINKS}")
  message(FATAL_ERROR "independent oracle direct link inventory differs: ${ACTUAL_LINKS}")
endif()

string(JOIN "|" CLOSURE_TEXT ${ACTUAL_CLOSURE})
foreach(FORBIDDEN IN ITEMS ista_timing_edit_native ista_timing_edit_transaction)
  string(FIND "${CLOSURE_TEXT}" "${FORBIDDEN}" FORBIDDEN_OFFSET)
  if(NOT FORBIDDEN_OFFSET EQUAL -1)
    message(FATAL_ERROR "independent oracle link closure contains forbidden target ${FORBIDDEN}")
  endif()
endforeach()

foreach(SOURCE IN LISTS ACTUAL_SOURCES)
  set(PATH "${TEST_ROOT}/${SOURCE}")
  if(NOT EXISTS "${PATH}")
    message(FATAL_ERROR "independent oracle source is absent: ${PATH}")
  endif()
  file(READ "${PATH}" CONTENT)
  foreach(FORBIDDEN IN ITEMS NativeCandidateExecutor NativeDesignEditTranslator TimingEditResolver TimingEditCoordinator PreparedBatch)
    string(FIND "${CONTENT}" "${FORBIDDEN}" FORBIDDEN_OFFSET)
    if(NOT FORBIDDEN_OFFSET EQUAL -1)
      message(FATAL_ERROR "${SOURCE} references forbidden production edit symbol ${FORBIDDEN}")
    endif()
  endforeach()
  if(NOT SOURCE MATCHES "IndependentFreshBaselineLoader\\.cpp$")
    foreach(NARROW_DEP IN ITEMS
            STAInterface.hpp
            IncrementalStaSession.hpp
            DataManager.hpp
            idm.h
            STADM
            dmInst)
      string(FIND "${CONTENT}" "${NARROW_DEP}" NARROW_OFFSET)
      if(NOT NARROW_OFFSET EQUAL -1)
        message(FATAL_ERROR "${SOURCE} bypasses the unique fresh-baseline loader via ${NARROW_DEP}")
      endif()
    endforeach()
    if(SOURCE MATCHES "IndependentFreshOracleProbe\\.cpp$")
      string(FIND "${CONTENT}" "IndependentFreshBaselineLoader::run" LOADER_USE_OFFSET)
      if(LOADER_USE_OFFSET EQUAL -1)
        message(FATAL_ERROR
            "IndependentFreshOracleProbe.cpp exports CSR state without entering the unique fresh-baseline loader")
      endif()
    else()
      string(FIND "${CONTENT}" "TimingGraphCsrAdapter.hpp" ADAPTER_OFFSET)
      if(NOT ADAPTER_OFFSET EQUAL -1)
        message(FATAL_ERROR
            "${SOURCE} bypasses the unique fresh-baseline loader via TimingGraphCsrAdapter.hpp")
      endif()
    endif()
  endif()
endforeach()

message(STATUS "independent oracle source/link closure is isolated from production edit implementation")
