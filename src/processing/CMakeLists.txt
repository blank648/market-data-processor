add_library(processing STATIC
    src/TickParser.cpp
    src/Normalizer.cpp
)
target_sources(processing PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/processing/TickParser.hpp
        include/processing/Normalizer.hpp
)
target_link_libraries(processing PUBLIC core spdlog::spdlog)
target_link_libraries(processing PRIVATE infra)
