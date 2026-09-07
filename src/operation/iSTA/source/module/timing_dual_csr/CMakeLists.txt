add_library(ista_timing_dual_csr
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTypes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrStateValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrArcGroupIndex.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrArcGroupIndexValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrReferenceValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrBaselineFactory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrDescriptorValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrUpdater.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrUpdatePhases.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrEdgeViewImpl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrResourceObservation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTimingPayloadViewImpl.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTimingResultValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTimingResultSemantics.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrFunctionalProjection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrTimingSemanticProjection.cpp
)

target_include_directories(ista_timing_dual_csr
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_timing_dual_csr
    PUBLIC
    ista_timing_csr_core
    ista_timing_edit_contract
)

target_compile_features(ista_timing_dual_csr PUBLIC cxx_std_20)
target_compile_options(ista_timing_dual_csr
    PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

# This target is DB-free. Do not add native STA, adapter, transaction, CUDA,
# pybind, or capture-time dependencies.

if(BUILD_TESTING)
    function(ista_add_timing_dual_csr_test target source)
        add_executable(${target}
            ${ISTA_TEST}/timing_dual_csr/${source}
        )
        target_include_directories(${target} PRIVATE ${ISTA_TEST})
        target_link_libraries(${target} PRIVATE ista_timing_dual_csr)
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
            LABELS "ista_unit;ista_timing_dual_csr"
        )
    endfunction()

    ista_add_timing_dual_csr_test(
        ista_timing_dual_csr_baseline_view_test
        DualCsrBaselineAndViewTest.cpp)
    ista_add_timing_dual_csr_test(
        ista_timing_dual_csr_updater_contract_test
        DualCsrUpdaterContractTest.cpp)
    ista_add_timing_dual_csr_test(
        ista_timing_dual_csr_aux_microcase_test
        DualCsrAuxMicrocaseTest.cpp)
    ista_add_timing_dual_csr_test(
        ista_timing_dual_csr_property_test
        DualCsrPropertyTest.cpp)
    ista_add_timing_dual_csr_test(
        ista_timing_dual_csr_functional_semantic_receipt_test
        DualCsrFunctionalSemanticReceiptTest.cpp)
endif()

if(BUILD_TESTING)
  ista_diagnostic_tests(
      ista_timing_dual_csr_aux_microcase_test
  )
endif()
