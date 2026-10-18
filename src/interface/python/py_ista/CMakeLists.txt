set(HOME_ISTA ${HOME_OPERATION}/iSTA)

add_library(py_ista
    ${CMAKE_CURRENT_SOURCE_DIR}/py_ista.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/py_ista_utils.cpp
)

target_link_libraries(py_ista
    PUBLIC
        tool_manager
        ista_interface
        ista_incregpusta_build_options
        tcl_util
        pybind11::pybind11
        ecc_logger
)

target_link_libraries(py_ista PRIVATE izh_timing_optimization ista_timing_database_headers ista_timing_edit_native)

target_include_directories(py_ista
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${HOME_UTILITY}/json
    PRIVATE
        ${HOME_ISTA}/source/data_manager/adapter/csr
        ${HOME_ISTA}/source/module/timing_csr
        ${HOME_ISTA}/source/module/timing_csr_snapshot
)

# ON-only acceptance readback: reuse the independent mechanical timing reader.
# No test inspection code is linked into the default binding.
if(ECC_INCREGPUSTA_DEBUG_CHECKS)
    target_link_libraries(py_ista PRIVATE ista_data_manager ista_timing_analyzer)
    if(ISTA_ENABLE_CUDA)
        target_link_libraries(py_ista PRIVATE ista_timing_dual_csr_native)
    endif()
    set(physical_diagnostic_sources
        PhysicalStaDiagnostic.cpp
        CsrNativeTimingOracleReader.cpp
        CsrNativeTimingOracleNodeReader.cpp
        CsrNativeTimingOracleArcReader.cpp
        real_differential/CsrRealDifferentialOracleEncoding.cpp
        real_differential/CsrRealDifferentialWire.cpp
    )
    list(TRANSFORM physical_diagnostic_sources PREPEND "${HOME_ISTA}/test/")
    target_sources(py_ista PRIVATE ${physical_diagnostic_sources})
    target_include_directories(py_ista PRIVATE
        "${HOME_ISTA}/test"
        "${HOME_ISTA}/test/timing_edit"
        "${HOME_ISTA}/test/real_differential"
    )
    set_source_files_properties(${physical_diagnostic_sources}
        PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-pedantic;-ffp-contract=off")
endif()
