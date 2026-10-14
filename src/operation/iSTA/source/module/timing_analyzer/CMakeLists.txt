add_library(ista_timing_analyzer
    ${ISTA_MODULE}/timing_analyzer/TimingAnalyzer.cpp
    ${ISTA_MODULE}/timing_analyzer/TimingPathTrace.cpp
)

target_link_libraries(ista_timing_analyzer
    PUBLIC
    ista_delay_calculator
    ista_data_manager
    ista_toolkit
    PRIVATE
    ista_timing_graph_csr_adapter
)

target_include_directories(ista_timing_analyzer
    PUBLIC
    ${ISTA_MODULE}/timing_analyzer/ta_data_manager
    ${ISTA_MODULE}/timing_analyzer
)
