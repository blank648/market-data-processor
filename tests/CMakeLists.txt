enable_testing()

add_executable(unit_tests)

target_sources(unit_tests PRIVATE
    core_tests.cpp
    integration_test.cpp
    book_tests.cpp
)

target_link_libraries(unit_tests PRIVATE
    core
    feed
    book
    infra
    GTest::gtest_main
    GTest::gmock
)

include(GoogleTest)
# gtest_add_tests() parses sources at configure time — does not run
# the binary for discovery. Required because the ASan+spdlog combination
# causes --gtest_list_tests to hang on Apple Silicon arm64.
# Limitation: new TEST() macros require cmake re-configure to be discovered.
# Acceptable for this project; switch back to gtest_discover_tests() if
# the spdlog static init is removed from BookProcessor.
gtest_add_tests(TARGET unit_tests
    SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/core_tests.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/integration_test.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/book_tests.cpp
)
