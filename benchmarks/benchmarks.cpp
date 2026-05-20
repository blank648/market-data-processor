#include <benchmark/benchmark.h>
#include <thread>
#include <random>
#include <cstring>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "processing/TickParser.hpp"
#include "processing/Normalizer.hpp"
#include "book/OrderBook.hpp"
#include "book/BookProcessor.hpp"
#include "feed/FeedSimulator.hpp"
#include "feed/FeedConfig.hpp"
#include "infra/Logger.hpp"
#include <spdlog/spdlog.h>

using namespace mdp;

// 1. BM_TickParser_Parse — parse a single valid tick string in a tight loop
static void BM_TickParser_Parse(benchmark::State& state) {
    TickRingBuffer16K input;
    TickRingBuffer4K output;
    TickParser parser(input, output);
    parser.start();

    MarketTick tick = MarketTick::make("AAPL", 150.0, 1.0, 0);
    for (auto run : state) {
        // Push one tick into parser
        while (!input.try_push(tick)) {
            tick = MarketTick::make("AAPL", 150.0, 1.0, 0);
            
            // Drain output to relieve backpressure if buffer full
            MarketTick out{};
            output.try_pop(out);
        }
        
        // Drain output to relieve backpressure continuously
        MarketTick out{};
        output.try_pop(out);
    }

    parser.stop();
}
BENCHMARK(BM_TickParser_Parse)->Iterations(100000); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// 2. BM_Normalizer_Push — push pre-constructed Tick into Normalizer (with thread running)
static void BM_Normalizer_Push(benchmark::State& state) {
    TickRingBuffer4K input;
    TickRingBuffer4K output;
    Normalizer norm(input, output);
    norm.start();

    MarketTick tick = MarketTick::make("AAPL", 150.0, 1.0, 0);
    int64_t timestamp = 1000;
    
    for (auto run : state) {
        tick.timestamp_ns = timestamp++; // Prevent immediate deduplication
        
        while (!input.try_push(tick)) {
            tick = MarketTick::make("AAPL", 150.0, 1.0, 0);
            tick.timestamp_ns = timestamp;
            MarketTick out{};
            output.try_pop(out);
        }
        
        MarketTick out{};
        output.try_pop(out);
    }

    norm.stop();
}
BENCHMARK(BM_Normalizer_Push)->Iterations(100000); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// 3. BM_OrderBook_Insert — insert a random bid/ask into OrderBook
static void BM_OrderBook_Insert(benchmark::State& state) {
    OrderBook book("AAPL");
    std::mt19937 gen(42);
    std::uniform_real_distribution<double> dis(100.0, 200.0);

    for (auto run : state) {
        BookDelta delta{};
        std::strncpy(delta.symbol.data(), "AAPL", delta.symbol.size() - 1);
        delta.symbol.back() = '\0';
        delta.side = OrderSide::BID;
        delta.price = dis(gen);
        delta.volume = 100;
        delta.timestamp_ns = 1000;
        book.apply(delta);
    }
}
BENCHMARK(BM_OrderBook_Insert)->Iterations(100000); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// 4. BM_OrderBook_BestBid — query best_bid() on a book with 1000 price levels
static void BM_OrderBook_BestBid(benchmark::State& state) {
    OrderBook book("AAPL");
    // Pre-fill the book with 1000 price levels
    for (int i = 0; i < 1000; ++i) {
        BookDelta delta{};
        std::strncpy(delta.symbol.data(), "AAPL", delta.symbol.size() - 1);
        delta.symbol.back() = '\0';
        delta.side = OrderSide::BID;
        delta.price = 100.0 + (i * 0.01);
        delta.volume = 100;
        book.apply(delta);
    }

    for (auto run : state) {
        benchmark::DoNotOptimize(book.best_bid());
    }
}
BENCHMARK(BM_OrderBook_BestBid)->Iterations(100000); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// 5. BM_RingBuffer_PushPop — single producer / single consumer, measure round-trip latency
static void BM_RingBuffer_PushPop(benchmark::State& state) {
    TickRingBuffer16K buffer;
    MarketTick tick = MarketTick::make("AAPL", 150.0, 1.0, 0);

    for (auto run : state) {
        buffer.try_push(tick);
        MarketTick out{};
        buffer.try_pop(out);
        benchmark::DoNotOptimize(out);
        tick = MarketTick::make("AAPL", 150.0, 1.0, 0);
    }
}
BENCHMARK(BM_RingBuffer_PushPop)->Iterations(100000); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)

// 6. BM_FullPipeline_Throughput — measure ticks/sec through FeedSim→Norm→Book pipeline
class FullPipelineFixture : public benchmark::Fixture {
public:
    std::unique_ptr<TickRingBuffer16K> sim_to_parser;
    std::unique_ptr<TickRingBuffer4K>  parser_to_norm;
    std::unique_ptr<TickRingBuffer4K>  norm_to_book;
    
    std::unique_ptr<FeedSimulator> sim;
    std::unique_ptr<TickParser>    parser;
    std::unique_ptr<Normalizer>    norm;
    std::unique_ptr<BookProcessor> book;

    void SetUp(const ::benchmark::State& state) override {
        // [FIX — Init once per test]
        mdp::Logger::init("bench_full", spdlog::level::off);
        
        sim_to_parser = std::make_unique<TickRingBuffer16K>();
        parser_to_norm = std::make_unique<TickRingBuffer4K>();
        norm_to_book = std::make_unique<TickRingBuffer4K>();
        
        FeedConfig config = FeedConfig::default_config();
        config.tick_rate_hz = 0; // Disable automatic generation to isolate throughput measurement
        
        sim = std::make_unique<FeedSimulator>(config, *sim_to_parser);
        parser = std::make_unique<TickParser>(*sim_to_parser, *parser_to_norm);
        norm = std::make_unique<Normalizer>(*parser_to_norm, *norm_to_book);
        book = std::make_unique<BookProcessor>(*norm_to_book);
        
        book->start(); 
        norm->start(); 
        parser->start(); 
        sim->start();
    }

    void TearDown(const ::benchmark::State& state) override {
        if (sim) { sim->stop(); }
        if (parser) { parser->stop(); }
        if (norm) { norm->stop(); }
        if (book) { book->stop(); }
        
        sim.reset();
        parser.reset();
        norm.reset();
        book.reset();
        
        sim_to_parser.reset();
        parser_to_norm.reset();
        norm_to_book.reset();
        
        spdlog::shutdown();
    }
};

BENCHMARK_DEFINE_F(FullPipelineFixture, BM_FullPipeline_Throughput)(benchmark::State& state) {
    int64_t timestamp = 1000;
    
    for (auto run : state) {
        MarketTick tick = MarketTick::make("AAPL", 150.0, 1.0, 0);
        tick.timestamp_ns = timestamp++;
        
        while (!sim_to_parser->try_push(tick)) {
            std::this_thread::yield();
            tick = MarketTick::make("AAPL", 150.0, 1.0, 0);
            tick.timestamp_ns = timestamp;
        }
    }
}
BENCHMARK_REGISTER_F(FullPipelineFixture, BM_FullPipeline_Throughput)->Iterations(100000); // NOLINT(cppcoreguidelines-avoid-non-const-global-variables,cppcoreguidelines-owning-memory)


