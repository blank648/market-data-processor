cmake_minimum_required(VERSION 3.28)

project(MarketDataProcessor
    VERSION 0.1.0
    DESCRIPTION "Multithreaded C++ Market Data Processor — Low-latency SPSC Pipeline"
    LANGUAGES CXX
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds not allowed. Use: cmake -B build")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ── Compile flags — UN FLAG PER GENERATOR EXPRESSION ─────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Debug>:-g>
    $<$<CONFIG:Debug>:-fsanitize=address>
    $<$<CONFIG:Debug>:-fsanitize=undefined>
)
add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address>
    $<$<CONFIG:Debug>:-fsanitize=undefined>
)

include(cmake/FetchDependencies.cmake)

add_subdirectory(src/core)
add_subdirectory(src/feed)
add_subdirectory(src/processing)
add_subdirectory(src/book)
add_subdirectory(src/strategy)
add_subdirectory(src/infra)

enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmarks)
