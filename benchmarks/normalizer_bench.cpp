#include <benchmark/benchmark.h>
#include <thread>
#include "core/RingBuffer.hpp"
#include "processing/Normalizer.hpp"
#include "infra/Logger.hpp"

using namespace mdp;

namespace {

inline void suppress_logs() {
    mdp::Logger::init("bench_norm", spdlog::level::off);
}

static void BM_Normalizer_Throughput_Unique(benchmark::State& state) {
    suppress_logs();
    TickRingBuffer4K input;
    TickRingBuffer4K output;
    Normalizer norm(input, output);
    norm.start();

    const int64_t batch = state.range(0);
    MarketTick out{};
    double price = 150.0;

    for (auto _ : state) {
        int64_t pushed = 0;
        while (pushed < batch) {
            MarketTick tick = MarketTick::make(
                "AAPL", price, 100.0, 1);
            price += 0.01;
            if (price > 300.0) price = 150.0;
            if (input.try_push(std::move(tick))) ++pushed; // Note: adjusted to use try_push as ring_buffer requires it
        }
        int64_t drained = 0;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (drained < batch &&
               std::chrono::steady_clock::now() < deadline) {
            if (output.try_pop(out)) ++drained; // Note: adjusted to use try_pop
        }
        benchmark::DoNotOptimize(out);
    }

    norm.stop();
    spdlog::shutdown();
    state.SetItemsProcessed(state.iterations() * batch);
    state.counters["ticks/s"] = benchmark::Counter(
        static_cast<double>(state.iterations() * batch),
        benchmark::Counter::kIsRate);
}
BENCHMARK(BM_Normalizer_Throughput_Unique)
    ->Arg(100)->Arg(1000)
    ->UseRealTime();

static void BM_Normalizer_Throughput_Duplicate(benchmark::State& state) {
    suppress_logs();
    TickRingBuffer4K input;
    TickRingBuffer4K output;
    Normalizer norm(input, output);
    norm.start();

    const int64_t batch = state.range(0);
    MarketTick tick = MarketTick::make("AAPL", 150.0, 100.0, 1);

    for (auto _ : state) {
        int64_t pushed = 0;
        while (pushed < batch) {
            if (input.try_push(MarketTick(tick))) ++pushed; // Note: adjusted to try_push
        }
        MarketTick out{};
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        while (output.try_pop(out)) { benchmark::DoNotOptimize(out); } // Note: adjusted to try_pop
    }

    norm.stop();
    spdlog::shutdown();
    state.SetItemsProcessed(state.iterations() * batch);
}
BENCHMARK(BM_Normalizer_Throughput_Duplicate)
    ->Arg(100)->Arg(1000)
    ->UseRealTime();

} // namespace
