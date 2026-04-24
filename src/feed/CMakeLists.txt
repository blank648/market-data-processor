add_library(feed STATIC
    src/FeedSimulator.cpp
)
target_sources(feed PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/feed/FeedSimulator.hpp
        include/feed/FeedConfig.hpp
        include/feed/IFeedSource.hpp
)
target_link_libraries(feed PUBLIC core spdlog::spdlog nlohmann_json::nlohmann_json)
target_link_libraries(feed PRIVATE infra)
