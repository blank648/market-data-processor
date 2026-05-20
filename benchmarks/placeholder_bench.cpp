#include <benchmark/benchmark.h>

static void BM_Placeholder(benchmark::State& state) {
    for (auto run : state) {}
}
BENCHMARK(BM_Placeholder); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

BENCHMARK_MAIN(); // NOLINT(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,cppcoreguidelines-pro-bounds-array-to-pointer-decay,hicpp-no-array-decay)
