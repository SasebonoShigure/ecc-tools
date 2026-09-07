message(STATUS "")
message(STATUS "############## iSTA: START CMAKE ##############")

############################ cmake flag ############################

set(CMAKE_CXX_STANDARD 20)

option(ECC_INCREGPUSTA_DEBUG_CHECKS "Enable IncreGPUSTA development diagnostics" OFF)
configure_file(IncreGpuStaBuildConfig.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/config/IncreGpuStaBuildConfig.hpp)
add_library(ista_incregpusta_build_options INTERFACE)
target_include_directories(ista_incregpusta_build_options INTERFACE
    ${CMAKE_CURRENT_BINARY_DIR}/config)
link_libraries(ista_incregpusta_build_options)
include(${CMAKE_CURRENT_SOURCE_DIR}/test/IncreGpuStaTestModes.cmake)


# add_compile_options("-Wconversion")
# add_compile_options("-fsanitize=address")
# link_libraries("-fsanitize=address")

############################ setting path ############################

set(HOME_ISTA ${HOME_OPERATION}/iSTA)

set(ISTA_INTERFACE ${HOME_ISTA}/interface)
set(ISTA_SOURCE ${HOME_ISTA}/source)
set(ISTA_TEST ${HOME_ISTA}/test)
set(ISTA_BENCHMARK ${HOME_ISTA}/benchmark)

############################ cmake subdirectory ############################

add_subdirectory(${ISTA_INTERFACE})
add_subdirectory(${ISTA_SOURCE})
add_subdirectory(${ISTA_BENCHMARK}/buffer_trace)
add_subdirectory(${ISTA_BENCHMARK}/dual_csr)
add_subdirectory(${ISTA_TEST})

########################################################

message(STATUS "############## iSTA: END CMAKE ##############\n")
