set(HOME_ISTA ${HOME_OPERATION}/iSTA)

add_library(py_ista
    ${CMAKE_CURRENT_SOURCE_DIR}/py_ista.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/py_ista_utils.cpp
)

target_link_libraries(py_ista
    PUBLIC
        tool_manager
        ista_interface
        tcl_util
        pybind11::pybind11
        ecc_logger
)

target_include_directories(py_ista
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${HOME_UTILITY}/json
    PRIVATE
        ${HOME_ISTA}/source/data_manager/adapter/csr
        ${HOME_ISTA}/source/module/timing_csr_snapshot
)
