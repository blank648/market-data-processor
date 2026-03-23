add_library(strategy INTERFACE)
target_include_directories(strategy INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(strategy INTERFACE core book spdlog::spdlog)
