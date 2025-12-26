cmake_minimum_required(VERSION 3.14)
project(Cycles VERSION 1.0.0 LANGUAGES CXX)

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

# Core Library
add_library(CyclesCore
    src/context.cpp
    src/graph_builder.cpp
    src/execution_graph.cpp
    src/thread_pool.cpp
    src/executor.cpp
)
target_include_directories(CyclesCore PUBLIC include)

# Tests
enable_testing()

add_executable(CyclesTests
    tests/test_topology.cpp
    tests/test_cycle_detection.cpp
    tests/test_concurrency.cpp
    tests/test_executor.cpp
    tests/test_explicit_ordering.cpp
)
target_link_libraries(CyclesTests PRIVATE CyclesCore gtest_main)

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
gtest_discover_tests(CyclesTests)
