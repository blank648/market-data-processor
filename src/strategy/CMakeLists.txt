add_library(strategy STATIC
    src/SignalEngine.cpp
)

target_include_directories(strategy PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(strategy PUBLIC core spdlog::spdlog)
