// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <benchmark/benchmark.h>
#include "core/RingBuffer.hpp"
#include "processing/TickParser.hpp"
#include "infra/Logger.hpp"
#include <chrono>

using namespace mdp;

// SETUP HELPER (file-local, not a fixture)
inline void suppress_logs() {
    mdp::Logger::init("bench", spdlog::level::off);
}

// BENCHMARK 1 — BM_TickParser_Throughput
// Measures end-to-end tick processing rate: ticks pushed to
// sim_to_parser / time elapsed.
static void BM_TickParser_Throughput(benchmark::State& state) {
    suppress_logs();
    TickRingBuffer16K input;
    TickRingBuffer4K  output;
    TickParser parser(input, output);
    parser.start();

    const int64_t batch = state.range(0);
    MarketTick tick = MarketTick::make("AAPL", 150.0, 100.0, 1);

    for (auto _ : state) {
        // push a batch into the parser's input
        int64_t pushed = 0;
        while (pushed < batch) {
            if (input.try_push(MarketTick(tick))) {
                ++pushed;
            }
        }
        // drain output to avoid back-pressure
        MarketTick out{};
        int64_t drained = 0;
        while (drained < batch) {
            if (output.try_pop(out)) {
                ++drained;
            }
        }
        benchmark::DoNotOptimize(out);
    }

    parser.stop();
    mdp::Logger::shutdown();

    state.SetItemsProcessed(state.iterations() * batch);
    state.counters["ticks/s"] = benchmark::Counter(
        static_cast<double>(state.iterations() * batch),
        benchmark::Counter::kIsRate
    );
}
BENCHMARK(BM_TickParser_Throughput)
    ->Arg(100)->Arg(1000)->Arg(10000)
    ->UseRealTime();

// BENCHMARK 2 — BM_TickParser_Latency
// Measures single-tick latency: push 1 tick, wait for it to appear
// in output, record elapsed time.
static void BM_TickParser_Latency(benchmark::State& state) {
    suppress_logs();
    TickRingBuffer16K input;
    TickRingBuffer4K  output;
    TickParser parser(input, output);
    parser.start();

    MarketTick tick = MarketTick::make("MSFT", 300.0, 50.0, 0);
    MarketTick out{};

    for (auto _ : state) {
        auto t0 = std::chrono::steady_clock::now();
        input.try_push(MarketTick(tick));
        while (!output.try_pop(out)) { 
            // spin
        }
        auto t1 = std::chrono::steady_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1-t0).count();
        state.SetIterationTime(static_cast<double>(ns) / 1e9);
        benchmark::DoNotOptimize(out);
    }

    parser.stop();
    mdp::Logger::shutdown();
}
BENCHMARK(BM_TickParser_Latency)
    ->UseManualTime()
    ->MinTime(1.0);
