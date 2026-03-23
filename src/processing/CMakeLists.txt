add_library(processing INTERFACE)
target_include_directories(processing INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(processing INTERFACE core spdlog::spdlog)
