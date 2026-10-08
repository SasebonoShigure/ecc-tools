add_library(izh_timing_optimization PreRouteRc.cpp PhysicalNetRc.cpp PreparedPhysicalBatch.cpp PhysicalPlacementRows.cpp)
target_include_directories(izh_timing_optimization PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(izh_timing_optimization PRIVATE "${HOME_OPERATION}/iSTA/interface")
target_link_libraries(izh_timing_optimization PUBLIC ista_timing_database_headers PRIVATE idb icts_source_module_routing_flute)
target_compile_options(izh_timing_optimization PRIVATE -Wall -Wextra -Werror -pedantic)

# Keep strict warnings on this module; legacy IDB headers have unrelated warnings.
target_include_directories(izh_timing_optimization SYSTEM PUBLIC "$<TARGET_PROPERTY:idb,INTERFACE_INCLUDE_DIRECTORIES>")
