add_executable(mdp-benchmarks
    ring_buffer_bench.cpp
    tick_parser_bench.cpp
    normalizer_bench.cpp
    pipeline_bench.cpp
)

target_link_libraries(mdp-benchmarks PRIVATE
    feed
    processing
    book
    infra
    benchmark::benchmark
    benchmark::benchmark_main
)

target_compile_features(mdp-benchmarks PRIVATE cxx_std_20)

target_include_directories(mdp-benchmarks PRIVATE
    ${CMAKE_SOURCE_DIR}/src/core/include
    ${CMAKE_SOURCE_DIR}/src/feed/include
    ${CMAKE_SOURCE_DIR}/src/processing/include
    ${CMAKE_SOURCE_DIR}/src/book/include
    ${CMAKE_SOURCE_DIR}/src/infra/include
)
