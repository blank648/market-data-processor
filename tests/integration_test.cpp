/**
 * @file integration_test.cpp
 * @brief End-to-end integration tests for the market data processor pipeline.
 */

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <future>

#include "feed/FeedSimulator.hpp"
#include "feed/FeedConfig.hpp"
#include "processing/TickParser.hpp"
#include "processing/Normalizer.hpp"
#include "core/RingBuffer.hpp"
#include "core/MarketTick.hpp"
#include "book/BookProcessor.hpp"
#include "book/BookTypes.hpp"
#include "book/IOrderBook.hpp"
#include "book/OrderBook.hpp"
#include "infra/Logger.hpp"

using namespace mdp;

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        mdp::Logger::init("market-data-test", spdlog::level::warn);
    }
    void TearDown() override {
        mdp::Logger::shutdown();
    }
};

// ── TEST 1: FullPipelineProducesValidTicks ──

TEST_F(IntegrationTest, FullPipelineProducesValidTicks) {
    // Setup
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 5'000;   // moderate rate for test stability

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);

    // [PIPELINE START ORDER]
    // Start consumer before producer to ensure that when producer starts publishing,
    // the consumer is already waiting to process data, preventing buffer overflow or lost ticks at startup.
    norm.start();
    parser.start();
    sim.start();

    // Let it run
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // [PIPELINE STOP ORDER]
    // Stop producer before consumer so no new data is generated,
    // then allow consumers slightly more time to drain their input buffers before stopping them.
    // [DRAIN PROTOCOL] Poll buffer.empty() before stopping downstream stage.
    // sleep_for(fixed_ms) is not sufficient at high tick rates —
    // buffer may still hold thousands of unprocessed ticks.
    auto drain_buffer = [](auto& buf,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!buf.empty() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    sim.stop();
    drain_buffer(sim_to_parser);   // wait for parser to drain its input
    parser.stop();
    drain_buffer(parser_to_norm);  // wait for normalizer to drain its input
    norm.stop();

    // Collect output ticks
    std::vector<MarketTick> collected;
    MarketTick tick{};
    while (norm_output.try_pop(tick)) {
        collected.push_back(tick);
    }

    // Assertions
    ASSERT_GT(collected.size(), 10u) << "Pipeline should have forwarded ticks";
    EXPECT_GT(sim.ticks_published(), 0u);
    EXPECT_GT(parser.ticks_processed(), 0u);
    EXPECT_GT(norm.stats().ticks_forwarded, 0u);

    for (const auto& t : collected) {
        EXPECT_GT(t.price,  0.0)  << "Price must be positive";
        EXPECT_GT(t.volume, 0.0)  << "Volume must be positive";
        EXPECT_GT(t.timestamp_ns, 0LL) << "Timestamp must be set";
        EXPECT_LE(t.side, 2u)     << "Side must be 0, 1, or 2";
        EXPECT_NE(t.symbol[0], '\0') << "Symbol must not be empty";
    }
}

// ── TEST 2: PipelineHandlesHighThroughput ──

