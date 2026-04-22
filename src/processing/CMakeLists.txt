add_library(processing STATIC
    src/TickParser.cpp
    src/Normalizer.cpp
)
target_include_directories(processing PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(processing PUBLIC core spdlog::spdlog)
