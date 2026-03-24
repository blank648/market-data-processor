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

set(DEFAULT_ENABLE_SANITIZERS OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(DEFAULT_ENABLE_SANITIZERS ON)
endif()
option(ENABLE_SANITIZERS "Enable Address/Undefined sanitizers" ${DEFAULT_ENABLE_SANITIZERS})
option(BUILD_BENCHMARKS "Build benchmark targets" ON)

include(cmake/FetchDependencies.cmake)

# ── Compile flags — UN FLAG PER GENERATOR EXPRESSION ─────────────────────────
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wno-unused-parameter
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Debug>:-g>
)

if(ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
        add_compile_options(
            $<$<CONFIG:Debug>:-fno-omit-frame-pointer>
            $<$<CONFIG:Debug>:-fsanitize=address>
            $<$<CONFIG:Debug>:-fsanitize=undefined>
        )
        add_link_options(
            $<$<CONFIG:Debug>:-fsanitize=address>
            $<$<CONFIG:Debug>:-fsanitize=undefined>
        )
    else()
        message(WARNING "ENABLE_SANITIZERS is ON, but compiler '${CMAKE_CXX_COMPILER_ID}' is unsupported.")
    endif()
endif()

add_subdirectory(src/core)
add_subdirectory(src/feed)
add_subdirectory(src/processing)
add_subdirectory(src/book)
add_subdirectory(src/strategy)
add_subdirectory(src/infra)

enable_testing()
add_subdirectory(tests)
if(BUILD_BENCHMARKS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/CMakeLists.txt")
    add_subdirectory(benchmarks)
else()
    message(STATUS "Benchmarks disabled or directory missing; skipping benchmarks target.")
endif()
