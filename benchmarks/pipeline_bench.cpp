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

class PipelineFixture : public benchmark::Fixture {
public:
    std::unique_ptr<TickRingBuffer16K> sim_to_parser;
    std::unique_ptr<TickRingBuffer4K>  parser_to_norm;
    std::unique_ptr<TickRingBuffer4K>  norm_output;
    
    std::unique_ptr<FeedSimulator> sim;
    std::unique_ptr<TickParser>    parser;
    std::unique_ptr<Normalizer>    norm;
    std::unique_ptr<BookProcessor> book;

    void SetUp(const ::benchmark::State& state) override {
        mdp::Logger::init("bench_pipe", spdlog::level::off);
        
        sim_to_parser = std::make_unique<TickRingBuffer16K>();
        parser_to_norm = std::make_unique<TickRingBuffer4K>();
        norm_output = std::make_unique<TickRingBuffer4K>();
        
        FeedConfig config = FeedConfig::default_config();
        // Use Arg(0) for tick rate if available
        if (state.range(0) > 0) {
            config.tick_rate_hz = static_cast<uint32_t>(state.range(0));
        }
        config.symbols = {"AAPL", "MSFT", "BTCUSD"};
        
        sim = std::make_unique<FeedSimulator>(config, *sim_to_parser);
        parser = std::make_unique<TickParser>(*sim_to_parser, *parser_to_norm);
        norm = std::make_unique<Normalizer>(*parser_to_norm, *norm_output);
        book = std::make_unique<BookProcessor>(*norm_output);
        
        book->start(); 
        norm->start(); 
        parser->start(); 
        sim->start();
    }

    void TearDown(const ::benchmark::State& state) override {
        // [FIX — Destructor Ordering]
        // Explicitly stop in producer-to-consumer order.
        // This ensures buffers are drained naturally before stages stop.
        if (sim) { sim->stop(); }
        if (parser) { parser->stop(); }
        if (norm) { norm->stop(); }
        if (book) { book->stop(); }
        
        // Destroy stages before buffers
        sim.reset();
        parser.reset();
        norm.reset();
        book.reset();
        
        sim_to_parser.reset();
        parser_to_norm.reset();
        norm_output.reset();
        
        spdlog::shutdown();
    }
};

BENCHMARK_DEFINE_F(PipelineFixture, BM_Pipeline_Throughput)(benchmark::State& state) {
    for (auto run : state) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    MetricsCollector collector(*sim, *parser, *norm, *book);
    auto snap = collector.snapshot();

    state.SetItemsProcessed(static_cast<int64_t>(snap.book_ticks_processed));
    state.counters["book_ticks/s"] = benchmark::Counter(
        static_cast<double>(snap.book_ticks_processed),
        benchmark::Counter::kIsRate);
    state.counters["drop_rate_%"] = benchmark::Counter(
        snap.feed_drop_rate() * 100.0);
}

BENCHMARK_REGISTER_F(PipelineFixture, BM_Pipeline_Throughput) // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
    ->Arg(1000)->Arg(5000)->Arg(10000)->Arg(50000)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond);

BENCHMARK_DEFINE_F(PipelineFixture, BM_Pipeline_BookSnapshotLatency)(benchmark::State& state) {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    for (auto run : state) {
        const auto* order_book = book->book("AAPL");
        if (order_book != nullptr) {
            auto snap = order_book->snapshot();
            benchmark::DoNotOptimize(snap);
        }
    }
    
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK_REGISTER_F(PipelineFixture, BM_Pipeline_BookSnapshotLatency) // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)
    ->UseRealTime()
    ->MinTime(1.0)
    ->Unit(benchmark::kNanosecond);



} // namespace
