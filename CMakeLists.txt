cmake_minimum_required(VERSION 3.14)

project(NexusAlloc
    VERSION 0.1.0
    DESCRIPTION "High-performance C++17 buddy memory allocator"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build unit tests" ON)
option(BUILD_BENCHMARKS "Build benchmark executable" ON)

if(CMAKE_BUILD_TYPE STREQUAL "")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(nexusalloc
    src/MemoryAllocator.cpp
    src/BuddyAllocator.cpp
)

target_include_directories(nexusalloc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(nexusalloc PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(nexusalloc PRIVATE /W4 /permissive-)
else()
    target_compile_options(nexusalloc PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTS)
    enable_testing()
    include(FetchContent)

    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    )

    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_subdirectory(tests)
endif()

