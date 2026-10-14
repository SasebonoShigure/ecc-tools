add_library(ista_timing_reporter
    ${ISTA_MODULE}/timing_reporter/TimingReporter.cpp
    ${ISTA_MODULE}/timing_reporter/CsrTimingReport.cpp
)

target_link_libraries(ista_timing_reporter
    PUBLIC
    ista_timing_analyzer
    ista_data_manager
    ista_toolkit
)

target_include_directories(ista_timing_reporter
    PUBLIC
    ${ISTA_MODULE}/timing_reporter/tr_data_manager
    ${ISTA_MODULE}/timing_reporter
)
