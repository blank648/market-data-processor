enable_testing()

add_executable(unit_tests
        # Fișierele de test se adaugă per sprint:
        # Sprint 1: core_tests.cpp
        # Sprint 2: feed_tests.cpp
        # Sprint 3: book_tests.cpp
)

# Placeholder ca să compileze acum fără erori
file(WRITE ${CMAKE_CURRENT_SOURCE_DIR}/placeholder_test.cpp
        "#include <gtest/gtest.h>\nTEST(Placeholder, AlwaysPass) { SUCCEED(); }\n"
)
target_sources(unit_tests PRIVATE placeholder_test.cpp)

target_link_libraries(unit_tests PRIVATE
        core feed processing book strategy infra
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(unit_tests)
