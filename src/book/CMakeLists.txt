add_library(book STATIC)

target_sources(book PRIVATE
    src/OrderBook.cpp
    src/BookProcessor.cpp
)

target_sources(book PUBLIC
    FILE_SET HEADERS
    BASE_DIRS include
    FILES
        include/book/OrderBook.hpp
        include/book/BookProcessor.hpp
        include/book/IOrderBook.hpp
        include/book/BookTypes.hpp
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
