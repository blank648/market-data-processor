add_library(infra STATIC
    src/DbWriter.cpp
)

target_include_directories(infra PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_sources(infra PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/infra/Logger.hpp
        include/infra/MetricsCollector.hpp
        include/infra/DbWriter.hpp
)

target_link_libraries(infra PUBLIC
    core
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    PostgreSQL::PostgreSQL
)

target_compile_definitions(infra PUBLIC
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>
)
