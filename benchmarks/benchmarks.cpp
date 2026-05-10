#include <benchmark/benchmark.h>
#include <thread>
#include <random>
#include <cstring>
#include <chrono>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "processing/TickParser.hpp"
#include "processing/Normalizer.hpp"
#include "book/OrderBook.hpp"
#include "book/BookProcessor.hpp"
#include "feed/FeedSimulator.hpp"
#include "feed/FeedConfig.hpp"

using namespace mdp;
using namespace std::chrono_literals;

// 1. BM_TickParser_Parse — parse a single valid tick string in a tight loop
static void BM_TickParser_Parse(benchmark::State& state) {
    TickRingBuffer16K input;
    TickRingBuffer4K output;
    TickParser parser(input, output);
    parser.start();

    MarketTick t = MarketTick::make("AAPL", 150.0, 1.0, 0);
    for (auto _ : state) {
        // Push one tick into parser
        while (!input.try_push(std::move(t))) {
            t = MarketTick::make("AAPL", 150.0, 1.0, 0);
            
            // Drain output to relieve backpressure if buffer full
            MarketTick out;
            output.try_pop(out);
        }
        
        // Drain output to relieve backpressure continuously
        MarketTick out;
        output.try_pop(out);
    }

    parser.stop();
}
BENCHMARK(BM_TickParser_Parse)->Iterations(100000);

// 2. BM_Normalizer_Push — push pre-constructed Tick into Normalizer (with thread running)
static void BM_Normalizer_Push(benchmark::State& state) {
    TickRingBuffer4K input;
    TickRingBuffer4K output;
    Normalizer norm(input, output);
    norm.start();

    MarketTick t = MarketTick::make("AAPL", 150.0, 1.0, 0);
    int64_t ts = 1000;
    
    for (auto _ : state) {
        t.timestamp_ns = ts++; // Prevent immediate deduplication
        
        while (!input.try_push(std::move(t))) {
            t = MarketTick::make("AAPL", 150.0, 1.0, 0);
            t.timestamp_ns = ts;
            MarketTick out;
            output.try_pop(out);
        }
        
        MarketTick out;
        output.try_pop(out);
    }

    norm.stop();
}
BENCHMARK(BM_Normalizer_Push)->Iterations(100000);

// 3. BM_OrderBook_Insert — insert a random bid/ask into OrderBook
static void BM_OrderBook_Insert(benchmark::State& state) {
    OrderBook book("AAPL");
    std::mt19937 gen(42);
    std::uniform_real_distribution<double> dis(100.0, 200.0);

    for (auto _ : state) {
        BookDelta d{};
        std::strncpy(d.symbol, "AAPL", 8);
        d.side = OrderSide::BID;
        d.price = dis(gen);
        d.volume = 100;
        d.timestamp_ns = 1000;
        book.apply(d);
    }
}
BENCHMARK(BM_OrderBook_Insert)->Iterations(100000);

// 4. BM_OrderBook_BestBid — query best_bid() on a book with 1000 price levels
static void BM_OrderBook_BestBid(benchmark::State& state) {
    OrderBook book("AAPL");
    // Pre-fill the book with 1000 price levels
    for (int i = 0; i < 1000; ++i) {
        BookDelta d{};
        std::strncpy(d.symbol, "AAPL", 8);
        d.side = OrderSide::BID;
        d.price = 100.0 + (i * 0.01);
        d.volume = 100;
        book.apply(d);
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(book.best_bid());
    }
}
BENCHMARK(BM_OrderBook_BestBid)->Iterations(100000);

// 5. BM_RingBuffer_PushPop — single producer / single consumer, measure round-trip latency
static void BM_RingBuffer_PushPop(benchmark::State& state) {
    TickRingBuffer16K buffer;
    MarketTick t = MarketTick::make("AAPL", 150.0, 1.0, 0);

    for (auto _ : state) {
        buffer.try_push(std::move(t));
        MarketTick out;
        buffer.try_pop(out);
        benchmark::DoNotOptimize(out);
        t = MarketTick::make("AAPL", 150.0, 1.0, 0);
    }
}
BENCHMARK(BM_RingBuffer_PushPop)->Iterations(100000);

// 6. BM_FullPipeline_Throughput — measure ticks/sec through FeedSim→Norm→Book pipeline
static void BM_FullPipeline_Throughput(benchmark::State& state) {
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_to_book;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 0; // Disable automatic generation to isolate throughput measurement
    
    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_to_book);
    BookProcessor book(norm_to_book);

    book.start();
    norm.start();
    parser.start();
    sim.start();

    int64_t ts = 1000;
    
    for (auto _ : state) {
        MarketTick t = MarketTick::make("AAPL", 150.0, 1.0, 0);
        t.timestamp_ns = ts++;
        
        while (!sim_to_parser.try_push(std::move(t))) {
            std::this_thread::yield();
            t = MarketTick::make("AAPL", 150.0, 1.0, 0);
            t.timestamp_ns = ts;
        }
    }

    sim.stop();
    parser.stop();
    norm.stop();
    book.stop();
}
BENCHMARK(BM_FullPipeline_Throughput)->Iterations(100000);
