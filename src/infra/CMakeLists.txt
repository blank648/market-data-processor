add_library(infra INTERFACE)
target_sources(infra INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/infra/Logger.hpp
        include/infra/MetricsCollector.hpp
)
target_link_libraries(infra INTERFACE spdlog::spdlog nlohmann_json::nlohmann_json)
target_compile_definitions(infra INTERFACE
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>)