TEST_F(IntegrationTest, PipelineHandlesHighThroughputWithoutCrash) {
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 100'000;  // high rate — tests back-pressure

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);

    // [PIPELINE START ORDER]
    // Start downstream consumers before upstream producers
    norm.start();
    parser.start();
    sim.start();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // [PIPELINE STOP ORDER]
    // Stop upstream producers before downstream consumers to drain buffers
    // [DRAIN PROTOCOL] Poll buffer.empty() before stopping downstream stage.
    // sleep_for(fixed_ms) is not sufficient at high tick rates —
    // buffer may still hold thousands of unprocessed ticks.
    auto drain_buffer = [](auto& buf,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!buf.empty() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    sim.stop();
    drain_buffer(sim_to_parser);   // wait for parser to drain its input
    parser.stop();
    drain_buffer(parser_to_norm);  // wait for normalizer to drain its input
    norm.stop();

    // Primary assertion: no crash, no hang
    // Secondary: pipeline processed something
    EXPECT_GT(sim.ticks_published() + sim.ticks_dropped(), 0u);
    EXPECT_GT(parser.ticks_processed() + parser.ticks_rejected(), 0u);

    // Log pipeline stats for human inspection
    auto stats = norm.stats();
    GTEST_LOG_(INFO) << "Sim published:   " << sim.ticks_published();
    GTEST_LOG_(INFO) << "Sim dropped:     " << sim.ticks_dropped();
    GTEST_LOG_(INFO) << "Parser accepted: " << parser.ticks_processed();
    GTEST_LOG_(INFO) << "Parser rejected: " << parser.ticks_rejected();
    GTEST_LOG_(INFO) << "Norm forwarded:  " << stats.ticks_forwarded;
    GTEST_LOG_(INFO) << "Norm deduped:    " << stats.ticks_deduplicated;
    GTEST_LOG_(INFO) << "Norm reordered:  " << stats.ticks_reordered;
}

// ── TEST 3: PipelineStopIsCleanWithRAII ──

TEST_F(IntegrationTest, PipelineStopIsCleanWithRAII) {
    // Verify that RAII destruction order doesn't hang or crash
    {
        TickRingBuffer16K sim_to_parser;
        TickRingBuffer4K  parser_to_norm;
        TickRingBuffer4K  norm_output;
        FeedConfig config = FeedConfig::default_config();
        config.tick_rate_hz = 1'000;

        FeedSimulator sim(config, sim_to_parser);
        TickParser    parser(sim_to_parser, parser_to_norm);
        Normalizer    norm(parser_to_norm, norm_output);

        norm.start();
        parser.start();
        sim.start();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        
        // [DRAIN PROTOCOL] Poll buffer.empty() before stopping downstream stage.
        // sleep_for(fixed_ms) is not sufficient at high tick rates —
        // buffer may still hold thousands of unprocessed ticks.
        auto drain_buffer = [](auto& buf,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!buf.empty() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        };

        sim.stop();
        drain_buffer(sim_to_parser);   // wait for parser to drain its input
        parser.stop();
        drain_buffer(parser_to_norm);  // wait for normalizer to drain its input
        norm.stop();
        
        // Destructors called in reverse order — should not hang
        // (norm, then parser, then sim) -> This is technically the wrong semantic order for graceful shutdown 
        // as producer is destroyed last, but RAII should still not hang explicitly inside dtors.
    }
    SUCCEED();  // If we reach here, RAII worked correctly
}

// ── TEST 4: BookProcessorReceivesTicks ──

TEST_F(IntegrationTest, BookProcessorReceivesTicks) {
    // Pipeline: Sim → Parser → Normalizer → BookProcessor
    // Config: 5 symbols, 1000 Hz, run for 300ms
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_output;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 1000;
    config.symbols = {"AAPL", "MSFT", "GOOG", "AMZN", "META"};
    config.initial_prices = {150.0, 300.0, 2800.0, 3400.0, 350.0}; // Must match length of symbols

    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_output);
    BookProcessor book(norm_output);

    // Start consumers before producers
    book.start();
    norm.start();
    parser.start();
    sim.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Stop producers before consumers, draining buffers between stages
    auto drain_buffer = [](auto& buf,
                           std::chrono::milliseconds timeout = std::chrono::milliseconds(200)) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!buf.empty() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    };

    sim.stop();
    drain_buffer(sim_to_parser);
    parser.stop();
    drain_buffer(parser_to_norm);
    norm.stop();
    drain_buffer(norm_output);
    book.stop();

    // [TEARDOWN ORDER] Explicit stop() ensures each jthread is joined
    // before the next stage's buffer reference becomes invalid.
    // Relying on destructor order is fragile when stages share references.
    book.stop();
    norm.stop();
    parser.stop();
    sim.stop();

    // Assertions
    EXPECT_GT(book.ticks_processed(), 0);
    EXPECT_EQ(book.books_active(), 5);

    GTEST_LOG_(INFO) << "Book ticks processed: " << book.ticks_processed();
    GTEST_LOG_(INFO) << "Active books: " << book.books_active();

    bool at_least_one_valid_book = false;
    for (const auto& symbol : config.symbols) {
        const auto* b = book.book(symbol);
        ASSERT_NE(b, nullptr) << "Book for " << symbol << " is null";

        auto tob = b->top_of_book();
        GTEST_LOG_(INFO) << "  [" << symbol << "] Top-of-book spread: " << tob.spread;

        if (tob.is_valid()) {
            at_least_one_valid_book = true;
            EXPECT_GT(tob.spread, 0.0);
            EXPECT_GT(tob.best_bid, 0.0);
            EXPECT_GT(tob.best_ask, 0.0);
            EXPECT_GE(tob.spread, 0.0); // Spread can be 0 if only one side has levels
        }
    }
    EXPECT_TRUE(at_least_one_valid_book) << "Expected at least one symbol to have a valid, two-sided book";
}

