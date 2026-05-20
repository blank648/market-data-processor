// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <chrono>
#include <thread>
#include <future>
#include <mutex>
#include <vector>
#include <string>

#include <gtest/gtest.h>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "processing/Normalizer.hpp"
#include "processing/TickParser.hpp"

using namespace mdp;
using namespace std::chrono_literals;

// Helper at top (used by both suites):
static mdp::MarketTick make_test_tick(std::string_view sym,
                                      double price, double volume,
                                      uint8_t side, int64_t ts_ns) {
    auto tick = mdp::MarketTick::make(sym, price, volume, side);
    // Override timestamp for deterministic tests
    tick.timestamp_ns = ts_ns;
    return tick;
}

// ── SUITE 1: TickParserTest ──────────────────────────────────────────────────

class TickParserTest : public ::testing::Test {
private:
    mdp::TickRingBuffer16K input_;
    mdp::TickRingBuffer4K  output_;

protected:
    mdp::TickRingBuffer16K& input() { return input_; }
    mdp::TickRingBuffer4K& output() { return output_; }
};

// 1. parse() with valid bid/ask/symbol
TEST_F(TickParserTest, ValidTickPassesThrough) {
    auto tick = make_test_tick("BTCUSD", 42000.0, 1.0, 0, 1000000LL);
    ASSERT_TRUE(input().try_push(tick));

    mdp::TickParser parser(input(), output());
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    mdp::MarketTick out;
    ASSERT_TRUE(output().try_pop(out));
    EXPECT_DOUBLE_EQ(out.price, 42000.0);
    EXPECT_GT(parser.ticks_processed(), 0U);
    EXPECT_EQ(parser.ticks_rejected(), 0U);
}

TEST_F(TickParserTest, NegativePriceIsRejected) {
    auto tick = make_test_tick("BTCUSD", -1.0, 1.0, 0, 1000000LL);
    ASSERT_TRUE(input().try_push(tick));

    mdp::TickParser parser(input(), output());
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    EXPECT_GT(parser.ticks_rejected(), 0U);
    mdp::MarketTick out;
    EXPECT_FALSE(output().try_pop(out));  // nothing forwarded
}

TEST_F(TickParserTest, ZeroVolumeIsRejected) {
    auto tick = make_test_tick("BTCUSD", 100.0, 0.0, 0, 1000000LL);
    ASSERT_TRUE(input().try_push(tick));

    mdp::TickParser parser(input(), output());
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    EXPECT_GT(parser.ticks_rejected(), 0U);
}

TEST_F(TickParserTest, EmptySymbolIsRejected) {
    mdp::MarketTick tick{};  // default: symbol all zeros
    tick.price = 100.0;
    tick.volume = 1.0;
    tick.timestamp_ns = 1000LL;
    ASSERT_TRUE(input().try_push(tick));

    mdp::TickParser parser(input(), output());
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    EXPECT_GT(parser.ticks_rejected(), 0U);
}

// 2. parse() with malformed input (empty string, missing fields)
TEST_F(TickParserTest, ParseMalformedInputIsRejected) {
    mdp::MarketTick tick_empty_sym{}; // empty symbol
    tick_empty_sym.price = 100.0;
    tick_empty_sym.volume = 1.0;
    tick_empty_sym.timestamp_ns = 1000LL;
    
    mdp::MarketTick tick_zero_price = make_test_tick("AAPL", 0.0, 1.0, 0, 1000LL);
    mdp::MarketTick tick_zero_vol = make_test_tick("AAPL", 100.0, 0.0, 0, 1000LL);

    ASSERT_TRUE(input().try_push(tick_empty_sym));
    ASSERT_TRUE(input().try_push(tick_zero_price));
    ASSERT_TRUE(input().try_push(tick_zero_vol));

    mdp::TickParser parser(input(), output());
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    EXPECT_EQ(parser.ticks_rejected(), 3U);
    EXPECT_EQ(parser.ticks_processed(), 0U);
    mdp::MarketTick out;
    EXPECT_FALSE(output().try_pop(out));
}

