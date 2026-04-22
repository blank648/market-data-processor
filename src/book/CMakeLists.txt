add_library(book STATIC)

target_sources(book PRIVATE
    src/OrderBook.cpp
    src/BookProcessor.cpp
)

target_include_directories(book PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(book PUBLIC cxx_std_20)

target_link_libraries(book PUBLIC
    core
    processing
    spdlog::spdlog
)
target_link_libraries(book PRIVATE infra)

# book depends on processing for TickRingBuffer4K type
# book depends on core for MarketTick, ThreadBase
