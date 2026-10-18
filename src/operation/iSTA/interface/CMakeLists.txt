add_library(ista_interface
    ${ISTA_INTERFACE}/IncrementalStaSession.cpp
    ${ISTA_INTERFACE}/IncrementalStaSessionPhysical.cpp
    ${ISTA_INTERFACE}/StaEditOperation.cpp
    ${ISTA_INTERFACE}/PhysicalTimingEdit.cpp
    ${ISTA_INTERFACE}/PhysicalStaBatchJson.cpp
    ${ISTA_INTERFACE}/PhysicalStaJournal.cpp
    ${ISTA_INTERFACE}/PhysicalStaArtifacts.cpp
    ${ISTA_INTERFACE}/PhysicalGreedySelection.cpp
    ${ISTA_INTERFACE}/PhysicalGreedyScoring.cpp
    ${ISTA_INTERFACE}/PhysicalGreedyCsrSelection.cpp
    ${ISTA_INTERFACE}/PhysicalStaSessionState.cpp
    ${ISTA_INTERFACE}/PhysicalStaNativeView.cpp
    ${ISTA_INTERFACE}/SemanticCsrNativeView.cpp
    ${ISTA_INTERFACE}/PhysicalStaSessionRecovery.cpp
    ${ISTA_INTERFACE}/PhysicalStaSessionBatch.cpp
    ${ISTA_INTERFACE}/STAInterfacePhysical.cpp
    ${ISTA_INTERFACE}/StaExecutionContract.cpp
    ${ISTA_INTERFACE}/StaLifecycleGuard.cpp
    ${ISTA_INTERFACE}/StaProductionLifecycle.cpp
    ${ISTA_INTERFACE}/StaCsrSessionContext.cpp
    ${ISTA_INTERFACE}/STAInterface.cpp
    ${ISTA_INTERFACE}/STAInterfaceOperations.cpp
    ${ISTA_INTERFACE}/STAInterfaceCuda.cpp
)

target_link_libraries(ista_interface
    PRIVATE
    ista_source
    ista_timing_edit_native
    izh_timing_optimization
    ista_timing_graph_csr_adapter
    ista_timing_reporter
    idm
    liberty
    spef
)

target_include_directories(ista_interface
    PUBLIC
    ${ISTA_INTERFACE}
)

if(ISTA_ENABLE_CUDA)
  target_sources(ista_interface PRIVATE CudaCsrEditState.cpp StaCudaSessionState.cpp CudaTargetedExecution.cpp)
  target_link_libraries(ista_interface PRIVATE ista_cuda ista_timing_dual_csr_native)
endif()

set_source_files_properties(
    CudaCsrEditState.cpp CudaTargetedExecution.cpp StaEditOperation.cpp PhysicalTimingEdit.cpp PhysicalStaSessionState.cpp PhysicalStaJournal.cpp PhysicalStaBatchJson.cpp
    PhysicalStaArtifacts.cpp PhysicalStaNativeView.cpp SemanticCsrNativeView.cpp PhysicalGreedySelection.cpp PhysicalGreedyScoring.cpp PhysicalGreedyCsrSelection.cpp STAInterfacePhysical.cpp IncrementalStaSessionPhysical.cpp PhysicalStaSessionRecovery.cpp PhysicalStaSessionBatch.cpp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-pedantic;-ffp-contract=off")
