enable_testing()

add_executable(unit_tests)

target_sources(unit_tests PRIVATE
    core_tests.cpp
    # Sprint 2: feed_tests.cpp
    # Sprint 3: book_tests.cpp
)

target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main GTest::gmock
    core feed processing book strategy infra
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
