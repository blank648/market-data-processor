add_library(feed STATIC
    src/FeedSimulator.cpp
)
target_include_directories(feed PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(feed PUBLIC core spdlog::spdlog nlohmann_json::nlohmann_json)
target_link_libraries(feed PRIVATE infra)
