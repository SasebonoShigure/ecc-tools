if(BUILD_TESTING)
  if(CMAKE_VERSION VERSION_LESS 3.12)
    find_package(PythonInterp 3.11 REQUIRED)
    set(ISTA_TEST_PYTHON_EXECUTABLE "${PYTHON_EXECUTABLE}")
  else()
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    set(ISTA_TEST_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
  endif()

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
      ista_incremental_samples_schema_validation PROPERTIES LABELS "ista_unit"
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
      PROPERTIES LABELS "ista_unit"
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
  target_link_libraries(ista_incremental_session_test PRIVATE ista_interface)
  add_test(NAME ista_incremental_session_test COMMAND ista_incremental_session_test)
  set_tests_properties(ista_incremental_session_test PROPERTIES LABELS "ista_unit")

  add_executable(ista_production_lifecycle_test
      StaProductionLifecycleTest.cpp
  )
  target_link_libraries(ista_production_lifecycle_test PRIVATE ista_interface)
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

  add_executable(ista_full_baseline_probe
      baseline/StaFullBaselineProbe.cpp
  )
  target_link_libraries(ista_full_baseline_probe
      PRIVATE
      ista_interface
      ista_source
      idm
  )

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
  )
endif()
