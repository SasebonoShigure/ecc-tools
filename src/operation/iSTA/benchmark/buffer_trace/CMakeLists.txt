set(ISTA_BUFFER_TRACE_CONTRACT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/BufferTraceCanonicalEncoding.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BufferTraceValidationRules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BufferTraceJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BufferTraceArtifactJson.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/GreedyBufferCandidateSelector.cpp
)

add_library(ista_buffer_trace_contract
    ${ISTA_BUFFER_TRACE_CONTRACT_SOURCES}
)

target_include_directories(ista_buffer_trace_contract
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_buffer_trace_contract
    PUBLIC
    ista_timing_edit_contract
    ista_timing_csr_core
)

target_compile_features(ista_buffer_trace_contract PUBLIC cxx_std_20)
target_compile_options(ista_buffer_trace_contract
    PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

# Pure Stage 4 contract only. Do not add Database, IDM, STAInterface, iZH,
# pybind, CUDA, native edit, transaction, or Stage 3 test-seam dependencies.

set(ISTA_BUFFER_TRACE_RUNTIME_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/BufferFixedTraceGenerator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BufferReplayContext.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/BufferTraceReplayer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FreshBaselineRunner.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/NativeBufferObservationExtractor.cpp
)

add_library(ista_buffer_trace_runtime
    ${ISTA_BUFFER_TRACE_RUNTIME_SOURCES}
)

target_include_directories(ista_buffer_trace_runtime
    PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(ista_buffer_trace_runtime
    PUBLIC
    ista_buffer_trace_contract
    ista_timing_edit_transaction
    ista_timing_edit_native
    PRIVATE
    ista_timing_graph_csr_adapter
    ista_timing_database_headers
)

target_compile_features(ista_buffer_trace_runtime PUBLIC cxx_std_20)
target_compile_options(ista_buffer_trace_runtime
    PRIVATE
    -Wall
    -Wextra
    -Werror
    -pedantic
    -ffp-contract=off
)

# Runtime remains production-only. Test adapters and Stage 3 proof/session seams
# are compiled solely into the exact-inventory one-shot probe executable.
