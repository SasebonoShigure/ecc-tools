add_library(ista_interface
    ${ISTA_INTERFACE}/IncrementalStaSession.cpp
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
    ista_timing_graph_csr_adapter
    idm
    liberty
    spef
)

target_include_directories(ista_interface
    PUBLIC
    ${ISTA_INTERFACE}
)

if(ISTA_ENABLE_CUDA)
  target_link_libraries(ista_interface PRIVATE ista_cuda)
endif()
