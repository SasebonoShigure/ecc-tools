if(BUILD_TESTING)
  if(CMAKE_VERSION VERSION_LESS 3.12)
    find_package(PythonInterp 3.11 REQUIRED)
    set(ISTA_TEST_PYTHON_EXECUTABLE "${PYTHON_EXECUTABLE}")
  else()
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    set(ISTA_TEST_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
  endif()

  function(ista_get_target_property_or_empty OUTPUT TARGET_NAME PROPERTY_NAME)
    get_target_property(PROPERTY_VALUE "${TARGET_NAME}" "${PROPERTY_NAME}")
    if("${PROPERTY_VALUE}" MATCHES "-NOTFOUND$")
      set(PROPERTY_VALUE)
    endif()
    set("${OUTPUT}" "${PROPERTY_VALUE}" PARENT_SCOPE)
  endfunction()

  function(ista_assert_unique_list LIST_VARIABLE DESCRIPTION)
    set(SEEN_ITEMS)
    foreach(ITEM IN LISTS ${LIST_VARIABLE})
      list(FIND SEEN_ITEMS "${ITEM}" SEEN_INDEX)
      if(NOT SEEN_INDEX EQUAL -1)
        message(FATAL_ERROR "duplicate ${DESCRIPTION}: ${ITEM}")
      endif()
      list(APPEND SEEN_ITEMS "${ITEM}")
    endforeach()
  endfunction()

  function(ista_assert_same_inventory ACTUAL_VARIABLE EXPECTED_VARIABLE DESCRIPTION)
    ista_assert_unique_list(${ACTUAL_VARIABLE} "actual ${DESCRIPTION}")
    ista_assert_unique_list(${EXPECTED_VARIABLE} "expected ${DESCRIPTION}")
    set(ACTUAL_SORTED ${${ACTUAL_VARIABLE}})
    set(EXPECTED_SORTED ${${EXPECTED_VARIABLE}})
    list(SORT ACTUAL_SORTED)
    list(SORT EXPECTED_SORTED)
    if(NOT "${ACTUAL_SORTED}" STREQUAL "${EXPECTED_SORTED}")
      message(
          FATAL_ERROR
          "${DESCRIPTION} mismatch; actual=[${ACTUAL_SORTED}] expected=[${EXPECTED_SORTED}]"
      )
    endif()
  endfunction()

  add_executable(ista_arc_timing_arc_delay_test
      ArcTimingArcDelayTest.cpp
  )
  target_link_libraries(ista_arc_timing_arc_delay_test
      PRIVATE
      ista_data_manager
  )
  add_test(NAME ista_arc_timing_arc_delay_test COMMAND ista_arc_timing_arc_delay_test)
  set_tests_properties(ista_arc_timing_arc_delay_test PROPERTIES LABELS "ista_unit")

  add_test(
      NAME ista_incremental_samples_schema_validation
      COMMAND
      "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/schema/validate_incremental_sta_samples.py"
      --self-test
  )
  set_tests_properties(
      ista_incremental_samples_schema_validation
      PROPERTIES LABELS "ista_unit"
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
  )

  add_test(
      NAME ista_timing_edit_trace_schema_validation
      COMMAND
      "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/schema/validate_timing_edit_trace.py"
      --schema
      "${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/schema/timing_edit_trace_v1.schema.json"
      --self-test
  )
  set_tests_properties(
      ista_timing_edit_trace_schema_validation
      PROPERTIES
      LABELS "ista_unit;ista_timing_edit"
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
  )

  add_test(
      NAME ista_csr_snapshot_schema_validation
      COMMAND
      "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/schema/validate_csr_snapshot_observation.py"
      --schema "${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/schema/csr_snapshot_observation_v1.schema.json"
      --manifest "${CMAKE_CURRENT_SOURCE_DIR}/../benchmark/schema/csr_snapshot_contract_manifest_v1.json"
      --self-test
  )
  set_tests_properties(
      ista_csr_snapshot_schema_validation
      PROPERTIES
      LABELS "ista_unit"
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
  )

  add_test(
      NAME ista_full_baseline_controller_validation
      COMMAND
      "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/baseline/run_baseline.py"
      --manifest "${CMAKE_CURRENT_SOURCE_DIR}/baseline/manifest.json"
      --self-test
  )
  set_tests_properties(
      ista_full_baseline_controller_validation
      PROPERTIES
      LABELS "ista_unit"
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
  )

  add_test(
      NAME ista_csr_projection_controller_validation
      COMMAND
      "${CMAKE_COMMAND}" -E env
      "PYTHONDONTWRITEBYTECODE=1"
      "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/projection/projection_controller.py"
      --manifest "${CMAKE_CURRENT_SOURCE_DIR}/baseline/manifest.json"
      --self-test
  )
  set_tests_properties(
      ista_csr_projection_controller_validation PROPERTIES LABELS "ista_unit"
  )

  add_executable(ista_execution_contract_test
      StaExecutionContractTest.cpp
  )
  target_link_libraries(ista_execution_contract_test PRIVATE ista_interface)
  add_test(NAME ista_execution_contract_test COMMAND ista_execution_contract_test)
  set_tests_properties(ista_execution_contract_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_lifecycle_guard_test
      StaLifecycleGuardTest.cpp
  )
  target_link_libraries(ista_lifecycle_guard_test PRIVATE ista_interface)
  add_test(NAME ista_lifecycle_guard_test COMMAND ista_lifecycle_guard_test)
  set_tests_properties(ista_lifecycle_guard_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_incremental_session_test
      IncrementalStaSessionTest.cpp
  )
  target_link_libraries(ista_incremental_session_test PRIVATE ista_interface ista_timing_graph_csr_adapter)
  add_test(NAME ista_incremental_session_test COMMAND ista_incremental_session_test)
  set_tests_properties(ista_incremental_session_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_production_lifecycle_test
      StaProductionLifecycleTest.cpp
  )
  target_link_libraries(ista_production_lifecycle_test PRIVATE ista_interface ista_timing_graph_csr_adapter)
  set(ISTA_PRODUCTION_LIFECYCLE_SCENARIOS
      owner_interop
      init_work_failure
      typed_init_cleanup_failure
      legacy_init_cleanup_failure
      typed_run_work_failure
      typed_run_cleanup_failure
      legacy_work_failures
      legacy_cleanup_run
      legacy_cleanup_extract
      legacy_cleanup_update
      legacy_cleanup_report
      legacy_cleanup_write
      typed_close_cleanup_failure
      legacy_close_cleanup_failure
      typed_init_throw
      legacy_init_throw
      typed_init_throw_cleanup_throw
      legacy_init_throw_cleanup_throw
      typed_run_throw
      legacy_throw_run
      legacy_throw_extract
      legacy_throw_update
      legacy_throw_report
      legacy_throw_write
      typed_close_throw
      legacy_close_throw
  )
  foreach(SCENARIO IN LISTS ISTA_PRODUCTION_LIFECYCLE_SCENARIOS)
    add_test(
        NAME "ista_production_lifecycle_${SCENARIO}"
        COMMAND ista_production_lifecycle_test "${SCENARIO}"
    )
    set_tests_properties(
        "ista_production_lifecycle_${SCENARIO}" PROPERTIES LABELS "ista_unit"
    )
  endforeach()

  add_executable(ista_graph_builder_state_test
      GraphBuilderStateTest.cpp
  )
  target_link_libraries(ista_graph_builder_state_test
      PRIVATE
      ista_source
  )
  add_test(NAME ista_graph_builder_state_test COMMAND ista_graph_builder_state_test)
  set_tests_properties(ista_graph_builder_state_test PROPERTIES LABELS "ista_unit")

  include("${CMAKE_CURRENT_SOURCE_DIR}/TimingEditTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/TimingEditOracleTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/BufferTraceTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTests.cmake")

  include("${CMAKE_CURRENT_SOURCE_DIR}/CsrTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/CsrCpuReferenceTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/CsrReferenceInputTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/CsrReferenceInputIsolationTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/CsrTimingScatterTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/CsrIsolationTests.cmake")

  add_executable(ista_full_baseline_probe
      baseline/StaFullBaselineProbe.cpp
  )
  target_link_libraries(ista_full_baseline_probe
      PRIVATE
      ista_timing_graph_csr_adapter
      ista_interface
      ista_source
      idm
  )

  add_executable(ista_csr_projection_probe
      projection/StaCsrProjectionProbe.cpp
      CsrNativeSemanticKeyTestSupport.hpp
      CsrNativeGraphRawProjector.cpp
      CsrNativeGraphOracleReader.cpp
      CsrNativeGraphIndependentEncoding.cpp
  )
  target_include_directories(ista_csr_projection_probe
      PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_link_libraries(ista_csr_projection_probe
      PRIVATE
      ista_timing_graph_csr_adapter
      ista_interface
      ista_source
      idm
  )

  include("${CMAKE_CURRENT_SOURCE_DIR}/CsrSessionTests.cmake")
  include("${CMAKE_CURRENT_SOURCE_DIR}/CsrRealDifferentialTests.cmake")

  if(TARGET ecc_py)
    add_test(
        NAME ista_python_binding_contract
        COMMAND
        "${CMAKE_COMMAND}" -E env
        "PYTHONPATH=$<TARGET_FILE_DIR:ecc_py>"
        "${ISTA_TEST_PYTHON_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/PythonBindingContractTest.py"
    )
    set_tests_properties(
        ista_python_binding_contract
        PROPERTIES LABELS "ista_integration;ista_smoke"
    )
  endif()

  add_test(
      NAME ista_full_baseline
      COMMAND
      "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/baseline/run_baseline.py"
      --probe "$<TARGET_FILE:ista_full_baseline_probe>"
      --manifest "${CMAKE_CURRENT_SOURCE_DIR}/baseline/manifest.json"
      --output-root "${CMAKE_CURRENT_BINARY_DIR}/baseline-runs"
      --repeats 3
  )
  set_tests_properties(
      ista_full_baseline
      PROPERTIES
      LABELS "ista_integration;ista_cpu;ista_smoke"
      TIMEOUT 300
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
  )

  add_test(
      NAME ista_csr_projection_fresh_process
      COMMAND
      "${CMAKE_COMMAND}" -E env
      "PYTHONDONTWRITEBYTECODE=1"
      "${ISTA_TEST_PYTHON_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/projection/projection_controller.py"
      --probe "$<TARGET_FILE:ista_csr_projection_probe>"
      --manifest "${CMAKE_CURRENT_SOURCE_DIR}/baseline/manifest.json"
      --run-root "${CMAKE_CURRENT_BINARY_DIR}/projection-runs"
      --repeats 3
      --timeout-seconds 300
  )
  set_tests_properties(
      ista_csr_projection_fresh_process
      PROPERTIES
      LABELS "ista_integration;ista_cpu;ista_smoke"
      TIMEOUT 600
  )
endif()
