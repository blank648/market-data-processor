add_library(infra INTERFACE)
target_include_directories(infra INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(infra INTERFACE core spdlog::spdlog)