// 3. parse() preserves symbol string exactly (case-sensitive)
TEST_F(TickParserTest, ParsePreservesSymbolStringExactly) {
    auto tick = make_test_tick("BtCuSd", 100.0, 1.0, 0, 1000LL);
    ASSERT_TRUE(input().try_push(tick));

    mdp::TickParser parser(input(), output());
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    mdp::MarketTick out;
    ASSERT_TRUE(output().try_pop(out));
    std::string_view out_sym(out.symbol.data());
    EXPECT_EQ(out_sym, "BtCuSd");
}

// 4. parse() with extreme price values (0.0, negative, very large double)
TEST_F(TickParserTest, ParseExtremePriceValues) {
    auto tick_zero = make_test_tick("BTCUSD", 0.0, 1.0, 0, 1000LL); // extreme 0.0 -> rejected
    auto tick_neg = make_test_tick("BTCUSD", -50.0, 1.0, 0, 1000LL); // negative -> rejected
    auto tick_large = make_test_tick("BTCUSD", 1e100, 1.0, 0, 1000LL); // very large -> accepted
    
    ASSERT_TRUE(input().try_push(tick_zero));
    ASSERT_TRUE(input().try_push(tick_neg));
    ASSERT_TRUE(input().try_push(tick_large));

    mdp::TickParser parser(input(), output());
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    EXPECT_EQ(parser.ticks_rejected(), 2U);
    EXPECT_EQ(parser.ticks_processed(), 1U);
    mdp::MarketTick out;
    ASSERT_TRUE(output().try_pop(out));
    EXPECT_DOUBLE_EQ(out.price, 1e100);
}

// ── SUITE 2: NormalizerTest ──────────────────────────────────────────────────

class NormalizerTest : public ::testing::Test {
private:
    mdp::TickRingBuffer4K input_;
    mdp::TickRingBuffer4K output_;

protected:
    mdp::TickRingBuffer4K& input() { return input_; }
    mdp::TickRingBuffer4K& output() { return output_; }
};

// 5. Constructs with valid RingBuffer reference without throwing
TEST_F(NormalizerTest, ConstructsWithoutThrowing) {
    EXPECT_NO_THROW({
        mdp::Normalizer norm(input(), output());
    });
}

// 6. start() + stop() lifecycle
TEST_F(NormalizerTest, StartSetsIsRunningTrueWithin50ms) {
    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(norm.is_running());
    norm.stop();
}

TEST_F(NormalizerTest, StopSetsIsRunningFalseWithin100ms) {
    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(norm.is_running());
    
    norm.stop();
    std::this_thread::sleep_for(100ms);
    ASSERT_FALSE(norm.is_running());
}

TEST_F(NormalizerTest, DoubleStartIsNoOp) {
    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(norm.is_running());
    
    EXPECT_NO_THROW({
        norm.start();
    });
    ASSERT_TRUE(norm.is_running());
    norm.stop();
}

TEST_F(NormalizerTest, DoubleStopIsNoOp) {
    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(20ms);
    norm.stop();
    std::this_thread::sleep_for(20ms);
    ASSERT_FALSE(norm.is_running());
    
    EXPECT_NO_THROW({
        norm.stop();
    });
    ASSERT_FALSE(norm.is_running());
}

TEST_F(NormalizerTest, DestructorWithRunningThreadDoesNotHang) {
    auto task = std::async(std::launch::async, [this]() {
        mdp::Normalizer norm(input(), output());
        norm.start();
        std::this_thread::sleep_for(20ms);
    });
    auto status = task.wait_for(500ms);
    ASSERT_EQ(status, std::future_status::ready);
}

// 7. stats().ticks_forwarded increments by 1 for each tick pushed that passes dedup
TEST_F(NormalizerTest, UniqueTicsAreForwarded) {
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(input().try_push(
            make_test_tick("ETHBTC", 0.05 + (i * 0.001), 1.0, 0, 1000LL + i)
        ));
    }

    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 3U);
    EXPECT_EQ(norm.stats().ticks_deduplicated, 0U);
}

// 8. stats().ticks_deduplicated increments when same tick is pushed twice consecutively
TEST_F(NormalizerTest, ExactDuplicateIsDropped) {
    // MarketTick is trivially copyable: std::move is a copy, both pushes carry the same data
    auto tick = make_test_tick("BTCUSD", 42000.0, 1.0, 0, 999LL);
    ASSERT_TRUE(input().try_push(tick));
    ASSERT_TRUE(input().try_push(tick));  // exact duplicate

    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 1U);
    EXPECT_EQ(norm.stats().ticks_deduplicated, 1U);
}