// ── TEST 5: FullPipelineSmoke ──

TEST_F(IntegrationTest, FullPipelineSmoke) {
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_to_book; // The shared RingBuffer

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 5000;
    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_to_book);
    BookProcessor book(norm_to_book);

    // Call start() on all in order
    sim.start();
    parser.start();
    norm.start();
    book.start();

    // Sleep 200ms
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Call stop() in reverse order
    book.stop();
    norm.stop();
    parser.stop();
    sim.stop();

    // Assertions
    auto stats = norm.stats();
    EXPECT_GT(stats.ticks_forwarded, 0u);
    EXPECT_GE(stats.ticks_forwarded, stats.ticks_deduplicated);

    // bookprocessor processes at least one symbol
    bool has_active = false;
    for (const auto& sym : config.symbols) {
        auto b = book.book(sym);
        if (b != nullptr && b->updates_applied() > 0) {
            has_active = true;
            break;
        }
    }
    EXPECT_TRUE(has_active);

    // No component is_running()
    EXPECT_FALSE(book.is_running());
    EXPECT_FALSE(norm.is_running());
    EXPECT_FALSE(parser.is_running());
    EXPECT_FALSE(sim.is_running());
}

// ── TEST 6: GracefulShutdownUnderLoad ──

TEST_F(IntegrationTest, GracefulShutdownUnderLoad) {
    TickRingBuffer16K sim_to_parser;
    TickRingBuffer4K  parser_to_norm;
    TickRingBuffer4K  norm_to_book;

    FeedConfig config = FeedConfig::default_config();
    config.tick_rate_hz = 0; // Prevent FeedSimulator from generating its own ticks to avoid SPSC UB
    
    FeedSimulator sim(config, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_to_book);
    BookProcessor book(norm_to_book);

    // start() all
    sim.start();
    parser.start();
    norm.start();
    book.start();

    // Push 50000 ticks directly into the RingBuffer from a separate thread
    auto spammer = std::async(std::launch::async, [&]() {
        for (int i = 0; i < 50000; ++i) {
            auto t = MarketTick::make("AAPL", 150.0, 1.0, 0);
            while (!sim_to_parser.try_push(std::move(t))) {
                if (!parser.is_running()) return; // Abort safely if the pipeline stops
                std::this_thread::yield();
                t = MarketTick::make("AAPL", 150.0, 1.0, 0);
            }
        }
    });

    // After 100ms, stop() all
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto shutdown = std::async(std::launch::async, [&]() {
        book.stop();
        norm.stop();
        parser.stop();
        sim.stop();
    });

    // Verify: no crash, no deadlock
    auto status = shutdown.wait_for(std::chrono::seconds(2));
    EXPECT_EQ(status, std::future_status::ready) << "Shutdown deadlocked!";
    
    spammer.wait();

    // Stats are readable
    EXPECT_NO_THROW({
        auto s = norm.stats();
        EXPECT_GE(s.ticks_forwarded, 0u);
    });
}
