add_library(ista_timing_edit_native
    NativeBufferCellValidation.cpp
    NativeDatabaseCandidate.cpp
    NativeDatabaseValidation.cpp
    NativeCandidateExecutor.cpp
    TimingEditResolver.cpp
    NativeBufferInsertion.cpp
    NativeDesignEditTranslator.cpp
    NativeTimingEditPreparer.cpp
    NativeTargetedEditPreparer.cpp
    NativeTargetedEditProjection.cpp
    NativeTimingEditTraceClosure.cpp
)

target_include_directories(ista_timing_edit_native
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ISTA_MODULE}/timing_cuda
)

target_link_libraries(ista_timing_edit_native
    PUBLIC
    ista_timing_edit_contract
    ista_timing_dual_csr
    PRIVATE
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
    ista_data_manager
    ista_graph_builder
    ista_delay_calculator
    ista_clock_propagator
    ista_timing_propagator
    ista_timing_analyzer
)

target_compile_options(ista_timing_edit_native PRIVATE
    -Wall -Wextra -Werror -pedantic -ffp-contract=off)