// 9. stats().ticks_reordered increments when an out-of-order tick arrives
TEST_F(NormalizerTest, OutOfOrderTimestampIsDropped) {
    ASSERT_TRUE(input().try_push(make_test_tick("BTCUSD", 100.0, 1.0, 0, 2000LL)));
    ASSERT_TRUE(input().try_push(make_test_tick("BTCUSD", 101.0, 1.0, 0, 1000LL))); // older ts

    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 1U);
    EXPECT_EQ(norm.stats().ticks_reordered, 1U);
}

TEST_F(NormalizerTest, DifferentSymbolsDontInterfereDeduplicate) {
    // Same ts+price but different symbols -> both forwarded
    ASSERT_TRUE(input().try_push(make_test_tick("BTCUSD", 100.0, 1.0, 0, 1000LL)));
    ASSERT_TRUE(input().try_push(make_test_tick("ETHUSD", 100.0, 1.0, 0, 1000LL)));

    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 2U);
    EXPECT_EQ(norm.stats().ticks_deduplicated, 0U);
}

// 10. Concurrent safety: push 10000 ticks from 4 threads simultaneously
TEST_F(NormalizerTest, ConcurrentSafetyWith10000Ticks) {
    mdp::Normalizer norm(input(), output());
    norm.start();

    // SPSC buffer requires mutually exclusive pushes from multiple simulated producers
    std::mutex push_mtx;
    auto push_task = [&](int thread_id) {
        std::string sym = "SYM" + std::to_string(thread_id);
        for (int i = 0; i < 2500; ++i) {
            auto tick = make_test_tick(sym, 100.0, 1.0, 0, 1000LL + i);
            bool pushed = false;
            while (!pushed) {
                std::lock_guard<std::mutex> lock(push_mtx);
                pushed = input().try_push(tick);
            }
            std::this_thread::yield();
        }
    };

    // Output buffer must be drained because 10000 ticks > 4096 capacity
    std::atomic<bool> producer_done{false};
    auto consumer_task = std::async(std::launch::async, [&]() {
        mdp::MarketTick tick;
        while (!producer_done) {
            while (output().try_pop(tick)) {}
            std::this_thread::yield();
        }
        // final drain
        while (output().try_pop(tick)) {}
    });

    std::vector<std::future<void>> futures;
    futures.reserve(4);
    for (int i = 0; i < 4; ++i) {
        futures.push_back(std::async(std::launch::async, push_task, i));
    }

    for (auto& future : futures) {
        future.wait();
    }

    // Wait until Normalizer consumes all inputs
    while (!input().empty()) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(50ms);
    norm.stop();
    
    producer_done = true;
    consumer_task.wait();

    auto stats = norm.stats();
    EXPECT_EQ(stats.ticks_forwarded + stats.ticks_deduplicated + stats.ticks_reordered, 10000U);
}

// 11. After stop(), stats() returns consistent snapshot
TEST_F(NormalizerTest, StatsReturnsConsistentSnapshotAfterStop) {
    ASSERT_TRUE(input().try_push(make_test_tick("AAPL", 150.0, 1.0, 0, 1000LL)));
    ASSERT_TRUE(input().try_push(make_test_tick("AAPL", 150.0, 1.0, 0, 1000LL))); // Duplicate
    
    mdp::Normalizer norm(input(), output());
    norm.start();
    std::this_thread::sleep_for(30ms);
    norm.stop();

    auto stats1 = norm.stats();
    auto stats2 = norm.stats();
    
    EXPECT_EQ(stats1.ticks_forwarded, 1U);
    EXPECT_EQ(stats1.ticks_deduplicated, 1U);
    
    EXPECT_EQ(stats1.ticks_forwarded, stats2.ticks_forwarded);
    EXPECT_EQ(stats1.ticks_deduplicated, stats2.ticks_deduplicated);
    EXPECT_EQ(stats1.ticks_reordered, stats2.ticks_reordered);
}
