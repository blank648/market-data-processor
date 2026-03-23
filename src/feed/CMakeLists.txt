add_library(feed INTERFACE)
target_include_directories(feed INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(feed INTERFACE core spdlog::spdlog nlohmann_json::nlohmann_json)
