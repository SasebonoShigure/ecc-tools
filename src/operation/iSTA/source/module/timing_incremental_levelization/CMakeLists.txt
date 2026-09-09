add_library(ista_timing_incremental_levelization
    ${CMAKE_CURRENT_SOURCE_DIR}/MixedLevelizationTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MixedLevelizationGraphBuilder.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CorrectedMixedKahnLevelizer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MixedLevelReconstructor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MixedLevelizationValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ActualDagCycleClassifier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CpuIncrementalLevelizationPipeline.cpp
)

target_include_directories(ista_timing_incremental_levelization
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_timing_incremental_levelization
    PUBLIC
    ista_timing_csr_core
)

target_compile_features(ista_timing_incremental_levelization PUBLIC cxx_std_20)
target_compile_options(ista_timing_incremental_levelization PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

# This target is deliberately DB-free. Do not add native STA, transaction,
# updater, CUDA, pybind, capture-time, or fresh-oracle dependencies.

if(BUILD_TESTING)
    function(ista_add_incremental_levelization_test target source)
        add_executable(${target}
            ${ISTA_TEST}/timing_incremental_levelization/${source}
        )
        target_include_directories(${target} PRIVATE
            ${ISTA_TEST}/timing_incremental_levelization
        )
        target_link_libraries(${target} PRIVATE
            ista_timing_incremental_levelization
        )
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Werror
            -pedantic
            -ffp-contract=off
        )
        add_test(NAME ${target} COMMAND ${target})
        set_tests_properties(${target} PROPERTIES
            LABELS "ista_unit;ista_incremental_levelization;ista_timing_incremental_levelization"
        )
    endfunction()

    ista_add_incremental_levelization_test(
        ista_mixed_topology_test MixedTopologyTest.cpp)
    ista_add_incremental_levelization_test(
        ista_mixed_id_mapping_test MixedIdMappingTest.cpp)
    ista_add_incremental_levelization_test(
        ista_mixed_graph_builder_test MixedGraphBuilderTest.cpp)
    ista_add_incremental_levelization_test(
        ista_corrected_mixed_kahn_test CorrectedMixedKahnTest.cpp)
    ista_add_incremental_levelization_test(
        ista_mixed_reconstruction_test MixedReconstructionTest.cpp)
    ista_add_incremental_levelization_test(
        ista_incremental_levelization_pipeline_test
        IncrementalLevelizationPipelineTest.cpp)
    ista_add_incremental_levelization_test(
        ista_incremental_levelization_property_test
        IncrementalLevelizationPropertyTest.cpp)
endif()

if(BUILD_TESTING)
  ista_diagnostic_tests(
      ista_mixed_graph_builder_test
      ista_incremental_levelization_property_test
  )
endif()
