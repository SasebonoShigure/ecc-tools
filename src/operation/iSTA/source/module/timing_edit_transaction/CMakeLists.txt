add_library(ista_timing_edit_transaction
    ${CMAKE_CURRENT_SOURCE_DIR}/CsrFullExactReceipt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingEditCoordinator.cpp
)

target_include_directories(ista_timing_edit_transaction
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_timing_edit_transaction
    PUBLIC
    ista_timing_edit_contract
    ista_timing_edit_native
    PRIVATE
    ista_timing_graph_csr_adapter
)

# The transaction layer runs under an already-held operation guard; keep this
# dependency closure narrow.
