cmake_minimum_required(VERSION 3.11)

set (CMAKE_CXX_STANDARD 20)
# set (CMAKE_BUILD_TYPE "Release")

set(CMAKE_CXX_FLAGS_RELEASE "$ENV{CXXFLAGS} -DNDEBUG")

aux_source_directory(./ SRC)
add_library(liberty ${SRC})

target_link_libraries(liberty PUBLIC absl::btree absl::inlined_vector ecc_logger tcl dl)

target_include_directories(liberty PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${HOME_UTILITY}
    ${HOME_THIRDPARTY}/parser/liberty/)


if(BUILD_TESTING)
  add_executable(liberty_expr_lifetime_test validation/LibertyExprLifetimeTest.cpp)
  target_link_libraries(liberty_expr_lifetime_test PRIVATE liberty)
  add_test(NAME liberty_expr_lifetime_test COMMAND liberty_expr_lifetime_test)
endif()
