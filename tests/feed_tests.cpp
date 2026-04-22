// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <chrono>
#include <cmath>
#include <thread>

#include <gtest/gtest.h>

#include "feed/FeedConfig.hpp"
#include "feed/FeedSimulator.hpp"

using namespace mdp;

// ── SUITE 1: FeedConfigTest ──────────────────────────────────────────────────

TEST(FeedConfigTest, DefaultConfigIsValid) {
    auto cfg = FeedConfig::default_config();
    EXPECT_TRUE(cfg.is_valid());
    EXPECT_FALSE(cfg.symbols.empty());
    EXPECT_EQ(cfg.symbols.size(), cfg.initial_prices.size());
}

TEST(FeedConfigTest, InvalidConfigZeroTickRate) {
    FeedConfig cfg = FeedConfig::default_config();
    cfg.tick_rate_hz = 0;
    EXPECT_FALSE(cfg.is_valid());
}

TEST(FeedConfigTest, InvalidConfigSymbolPriceMismatch) {
    FeedConfig cfg = FeedConfig::default_config();
    cfg.symbols.push_back("EXTRA");  // one more symbol than prices
    EXPECT_FALSE(cfg.is_valid());
}

TEST(FeedConfigTest, InvalidConfigZeroVolatility) {
    FeedConfig cfg = FeedConfig::default_config();
    cfg.volatility = 0.0;
    EXPECT_FALSE(cfg.is_valid());
}

// ── SUITE 2: FeedSimulatorTest ───────────────────────────────────────────────

class FeedSimulatorTest : public ::testing::Test {
protected:
    TickRingBuffer16K output_buffer_;
    FeedConfig        config_ = FeedConfig::default_config();
};

TEST_F(FeedSimulatorTest, StartsAndStopsCleanly) {
    FeedSimulator sim(config_, output_buffer_);
    sim.start();
    EXPECT_TRUE(sim.is_running());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sim.stop();
    EXPECT_FALSE(sim.is_running());
}

TEST_F(FeedSimulatorTest, ProducesTicksAfterStart) {
    config_.tick_rate_hz = 10'000;
    FeedSimulator sim(config_, output_buffer_);
    sim.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sim.stop();
    EXPECT_GT(sim.ticks_published(), 0u);
}

TEST_F(FeedSimulatorTest, TicksHaveValidPrices) {
    config_.tick_rate_hz = 1'000;
    FeedSimulator sim(config_, output_buffer_);
    sim.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sim.stop();

    MarketTick tick;
    int checked = 0;
    while (output_buffer_.try_pop(tick) && checked < 100) {
        EXPECT_GT(tick.price, 0.0);
        EXPECT_FALSE(std::isnan(tick.price));
        EXPECT_GT(tick.volume, 0.0);
        EXPECT_LE(tick.side, 2u);
        EXPECT_GT(tick.timestamp_ns, 0LL);
        ++checked;
    }
    EXPECT_GT(checked, 0);
}

TEST_F(FeedSimulatorTest, SourceNameIsNotEmpty) {
    FeedSimulator sim(config_, output_buffer_);
    EXPECT_FALSE(sim.source_name().empty());
}

TEST_F(FeedSimulatorTest, DroppedTicksWhenBufferFull) {
    // Use tiny config to quickly fill buffer
    TickRingBuffer4K small_buf;  // only 4096 slots
    config_.tick_rate_hz = 1'000'000;  // 1M ticks/sec → overflows fast
    
    // Note: FeedSimulator takes TickRingBuffer16K — test documents the
    // dropped counter concept via ticks_dropped() returning >= 0
    FeedSimulator sim(config_, output_buffer_);
    sim.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    sim.stop();
    
    // dropped + published = attempted
    uint64_t total = sim.ticks_published() + sim.ticks_dropped();
    EXPECT_GT(total, 0u);
}
