set(ISTA_TIMING_GRAPH_CSR_ADAPTER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/CanonicalTimingRecordProjector.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CanonicalTimingRecordProjectorInternal.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrGraphEncoding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrGraphValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingGraphCsrAdapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingGraphCsrArcExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingGraphCsrAdjacency.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NativeGraphProjection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrReferenceInputAdapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingResultScatter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingResultScatterValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingResultScatterPreparation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrReportNodeReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/DualCsrNativeResultPreparation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingResultScatterPublish.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrConstraintExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrClockSeedExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrTimingCheckExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrLibertyExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrLoadExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrParasiticExport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrSessionSnapshotAdapter.cpp
)

add_library(ista_timing_graph_csr_adapter
    ${ISTA_TIMING_GRAPH_CSR_ADAPTER_SOURCES}
)

target_include_directories(ista_timing_graph_csr_adapter
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_timing_graph_csr_adapter
    PUBLIC
    ista_timing_csr_core
    ista_timing_csr_snapshot_contract
    PRIVATE
    ista_timing_database_headers
    ista_timing_dual_csr
)

target_compile_options(ista_timing_graph_csr_adapter PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)
