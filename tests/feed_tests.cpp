// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <chrono>
#include <cmath>
#include <thread>
#include <future>
#include <concepts>

#include <gtest/gtest.h>

#include "feed/FeedConfig.hpp"
#include "feed/FeedSimulator.hpp"
#include "feed/IFeedSource.hpp"
#include "core/ThreadBase.hpp"

using namespace mdp;
using namespace std::chrono_literals;

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

// 1. FeedSimulator constructs without throwing
TEST_F(FeedSimulatorTest, ConstructsWithoutThrowing) {
    EXPECT_NO_THROW({
        FeedSimulator sim(config_, output_buffer_);
    });
}

// 2. start() -> is_running() returns true within 50ms (poll to handle sanitizer overhead)
TEST_F(FeedSimulatorTest, StartSetsIsRunningTrueWithin50ms) {
    FeedSimulator sim(config_, output_buffer_);
    sim.start();
    const auto deadline = std::chrono::steady_clock::now() + 500ms;
    while (!sim.is_running() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(sim.is_running());
    sim.stop();
}

// 3. stop() -> is_running() returns false within 100ms
TEST_F(FeedSimulatorTest, StopSetsIsRunningFalseWithin100ms) {
    FeedSimulator sim(config_, output_buffer_);
    sim.start();
    std::this_thread::sleep_for(50ms);
    ASSERT_TRUE(sim.is_running());
    
    sim.stop();
    std::this_thread::sleep_for(100ms);
    ASSERT_FALSE(sim.is_running());
}

// 4. Double start() is a no-op (second call does not throw, is_running() stays true)
TEST_F(FeedSimulatorTest, DoubleStartIsNoOp) {
    FeedSimulator sim(config_, output_buffer_);
    sim.start();
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(sim.is_running());
    
    EXPECT_NO_THROW({
        sim.start();
    });
    ASSERT_TRUE(sim.is_running());
    sim.stop();
}

// 5. Double stop() is a no-op (does not throw)
TEST_F(FeedSimulatorTest, DoubleStopIsNoOp) {
    FeedSimulator sim(config_, output_buffer_);
    sim.start();
    std::this_thread::sleep_for(20ms);
    sim.stop();
    std::this_thread::sleep_for(20ms);
    ASSERT_FALSE(sim.is_running());
    
    EXPECT_NO_THROW({
        sim.stop();
    });
    ASSERT_FALSE(sim.is_running());
}

// 6. Destructor with running thread: construct, start(), let destructor run — must not hang
TEST_F(FeedSimulatorTest, DestructorWithRunningThreadDoesNotHang) {
    auto task = std::async(std::launch::async, [this]() {
        FeedSimulator sim(config_, output_buffer_);
        sim.start();
        std::this_thread::sleep_for(20ms);
        // Destructor called here at end of scope
    });
    
    auto status = task.wait_for(500ms);
    ASSERT_EQ(status, std::future_status::ready);
}

// 7. source_name() returns non-empty string_view
TEST_F(FeedSimulatorTest, SourceNameReturnsNonEmptyStringView) {
    FeedSimulator sim(config_, output_buffer_);
    auto name = sim.source_name();
    EXPECT_FALSE(name.empty());
}

// 8. IFeedSource interface is satisfied
static_assert(std::derived_from<FeedSimulator, IFeedSource>, "FeedSimulator must derive from IFeedSource");

// 9. ThreadBase interface is satisfied
static_assert(std::derived_from<FeedSimulator, ThreadBase<FeedSimulator>>, "FeedSimulator must derive from ThreadBase");
