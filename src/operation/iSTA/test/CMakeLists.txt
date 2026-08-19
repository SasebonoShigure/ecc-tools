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
