// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <core/MarketTick.hpp>
#include <core/RingBuffer.hpp>
#include <core/ThreadBase.hpp>

#include <atomic>
#include <chrono>
#include <numeric>
#include <string_view>
#include <thread>
#include <vector>

using namespace mdp;
using namespace std::chrono_literals;

// ─── SUITE 1: MarketTickTest ───────────────────────────────────────────────

TEST(MarketTickTest, SizeIs40Bytes) {
    EXPECT_EQ(sizeof(MarketTick), 40u);
}

TEST(MarketTickTest, TrivialCopyability) {
    static_assert(std::is_trivially_copyable_v<MarketTick>, "MarketTick must be trivially copyable.");
    EXPECT_TRUE(std::is_trivially_copyable_v<MarketTick>);
}

TEST(MarketTickTest, MakePopulatesAllFields) {
    auto t = MarketTick::make("BTCUSD", 42000.5, 1.23, 0);

    std::string_view sym{t.symbol.data(), 6};
    EXPECT_EQ(sym, "BTCUSD");

    // Check remainder of the symbol buffer is zeroed
    EXPECT_EQ(t.symbol[6], '\0');
    EXPECT_EQ(t.symbol[7], '\0');

    EXPECT_DOUBLE_EQ(t.price, 42000.5);
    EXPECT_DOUBLE_EQ(t.volume, 1.23);
    EXPECT_EQ(t.side, 0);
    EXPECT_GT(t.timestamp_ns, 0);
}

TEST(MarketTickTest, TimestampIsMonotonicallyIncreasing) {
    auto t1 = MarketTick::make("BTC", 1.0, 1.0, 0);
    auto t2 = MarketTick::make("ETH", 1.0, 1.0, 0);
    EXPECT_GE(t2.timestamp_ns, t1.timestamp_ns);
}

TEST(MarketTickTest, SymbolTruncatesAt8Chars) {
    auto t = MarketTick::make("TOOLONGSYMBOL", 1.0, 1.0, 0);
    std::string_view sym{t.symbol.data(), 8};
    EXPECT_EQ(sym, "TOOLONGS");
}

TEST(MarketTickTest, ToStringContainsPriceAndSymbol) {
    auto t = MarketTick::make("BTCUSD", 42000.5, 1.23, 0);
    std::string s = t.to_string();
    EXPECT_NE(s.find("BTCUSD"), std::string::npos);
    EXPECT_NE(s.find("42000.50"), std::string::npos);
    EXPECT_NE(s.find("BID"), std::string::npos);
}

TEST(MarketTickTest, EqualityOperator) {
    auto t1 = MarketTick::make("BTCUSD", 42000.5, 1.23, 0);
    auto t2 = t1; // Exact copy including timestamp and padding
    EXPECT_EQ(t1, t2);

    t2.price = 40000.0;
    EXPECT_NE(t1, t2);
}

// ─── SUITE 2: RingBufferTest ───────────────────────────────────────────────

class RingBufferTest : public ::testing::Test {
protected:
    RingBuffer<MarketTick, 4> rb_;

    MarketTick make_tick(double price) {
        return MarketTick::make("TEST", price, 1.0, 0);
    }
};

TEST_F(RingBufferTest, EmptyOnConstruction) {
    EXPECT_TRUE(rb_.empty());
    EXPECT_EQ(rb_.size(), 0u);
    EXPECT_EQ(rb_.capacity(), 4u);
}

TEST_F(RingBufferTest, PushToEmptySucceeds) {
    ASSERT_TRUE(rb_.try_push(make_tick(100.0)));
    EXPECT_EQ(rb_.size(), 1u);
    EXPECT_FALSE(rb_.empty());
}

TEST_F(RingBufferTest, PopFromEmptyReturnsFalse) {
    MarketTick out;
    ASSERT_FALSE(rb_.try_pop(out));
}

TEST_F(RingBufferTest, PushBeyondCapacityReturnsFalse) {
    EXPECT_TRUE(rb_.try_push(make_tick(1.0)));
    EXPECT_TRUE(rb_.try_push(make_tick(2.0)));
    EXPECT_TRUE(rb_.try_push(make_tick(3.0)));
    EXPECT_TRUE(rb_.try_push(make_tick(4.0)));

    EXPECT_FALSE(rb_.try_push(make_tick(5.0)));
    EXPECT_TRUE(rb_.full());
}

TEST_F(RingBufferTest, FIFOOrderPreserved) {
    ASSERT_TRUE(rb_.try_push(make_tick(10.0)));
    ASSERT_TRUE(rb_.try_push(make_tick(20.0)));
    ASSERT_TRUE(rb_.try_push(make_tick(30.0)));

    MarketTick out;
    ASSERT_TRUE(rb_.try_pop(out));
    EXPECT_DOUBLE_EQ(out.price, 10.0);

    ASSERT_TRUE(rb_.try_pop(out));
    EXPECT_DOUBLE_EQ(out.price, 20.0);

    ASSERT_TRUE(rb_.try_pop(out));
    EXPECT_DOUBLE_EQ(out.price, 30.0);
}

