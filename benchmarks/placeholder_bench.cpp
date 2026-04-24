#include <benchmark/benchmark.h>
static void BM_Placeholder(benchmark::State& s) { for (auto _ : s) {} }
BENCHMARK(BM_Placeholder);
BENCHMARK_MAIN();
