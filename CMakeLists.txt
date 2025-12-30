cmake_minimum_required(VERSION 3.14)
project(plexus VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

FetchContent_Declare(
  googlebenchmark
  URL https://github.com/google/benchmark/archive/refs/tags/v1.8.3.zip
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

# Core Library
add_library(plexusCore
    src/context.cpp
    src/graph_builder.cpp
    src/execution_graph.cpp
    src/executor.cpp
)
target_include_directories(plexusCore PUBLIC include)

# Tests
enable_testing()

add_executable(plexusTests
    tests/test_topology.cpp
    tests/test_cycle_detection.cpp
    tests/test_concurrency.cpp
    tests/test_executor.cpp
    tests/test_explicit_ordering.cpp
    tests/test_error_handling.cpp
    tests/test_graph_viz.cpp
    tests/test_affinity.cpp
    tests/test_stress_loop.cpp
    tests/test_node_groups.cpp
)
target_link_libraries(plexusTests PRIVATE plexusCore gtest_main)
target_include_directories(plexusTests PRIVATE src)

# Documentation
find_package(Doxygen)
if(DOXYGEN_FOUND)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile.in ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs/doxygen)
    add_custom_target(docs
        ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM)
endif()

include(GoogleTest)
gtest_discover_tests(plexusTests)

# Benchmarks
add_executable(plexusBenchmarks
    benchmarks/benchmark_thread_pool.cpp
)
target_link_libraries(plexusBenchmarks PRIVATE plexusCore benchmark::benchmark_main)
target_include_directories(plexusBenchmarks PRIVATE src)
