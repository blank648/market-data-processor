add_library(book INTERFACE)
target_include_directories(book INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(book INTERFACE core spdlog::spdlog)
