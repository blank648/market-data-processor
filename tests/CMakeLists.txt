enable_testing()

add_executable(unit_tests)

target_sources(unit_tests PRIVATE
    core_tests.cpp
    integration_test.cpp
    book_tests.cpp
    infra_tests.cpp
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
gtest_discover_tests(unit_tests
    DISCOVERY_TIMEOUT 30
)
