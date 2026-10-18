add_library(ista_timing_edit_contract
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingEditCanonicalEncoding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingEditPreparedBatchValidation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingEditResolverView.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingEditTrace.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingEditTraceJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/TimingEditValidation.cpp
)

target_include_directories(ista_timing_edit_contract
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_timing_edit_contract
    PUBLIC
    ista_timing_csr_core
)

target_compile_options(ista_timing_edit_contract PRIVATE -ffp-contract=off)

# This target is intentionally Database-free. Do not add IDM, STAInterface,
# iZH, native edit, transaction, CUDA, or pybind dependencies.
