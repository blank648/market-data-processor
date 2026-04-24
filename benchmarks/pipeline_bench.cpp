#include <benchmark/benchmark.h>
#include <thread>
#include "core/RingBuffer.hpp"
#include "feed/FeedSimulator.hpp"
#include "processing/TickParser.hpp"
#include "processing/Normalizer.hpp"
#include "book/BookProcessor.hpp"
#include "infra/Logger.hpp"
#include "infra/MetricsCollector.hpp"

using namespace mdp;

namespace {

static void BM_Pipeline_Throughput(benchmark::State& state) {
    mdp::Logger::init("bench_pipe", spdlog::level::off);

    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = static_cast<uint32_t>(state.range(0));
    config.symbols      = {"AAPL", "MSFT", "BTCUSD"};

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);
    BookProcessor book(norm_output);

    book.start(); norm.start(); parser.start(); sim.start();

    for (auto _ : state) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    sim.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    parser.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    norm.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    book.stop();

    MetricsCollector collector(sim, parser, norm, book);
    auto snap = collector.snapshot();

    state.SetItemsProcessed(
        static_cast<int64_t>(snap.book_ticks_processed));
    state.counters["book_ticks/s"] = benchmark::Counter(
        static_cast<double>(snap.book_ticks_processed),
        benchmark::Counter::kIsRate);
    state.counters["drop_rate_%"] = benchmark::Counter(
        snap.feed_drop_rate() * 100.0);

    spdlog::shutdown();
}
BENCHMARK(BM_Pipeline_Throughput)
    ->Arg(1000)->Arg(5000)->Arg(10000)->Arg(50000)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

static void BM_Pipeline_BookSnapshotLatency(benchmark::State& state) {
    mdp::Logger::init("bench_snap", spdlog::level::off);

    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 10'000;
    config.symbols      = {"AAPL", "MSFT"};

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);
    BookProcessor book(norm_output);

    book.start(); norm.start(); parser.start(); sim.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    for (auto _ : state) {
        auto* ob = book.book("AAPL");
        if (ob) {
            auto snap = ob->snapshot();
            benchmark::DoNotOptimize(snap);
        }
    }

    sim.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    parser.stop(); norm.stop(); book.stop();
    spdlog::shutdown();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Pipeline_BookSnapshotLatency)
    ->UseRealTime()
    ->MinTime(1.0)
    ->Unit(benchmark::kNanosecond);

} // namespace
