// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <benchmark/benchmark.h>
#include "core/RingBuffer.hpp"
#include <atomic>
#include <thread>

using namespace mdp;

namespace {

// BENCHMARK 1 — BM_RingBuffer_PushPop_Sequential
// Single-threaded: push one tick, pop one tick, repeat.
// Measures raw push+pop round-trip latency with no contention.
void BM_RingBuffer_PushPop_Sequential(benchmark::State& state) {
    TickRingBuffer4K ring_buf;
    MarketTick tick = MarketTick::make("AAPL", 150.0, 100.0, 1);
    for (auto run : state) {
        ring_buf.try_push(tick);
        MarketTick out{};
        ring_buf.try_pop(out);
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBuffer_PushPop_Sequential); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// BENCHMARK 2 — BM_RingBuffer_Push_ThroughputBurst
// Push as many ticks as possible in a burst (fill buffer then drain).
// Use state.range(0) for burst size: {64, 512, 4096}.
void BM_RingBuffer_Push_ThroughputBurst(benchmark::State& state) {
    const int64_t burst = state.range(0);
    TickRingBuffer16K ring_buf;
    MarketTick tick = MarketTick::make("MSFT", 300.0, 50.0, 0);
    MarketTick out{};
    for (auto run : state) {
        // fill
        for (int64_t idx = 0; idx < burst; ++idx) {
            ring_buf.try_push(tick);
        }
        // drain
        for (int64_t idx = 0; idx < burst; ++idx) {
            ring_buf.try_pop(out);
        }
        benchmark::DoNotOptimize(out);
    }
    state.SetItemsProcessed(state.iterations() * burst * 2); // push+pop
}
BENCHMARK(BM_RingBuffer_Push_ThroughputBurst) // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
    ->Arg(64)->Arg(512)->Arg(4096);

// BENCHMARK 3 — BM_RingBuffer_SPSC_Concurrent
// True SPSC: producer thread pushes, consumer thread pops,
// measure sustained throughput over benchmark duration.
void BM_RingBuffer_SPSC_Concurrent(benchmark::State& state) {
    TickRingBuffer16K ring_buf;
    MarketTick tick = MarketTick::make("BTC", 60000.0, 1.0, 1);
    std::atomic<bool> running{true};
    int64_t consumed = 0;

    // Consumer thread
    std::thread consumer([&]() {
        MarketTick out{};
        while (running.load(std::memory_order_relaxed)) {
            if (ring_buf.try_pop(out)) {
                ++consumed;
            }
        }
        // drain remaining
        while (ring_buf.try_pop(out)) {
            ++consumed;
        }
    });

    for (auto run : state) {
        while (!ring_buf.try_push(tick)) { 
            // spin — buffer full
        }
    }

    running.store(false, std::memory_order_relaxed);
    consumer.join();

    state.SetItemsProcessed(consumed);
    state.counters["consumed"] = benchmark::Counter(
        static_cast<double>(consumed),
        benchmark::Counter::kIsRate,
        benchmark::Counter::OneK::kIs1000
    );
}
BENCHMARK(BM_RingBuffer_SPSC_Concurrent) // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
    ->MeasureProcessCPUTime()
    ->UseRealTime()
    ->MinTime(1.0);

} // namespace
