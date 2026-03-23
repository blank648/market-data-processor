include(FetchContent)
set(FETCHCONTENT_QUIET FALSE)   # Vrei să vezi ce downloadează în CLion Build log

# ── Google Test v1.14.0 ───────────────────────────────────────────────────────
FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0
        GIT_SHALLOW    TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ── Google Benchmark v1.8.4 ───────────────────────────────────────────────────
FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.4
        GIT_SHALLOW    TRUE
)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(benchmark)

# ── spdlog v1.14.1 ────────────────────────────────────────────────────────────
FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.14.1
        GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(spdlog)

# ── nlohmann/json v3.11.3 ─────────────────────────────────────────────────────
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)