TEST_F(RingBufferTest, WrapAroundMaintainsIntegrity) {
    // Push 4 ticks
    for (int i = 1; i <= 4; ++i) {
        ASSERT_TRUE(rb_.try_push(make_tick(static_cast<double>(i))));
    }

    // Pop 2
    MarketTick out;
    ASSERT_TRUE(rb_.try_pop(out)); EXPECT_DOUBLE_EQ(out.price, 1.0);
    ASSERT_TRUE(rb_.try_pop(out)); EXPECT_DOUBLE_EQ(out.price, 2.0);

    // Push 2 more (wraps around)
    ASSERT_TRUE(rb_.try_push(make_tick(5.0)));
    ASSERT_TRUE(rb_.try_push(make_tick(6.0)));

    // Pop all 4
    ASSERT_TRUE(rb_.try_pop(out)); EXPECT_DOUBLE_EQ(out.price, 3.0);
    ASSERT_TRUE(rb_.try_pop(out)); EXPECT_DOUBLE_EQ(out.price, 4.0);
    ASSERT_TRUE(rb_.try_pop(out)); EXPECT_DOUBLE_EQ(out.price, 5.0);
    ASSERT_TRUE(rb_.try_pop(out)); EXPECT_DOUBLE_EQ(out.price, 6.0);
}

// ─── SUITE 3: SPSCConcurrentTest ───────────────────────────────────────────

static constexpr int kMsgCount = 100'000;
static constexpr std::size_t kBufSize = 1024;

TEST(SPSCConcurrentTest, NoDataLossUnderConcurrentLoad) {
    RingBuffer<MarketTick, kBufSize> rb;
    std::atomic<int> produced{0}, consumed{0};
    std::atomic<bool> stop_flag{false};

    std::vector<double> received;
    received.reserve(kMsgCount);

    std::thread producer([&]() {
        for (int i = 0; i < kMsgCount && !stop_flag.load(std::memory_order_relaxed); ++i) {
            auto tick = MarketTick::make("PERF", static_cast<double>(i), 1.0, 0);
            while (!stop_flag.load(std::memory_order_relaxed) && !rb.try_push(std::move(tick))) {
                // spin
            }
            ++produced;
        }
    });

    std::thread consumer([&]() {
        MarketTick out;
        while (!stop_flag.load(std::memory_order_relaxed) && consumed.load(std::memory_order_relaxed) < kMsgCount) {
            if (rb.try_pop(out)) {
                received.push_back(out.price);
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kMsgCount);
    for (int i = 0; i < kMsgCount; ++i) {
        EXPECT_DOUBLE_EQ(received[i], static_cast<double>(i));
    }
}

TEST(SPSCConcurrentTest, ThroughputBaseline) {
    RingBuffer<MarketTick, kBufSize> rb;
    std::atomic<int> produced{0}, consumed{0};
    std::atomic<bool> stop_flag{false};

    auto start = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        for (int i = 0; i < kMsgCount && !stop_flag.load(std::memory_order_relaxed); ++i) {
            auto tick = MarketTick::make("PERF", static_cast<double>(i), 1.0, 0);
            while (!stop_flag.load(std::memory_order_relaxed) && !rb.try_push(std::move(tick))) {
                // spin
            }
            ++produced;
        }
    });

    std::thread consumer([&]() {
        MarketTick out;
        while (!stop_flag.load(std::memory_order_relaxed) && consumed.load(std::memory_order_relaxed) < kMsgCount) {
            if (rb.try_pop(out)) {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = end - start;

    double msgs_per_sec = kMsgCount / diff.count();
    GTEST_LOG_(INFO) << "Throughput Baseline: " << msgs_per_sec << " msgs/sec";
}

// ─── SUITE 4: ThreadBaseTest ───────────────────────────────────────────────

class TestWorker : public ThreadBase {
public:
    std::atomic<int> tick_count{0};

    explicit TestWorker() : ThreadBase("TestWorker") {}
    ~TestWorker() override { stop(); }

protected:
    void run(mdp::StopToken st) override {
        while (!st.stop_requested()) {
            ++tick_count;
            std::this_thread::sleep_for(100us);
        }
    }
};

TEST(ThreadBaseTest, WorkerStartsAndStops) {
    TestWorker worker;
    EXPECT_FALSE(worker.is_running());

    worker.start();
    // Yield shortly so the thread sets running_ to true
    std::this_thread::sleep_for(2ms);
    EXPECT_TRUE(worker.is_running());
    EXPECT_EQ(worker.name(), "TestWorker");

    std::this_thread::sleep_for(10ms);
    worker.stop();
    EXPECT_FALSE(worker.is_running());

    EXPECT_GT(worker.tick_count.load(), 0);
}

// Verifies the subclass destructor protocol:
// ~SubClass() { stop(); } must be called before members are destroyed.
// ~ThreadBase() asserts !joinable() — this test confirms the assert
// does NOT fire when the protocol is followed correctly.
TEST(ThreadBaseTest, ExplicitDestructorStopsThreadCleanly) {
    {
        TestWorker worker;
        worker.start();
        std::this_thread::sleep_for(5ms);
        // Destructor should stop the thread cleanly without hanging or terminating
    }
    SUCCEED();
}

TEST(ThreadBaseTest, DoubleStopIsIdempotent) {
    TestWorker worker;
    worker.start();
    std::this_thread::sleep_for(2ms);

    worker.stop();
    EXPECT_FALSE(worker.is_running());

    // Additional stop() calls should not crash or hang
    worker.stop();
    worker.stop();

    SUCCEED();
}
