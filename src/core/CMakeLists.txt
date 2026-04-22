add_library(core INTERFACE)

target_include_directories(core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_sources(core INTERFACE
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/core/MarketTick.hpp
        include/core/RingBuffer.hpp
        include/core/ThreadBase.hpp
)

target_compile_features(core INTERFACE cxx_std_20)

target_link_libraries(core INTERFACE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)
