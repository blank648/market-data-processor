// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <chrono>
#include <thread>

#include <gtest/gtest.h>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "processing/Normalizer.hpp"
#include "processing/TickParser.hpp"

using namespace mdp;

// Helper at top (used by both suites):
static mdp::MarketTick make_test_tick(std::string_view sym,
                                      double price, double volume,
                                      uint8_t side, int64_t ts_ns) {
    auto t = mdp::MarketTick::make(sym, price, volume, side);
    // Override timestamp for deterministic tests
    t.timestamp_ns = ts_ns;
    return t;
}

// ── SUITE 1: TickParserTest ──────────────────────────────────────────────────

class TickParserTest : public ::testing::Test {
protected:
    mdp::TickRingBuffer16K input_;
    mdp::TickRingBuffer4K  output_;
};

TEST_F(TickParserTest, ValidTickPassesThrough) {
    auto tick = make_test_tick("BTCUSD", 42000.0, 1.0, 0, 1000000LL);
    ASSERT_TRUE(input_.try_push(std::move(tick)));

    mdp::TickParser parser(input_, output_);
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    mdp::MarketTick out;
    ASSERT_TRUE(output_.try_pop(out));
    EXPECT_DOUBLE_EQ(out.price, 42000.0);
    EXPECT_GT(parser.ticks_processed(), 0u);
    EXPECT_EQ(parser.ticks_rejected(), 0u);
}

TEST_F(TickParserTest, NegativePriceIsRejected) {
    auto tick = make_test_tick("BTCUSD", -1.0, 1.0, 0, 1000000LL);
    ASSERT_TRUE(input_.try_push(std::move(tick)));

    mdp::TickParser parser(input_, output_);
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    EXPECT_GT(parser.ticks_rejected(), 0u);
    mdp::MarketTick out;
    EXPECT_FALSE(output_.try_pop(out));  // nothing forwarded
}

TEST_F(TickParserTest, ZeroVolumeIsRejected) {
    auto tick = make_test_tick("BTCUSD", 100.0, 0.0, 0, 1000000LL);
    ASSERT_TRUE(input_.try_push(std::move(tick)));

    mdp::TickParser parser(input_, output_);
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    EXPECT_GT(parser.ticks_rejected(), 0u);
}

TEST_F(TickParserTest, EmptySymbolIsRejected) {
    mdp::MarketTick tick{};  // default: symbol all zeros
    tick.price = 100.0;
    tick.volume = 1.0;
    ASSERT_TRUE(input_.try_push(std::move(tick)));

    mdp::TickParser parser(input_, output_);
    parser.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    parser.stop();

    EXPECT_GT(parser.ticks_rejected(), 0u);
}

// ── SUITE 2: NormalizerTest ──────────────────────────────────────────────────

class NormalizerTest : public ::testing::Test {
protected:
    mdp::TickRingBuffer4K input_;
    mdp::TickRingBuffer4K output_;
};

TEST_F(NormalizerTest, UniqueTicsAreForwarded) {
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(input_.try_push(
            make_test_tick("ETHBTC", 0.05 + i * 0.001, 1.0, 0, 1000LL + i)
        ));
    }

    mdp::Normalizer norm(input_, output_);
    norm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 3u);
    EXPECT_EQ(norm.stats().ticks_deduplicated, 0u);
}

TEST_F(NormalizerTest, ExactDuplicateIsDropped) {
    auto tick = make_test_tick("BTCUSD", 42000.0, 1.0, 0, 999LL);
    ASSERT_TRUE(input_.try_push(tick));
    ASSERT_TRUE(input_.try_push(tick));  // exact duplicate

    mdp::Normalizer norm(input_, output_);
    norm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 1u);
    EXPECT_EQ(norm.stats().ticks_deduplicated, 1u);
}

TEST_F(NormalizerTest, OutOfOrderTimestampIsDropped) {
    ASSERT_TRUE(input_.try_push(make_test_tick("BTCUSD", 100.0, 1.0, 0, 2000LL)));
    ASSERT_TRUE(input_.try_push(make_test_tick("BTCUSD", 101.0, 1.0, 0, 1000LL))); // older ts

    mdp::Normalizer norm(input_, output_);
    norm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 1u);
    EXPECT_EQ(norm.stats().ticks_reordered, 1u);
}

TEST_F(NormalizerTest, DifferentSymbolsDontInterfereDeduplicate) {
    // Same ts+price but different symbols → both forwarded
    ASSERT_TRUE(input_.try_push(make_test_tick("BTCUSD", 100.0, 1.0, 0, 1000LL)));
    ASSERT_TRUE(input_.try_push(make_test_tick("ETHUSD", 100.0, 1.0, 0, 1000LL)));

    mdp::Normalizer norm(input_, output_);
    norm.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 2u);
    EXPECT_EQ(norm.stats().ticks_deduplicated, 0u);
}
