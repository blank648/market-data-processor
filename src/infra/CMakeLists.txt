add_library(infra INTERFACE)
target_include_directories(infra INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(infra INTERFACE spdlog::spdlog)
target_compile_definitions(infra INTERFACE
    $<$<CONFIG:Debug>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE>
    $<$<CONFIG:Release>:SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO>)
