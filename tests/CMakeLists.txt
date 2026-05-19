enable_testing()

add_executable(unit_tests)

target_sources(unit_tests PRIVATE
    core_tests.cpp
    integration_test.cpp
    book_tests.cpp
    infra_tests.cpp
    feed_tests.cpp
    processing_tests.cpp
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

# Bridge integration test: requires a live PostgreSQL instance.
# Set MDP_TEST_DB_CONN to override the default connection string.
add_executable(bridge_integration_tests
    bridge_integration_test.cpp
)

target_link_libraries(bridge_integration_tests PRIVATE
    infra
    pqxx
    GTest::gtest_main
)

gtest_discover_tests(bridge_integration_tests
    DISCOVERY_TIMEOUT 30
)
