add_library(ista_timing_csr_snapshot_contract
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrSnapshotStorage.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrSnapshotObservationValidator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrSnapshotObservationTransition.cpp
)

target_include_directories(ista_timing_csr_snapshot_contract
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_timing_csr_snapshot_contract
    PUBLIC
    ista_timing_csr_core
)

# This pure contract is deliberately not aggregated into ista_module. Step 7
# may expose it only through the graph adapter, preserving one interface edge.
