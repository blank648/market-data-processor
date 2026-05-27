// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file extended_tests.cpp
 * @brief Mock-data driven tests covering gaps identified in the data integrity audit.
 *
 * Coverage:
 *  1. MarketTick::make timestamp is Unix epoch nanoseconds (system_clock fix)
 *  2. TickParser::enrich() side clamping and 7-char symbol null-termination
 *  3. Normalizer: same-timestamp different-price must NOT be deduplicated
 *  4. BookProcessor 2-arg constructor: snapshots reach db_queue
 *  5. BookProcessor 3-arg constructor: snapshots reach both db_queue and signal_queue
 *  6. BookProcessor mid-price = (bid+ask)/2 when both sides are present
 *  7. BookProcessor snapshots_dropped() counter increments when queue is full
 *  8. SignalEngine: HOLD emitted when price is within ±0.1% of EMA
 *  9. SignalEngine: HOLD signals are forwarded to output queue (not filtered)
 * 10. SignalEngine: independent EMA per symbol (AAPL BUY does not affect MSFT)
 * 11. Full pipeline: FeedSimulator → Parser → Normalizer → BookProcessor → SignalEngine
 * 12. Full pipeline graceful shutdown with all 5 stages
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "book/BookProcessor.hpp"
#include "core/MarketSnapshot.hpp"
#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "feed/FeedConfig.hpp"
#include "feed/FeedSimulator.hpp"
#include "infra/Logger.hpp"
#include "processing/Normalizer.hpp"
#include "processing/TickParser.hpp"
#include "strategy/Signal.hpp"
#include "strategy/SignalEngine.hpp"

using namespace mdp;
using namespace std::chrono_literals;

// ── Shared test helpers ──────────────────────────────────────────────────────

template <typename Pred>
static bool wait_until(Pred pred, std::chrono::milliseconds timeout = 500ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::yield();
    }
    return false;
}

template <typename T, std::size_t N>
static bool wait_for_item(RingBuffer<T, N>& q, T& out,
                          std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (q.try_pop(out)) return true;
        std::this_thread::yield();
    }
    return false;
}

template <typename T, std::size_t N>
static void drain_buf(RingBuffer<T, N>& buf,
                      std::chrono::milliseconds timeout = 200ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!buf.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

template <typename T, std::size_t N>
static std::vector<T> collect_all(RingBuffer<T, N>& q) {
    std::vector<T> out;
    T item{};
    while (q.try_pop(item)) out.push_back(item);
    return out;
}

// ═════════════════════════════════════════════════════════════════════════════
// SUITE 1 — MarketTickTimestampTest
// Verifies that MarketTick::make() uses system_clock (Unix epoch) after the
// steady_clock → system_clock fix in MarketTick.hpp.
// ═════════════════════════════════════════════════════════════════════════════

TEST(MarketTickTimestampTest, MakeTimestampIsUnixEpochNanoseconds) {
    const auto tick = MarketTick::make("AAPL", 150.0, 1000.0, 0);

    // Unix epoch nanoseconds lower bound: 2024-01-01 ≈ 1.704e18 ns
    // Upper bound: 2034-01-01 ≈ 2.019e18 ns
    constexpr int64_t kMin = 1'700'000'000LL * 1'000'000'000LL;
    constexpr int64_t kMax = 2'020'000'000LL * 1'000'000'000LL;

    EXPECT_GE(tick.timestamp_ns, kMin)
        << "timestamp_ns is below the 2024 floor — steady_clock may still be in use";
    EXPECT_LE(tick.timestamp_ns, kMax)
        << "timestamp_ns is unreasonably far in the future";
}

TEST(MarketTickTimestampTest, ConsecutiveMakeCallsAreNonDecreasing) {
    const auto t1 = MarketTick::make("AAPL", 150.0, 100.0, 0);
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    const auto t2 = MarketTick::make("AAPL", 150.0, 100.0, 0);

    EXPECT_GE(t2.timestamp_ns, t1.timestamp_ns)
        << "Consecutive make() calls must produce non-decreasing timestamps";
}

// ═════════════════════════════════════════════════════════════════════════════
// SUITE 2 — TickParserEnrichTest
// enrich() is called before validation; covers side clamping and null-term.
// ═════════════════════════════════════════════════════════════════════════════

class TickParserEnrichTest : public ::testing::Test {
private:
    TickRingBuffer16K input_;
    TickRingBuffer4K  output_;

protected:
    TickRingBuffer16K& input()  { return input_; }
    TickRingBuffer4K&  output() { return output_; }
};

TEST_F(TickParserEnrichTest, SideAboveTwoIsClampedByEnrichAndForwarded) {
    // After the enrich-before-validate fix, enrich() clamps side to min(side,2)
    // before validate() runs its side > 2 check. So side=5 → clamped to 2 → valid → forwarded.
    auto tick = MarketTick::make("AAPL", 150.0, 100.0, 0);
    tick.side = 5;
    ASSERT_TRUE(input().try_push(tick));

    TickParser parser(input(), output());
    parser.start();
    ASSERT_TRUE(wait_until([&] {
        return parser.ticks_processed() + parser.ticks_rejected() >= 1;
    }));
    parser.stop();

    MarketTick out;
    ASSERT_TRUE(output().try_pop(out))
        << "Tick with side=5 must be clamped to 2 by enrich() and forwarded";
    EXPECT_EQ(out.side, 2U);
    EXPECT_EQ(parser.ticks_rejected(), 0U);
    EXPECT_EQ(parser.ticks_processed(), 1U);
}

TEST_F(TickParserEnrichTest, SevenCharSymbolHasNullTerminatorAtIndex7) {
    // "ABCDEFG" fills symbol[0..6]; enrich() forces symbol[7] = '\0'
    auto tick = MarketTick::make("ABCDEFG", 100.0, 10.0, 0);
    ASSERT_TRUE(input().try_push(tick));

    TickParser parser(input(), output());
    parser.start();
    ASSERT_TRUE(wait_until([&] { return parser.ticks_processed() >= 1; }));
    parser.stop();

    MarketTick out;
    ASSERT_TRUE(output().try_pop(out));
    EXPECT_EQ(out.symbol[7], '\0') << "enrich() must null-terminate symbol at index 7";

    const std::string_view sv(out.symbol.data(),
                               ::strnlen(out.symbol.data(), out.symbol.size()));
    EXPECT_EQ(sv, "ABCDEFG");
}

// ═════════════════════════════════════════════════════════════════════════════
// SUITE 3 — NormalizerBoundaryTest
// Dedup fires only on exact (price == last_price && ts == last_ts) match.
// ═════════════════════════════════════════════════════════════════════════════

class NormalizerBoundaryTest : public ::testing::Test {
private:
    TickRingBuffer4K input_;
    TickRingBuffer4K output_;

protected:
    TickRingBuffer4K& input()  { return input_; }
    TickRingBuffer4K& output() { return output_; }
};

TEST_F(NormalizerBoundaryTest, SameTimestampDifferentPriceIsNotDeduplicated) {
    auto t1 = MarketTick::make("AAPL", 100.0, 1.0, 0);
    t1.timestamp_ns = 5000LL;
    auto t2 = MarketTick::make("AAPL", 101.0, 1.0, 0);  // same ts, different price
    t2.timestamp_ns = 5000LL;

    ASSERT_TRUE(input().try_push(t1));
    ASSERT_TRUE(input().try_push(t2));

    Normalizer norm(input(), output());
    norm.start();
    ASSERT_TRUE(wait_until([&] {
        const auto s = norm.stats();
        return s.ticks_forwarded + s.ticks_deduplicated + s.ticks_reordered >= 2;
    }));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 2U)
        << "Same timestamp but different price must NOT be treated as duplicate";
    EXPECT_EQ(norm.stats().ticks_deduplicated, 0U);
}

TEST_F(NormalizerBoundaryTest, SameTimestampSamePriceIsDeduplicated) {
    auto t1 = MarketTick::make("AAPL", 100.0, 1.0, 0);
    t1.timestamp_ns = 5000LL;
    const auto t2 = t1;  // exact copy

    ASSERT_TRUE(input().try_push(t1));
    ASSERT_TRUE(input().try_push(t2));

    Normalizer norm(input(), output());
    norm.start();
    ASSERT_TRUE(wait_until([&] {
        const auto s = norm.stats();
        return s.ticks_forwarded + s.ticks_deduplicated + s.ticks_reordered >= 2;
    }));
    norm.stop();

    EXPECT_EQ(norm.stats().ticks_forwarded, 1U);
    EXPECT_EQ(norm.stats().ticks_deduplicated, 1U);
}

// ═════════════════════════════════════════════════════════════════════════════
// SUITE 4 — BookProcessorOutputQueueTest
// 2-arg / 3-arg constructors, snapshot delivery, mid-price, drop counter.
// ═════════════════════════════════════════════════════════════════════════════

class BookProcessorOutputQueueTest : public ::testing::Test {
protected:
    TickRingBuffer4K     input_;
    SnapshotRingBuffer4K db_queue_;
    SnapshotRingBuffer4K signal_queue_;
};

TEST_F(BookProcessorOutputQueueTest, TwoArgConstructorPushesSnapshotToDbQueue) {
    BookProcessor proc(input_, db_queue_);
    proc.start();

    // Tick 1: first tick for AAPL → reference set, side=BID, book gains bid level
    // Top-of-book changes → snapshot pushed to db_queue_
    input_.try_push(MarketTick::make("AAPL", 150.0, 100.0, 0));
    ASSERT_TRUE(wait_until([&] { return proc.ticks_processed() >= 1; }));
    proc.stop();

    MarketSnapshot snap;
    EXPECT_TRUE(db_queue_.try_pop(snap))
        << "2-arg constructor must push snapshot to db_queue on top-of-book change";
    EXPECT_GT(snap.price, 0.0);

    // signal_queue_ is not connected to this proc — must stay empty
    MarketSnapshot ignored;
    EXPECT_FALSE(signal_queue_.try_pop(ignored))
        << "signal_queue must remain empty with 2-arg constructor";
}

TEST_F(BookProcessorOutputQueueTest, ThreeArgConstructorPushesSnapshotToBothQueues) {
    BookProcessor proc(input_, db_queue_, signal_queue_);
    proc.start();

    input_.try_push(MarketTick::make("MSFT", 300.0, 200.0, 0));
    ASSERT_TRUE(wait_until([&] { return proc.ticks_processed() >= 1; }));
    proc.stop();

    MarketSnapshot db_snap, sig_snap;
    EXPECT_TRUE(db_queue_.try_pop(db_snap))
        << "3-arg constructor must push snapshot to db_queue";
    EXPECT_TRUE(signal_queue_.try_pop(sig_snap))
        << "3-arg constructor must push snapshot to signal_queue";

    EXPECT_GT(db_snap.price, 0.0);
    EXPECT_GT(sig_snap.price, 0.0);
    EXPECT_DOUBLE_EQ(db_snap.price, sig_snap.price)
        << "Both queues must receive the identical snapshot";
}

TEST_F(BookProcessorOutputQueueTest, SnapshotPriceIsMidpointWhenBothSidesPresent) {
    // Tick 1 (GOOG 100.0): first tick → reference=100, side=BID → bid[100]=50
    //   snap: only bid present → price = best_bid = 100.0
    // Tick 2 (GOOG 110.0): price(110)>=ref(100) → side=ASK; 110 > best_bid(100) → no cross
    //   book: bid[100]=50, ask[110]=50 → mid = (100+110)/2 = 105.0
    BookProcessor proc(input_, db_queue_);
    proc.start();

    input_.try_push(MarketTick::make("GOOG", 100.0, 50.0, 0));
    ASSERT_TRUE(wait_until([&] { return proc.ticks_processed() >= 1; }));

    input_.try_push(MarketTick::make("GOOG", 110.0, 50.0, 0));
    ASSERT_TRUE(wait_until([&] { return proc.ticks_processed() >= 2; }));
    proc.stop();

    const auto snaps = collect_all(db_queue_);
    ASSERT_GE(snaps.size(), 1U);

    // Last snapshot reflects the two-sided book: mid = 105.0
    EXPECT_NEAR(snaps.back().price, 105.0, 0.1);
}

TEST_F(BookProcessorOutputQueueTest, SnapshotsDroppedCounterIncrementsWhenDbQueueFull) {
    // Fill db_queue_ to capacity so every subsequent push will fail.
    MarketSnapshot dummy{};
    int filled = 0;
    while (db_queue_.try_push(dummy)) { ++filled; }
    ASSERT_GT(filled, 0);
    ASSERT_FALSE(db_queue_.try_push(dummy)) << "Queue must be full after fill loop";

    BookProcessor proc(input_, db_queue_);
    proc.start();

    // First tick for "DROP" changes top-of-book → snapshot emitted → push fails → counter++
    input_.try_push(MarketTick::make("DROP", 50.0, 10.0, 0));
    ASSERT_TRUE(wait_until([&] { return proc.ticks_processed() >= 1; }));
    proc.stop();

    EXPECT_GE(proc.snapshots_dropped(), 1U)
        << "snapshots_dropped must increment when db_queue is full";
}

TEST_F(BookProcessorOutputQueueTest, SnapshotsDroppedInitiallyZero) {
    BookProcessor proc(input_, db_queue_, signal_queue_);
    EXPECT_EQ(proc.snapshots_dropped(), 0U);
}

// ═════════════════════════════════════════════════════════════════════════════
// SUITE 5 — SignalEngineExtendedTest
// HOLD boundary, HOLD forwarding, multi-symbol independence.
// ═════════════════════════════════════════════════════════════════════════════

class SignalEngineExtendedTest : public ::testing::Test {
private:
    SnapshotRingBuffer4K in_;
    SignalRingBuffer4K   out_;
    SignalEngine         engine_{in_, out_, 20};

protected:
    SnapshotRingBuffer4K& in_queue()  { return in_; }
    SignalRingBuffer4K&   out_queue() { return out_; }

    void SetUp() override { engine_.start(); }
    void TearDown() override { engine_.stop(); }

    MarketSnapshot make_snap(const char* sym, double price) {
        MarketSnapshot s{};
        s.price = price;
        s.volume = 1.0;
        s.timestamp_ns = 0;
        s.sequence = 0;
        const auto len = std::min(std::strlen(sym), s.symbol.size() - 1);
        std::memcpy(s.symbol.data(), sym, len);
        s.symbol[len] = '\0';
        return s;
    }

    Signal pop_signal() {
        Signal sig{};
        EXPECT_TRUE(wait_for_item(out_queue(), sig, 1s))
            << "Expected a signal within 1s";
        return sig;
    }
};

TEST_F(SignalEngineExtendedTest, HoldSignalsAreForwardedToOutputQueue) {
    // First tick always initializes EMA = price → HOLD. Verify HOLD reaches output.
    in_queue().try_push(make_snap("HOLD1", 200.0));

    Signal sig{};
    ASSERT_TRUE(wait_for_item(out_queue(), sig, 1s))
        << "HOLD signal must be pushed to the output queue — it must not be filtered";
    EXPECT_EQ(sig.type, SignalType::HOLD);
}

TEST_F(SignalEngineExtendedTest, HoldSignalEmittedWhenPriceWithinEmaMargin) {
    // Init: EMA = 100.0
    in_queue().try_push(make_snap("HMRG", 100.0));
    const Signal s1 = pop_signal();
    ASSERT_EQ(s1.type, SignalType::HOLD);
    ASSERT_DOUBLE_EQ(s1.ema_value, 100.0);

    // price=100.05 is 0.05% above EMA(~100.0).
    // BUY: ema(100.0) < 100.05*0.999(99.9500) → FALSE  (100.0 > 99.95)
    // SELL: ema(100.0) > 100.05*1.001(100.150) → FALSE
    in_queue().try_push(make_snap("HMRG", 100.05));
    const Signal s2 = pop_signal();
    EXPECT_EQ(s2.type, SignalType::HOLD)
        << "Price 0.05% above EMA must stay HOLD, not trigger BUY";
}

TEST_F(SignalEngineExtendedTest, BuySignalEmittedOnLargeUpwardMove) {
    // EMA=100, price jumps to 115 → ema(100) < 115*0.999(114.885) → BUY
    in_queue().try_push(make_snap("BSYM", 100.0));
    ASSERT_EQ(pop_signal().type, SignalType::HOLD);

    in_queue().try_push(make_snap("BSYM", 115.0));
    const Signal s = pop_signal();
    EXPECT_EQ(s.type, SignalType::BUY);
    EXPECT_DOUBLE_EQ(s.price, 115.0);
}

TEST_F(SignalEngineExtendedTest, SellSignalEmittedOnLargeDownwardMove) {
    // EMA=100, price drops to 85 → ema(100) > 85*1.001(85.085) → SELL
    in_queue().try_push(make_snap("SSYM", 100.0));
    ASSERT_EQ(pop_signal().type, SignalType::HOLD);

    in_queue().try_push(make_snap("SSYM", 85.0));
    const Signal s = pop_signal();
    EXPECT_EQ(s.type, SignalType::SELL);
    EXPECT_DOUBLE_EQ(s.price, 85.0);
}

TEST_F(SignalEngineExtendedTest, MultipleSymbolsMaintainIndependentEmaState) {
    // Init AAPL @ 100.0, MSFT @ 200.0
    in_queue().try_push(make_snap("AAPL", 100.0));
    const Signal a1 = pop_signal();
    EXPECT_EQ(a1.type, SignalType::HOLD);

    in_queue().try_push(make_snap("MSFT", 200.0));
    const Signal m1 = pop_signal();
    EXPECT_EQ(m1.type, SignalType::HOLD);

    // AAPL jumps to 120 → BUY  (ema≈100 < 120*0.999=119.88 → TRUE)
    in_queue().try_push(make_snap("AAPL", 120.0));
    const Signal a2 = pop_signal();
    EXPECT_EQ(a2.type, SignalType::BUY)
        << "AAPL jump from 100→120 must trigger BUY";
    EXPECT_EQ(a2.symbol_view(), "AAPL");

    // MSFT drops to 180 → SELL  (ema≈200 > 180*1.001=180.18 → TRUE)
    in_queue().try_push(make_snap("MSFT", 180.0));
    const Signal m2 = pop_signal();
    EXPECT_EQ(m2.type, SignalType::SELL)
        << "MSFT drop from 200→180 must trigger SELL";
    EXPECT_EQ(m2.symbol_view(), "MSFT");
}

// ═════════════════════════════════════════════════════════════════════════════
// SUITE 6 — FullPipelineWithSignalEngineTest
// End-to-end: FeedSimulator → Parser → Normalizer → BookProcessor → SignalEngine
// ═════════════════════════════════════════════════════════════════════════════

class FullPipelineWithSignalEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        mdp::Logger::init("full-pipe-test", spdlog::level::warn);
    }
    void TearDown() override {
        mdp::Logger::shutdown();
    }
};

TEST_F(FullPipelineWithSignalEngineTest, PipelineProducesValidSignalsFromFeedData) {
    TickRingBuffer16K    sim_to_parser;
    TickRingBuffer4K     parser_to_norm;
    TickRingBuffer4K     norm_to_book;
    SnapshotRingBuffer4K book_to_engine;
    SignalRingBuffer4K   signal_out;

    FeedConfig cfg = FeedConfig::default_config();
    cfg.tick_rate_hz  = 2'000;
    cfg.symbols        = {"AAPL", "MSFT", "GOOG"};
    cfg.initial_prices = {150.0, 300.0, 2800.0};

    FeedSimulator sim(cfg, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_to_book);
    BookProcessor book(norm_to_book, book_to_engine);   // 2-arg: book_to_engine is db_queue
    SignalEngine  engine(book_to_engine, signal_out, 20);

    // Start consumers before producers
    engine.start();
    book.start();
    norm.start();
    parser.start();
    sim.start();

    std::this_thread::sleep_for(500ms);

    // Stop in producer-first order with drain between stages
    sim.stop();
    drain_buf(sim_to_parser);
    parser.stop();
    drain_buf(parser_to_norm);
    norm.stop();
    drain_buf(norm_to_book);
    book.stop();
    drain_buf(book_to_engine);
    engine.stop();

    const auto signals = collect_all(signal_out);

    ASSERT_GT(signals.size(), 0U)
        << "Full 5-stage pipeline must produce at least one signal in 500ms";

    size_t buys = 0, sells = 0, holds = 0;
    for (const auto& sig : signals) {
        EXPECT_GT(sig.price, 0.0);
        EXPECT_GT(sig.ema_value, 0.0);
        EXPECT_NE(sig.symbol[0], '\0');
        EXPECT_TRUE(sig.type == SignalType::BUY ||
                    sig.type == SignalType::SELL ||
                    sig.type == SignalType::HOLD);

        if (sig.type == SignalType::BUY)  ++buys;
        if (sig.type == SignalType::SELL) ++sells;
        if (sig.type == SignalType::HOLD) ++holds;
    }

    GTEST_LOG_(INFO) << "Signals: " << signals.size()
                     << "  BUY=" << buys << "  SELL=" << sells << "  HOLD=" << holds;

    // The primary assertion is end-to-end plumbing: all 5 stages are wired
    // and signals flow through. BUY/SELL generation depends on the EMA
    // converging enough to cross the 0.1% band; that is verified in the
    // SignalEngineExtendedTest unit tests which inject controlled prices.
    GTEST_LOG_(INFO) << "Note: BUY/SELL rate depends on top-of-book change frequency "
                        "and EMA convergence — tested deterministically in SignalEngineExtendedTest";
}

TEST_F(FullPipelineWithSignalEngineTest, FiveStageShutdownCompletesWithinThreeSeconds) {
    TickRingBuffer16K    sim_to_parser;
    TickRingBuffer4K     parser_to_norm;
    TickRingBuffer4K     norm_to_book;
    SnapshotRingBuffer4K book_to_engine;
    SignalRingBuffer4K   signal_out;

    FeedConfig cfg = FeedConfig::default_config();
    cfg.tick_rate_hz = 5'000;

    FeedSimulator sim(cfg, sim_to_parser);
    TickParser    parser(sim_to_parser, parser_to_norm);
    Normalizer    norm(parser_to_norm, norm_to_book);
    BookProcessor book(norm_to_book, book_to_engine);
    SignalEngine  engine(book_to_engine, signal_out, 20);

    engine.start();
    book.start();
    norm.start();
    parser.start();
    sim.start();

    std::this_thread::sleep_for(100ms);

    const auto shutdown = std::async(std::launch::async, [&] {
        sim.stop();
        parser.stop();
        norm.stop();
        book.stop();
        engine.stop();
    });

    EXPECT_EQ(shutdown.wait_for(3s), std::future_status::ready)
        << "5-stage pipeline (including SignalEngine) must shut down within 3s";
}

// ═════════════════════════════════════════════════════════════════════════════
// SUITE 7 — PostgresFeedReaderUnitTest
// Unit tests that do NOT require a live PostgreSQL instance.
// Construction is always safe because pqxx::connection is created only in run().
// The interruptible_sleep helper (100ms slices) means stop() returns in ≤200ms
// even when the 5s retry interval is active.
// ═════════════════════════════════════════════════════════════════════════════

#include "feed/PostgresFeedReader.hpp"

class PostgresFeedReaderUnitTest : public ::testing::Test {
protected:
    TickRingBuffer16K output_;
    FeedConfig        cfg_ = FeedConfig::default_config();
};

TEST_F(PostgresFeedReaderUnitTest, ConstructsWithoutThrowingOnInvalidConnString) {
    EXPECT_NO_THROW({
        PostgresFeedReader reader(cfg_, "host=127.0.0.1 port=9 dbname=no", output_);
    });
}

TEST_F(PostgresFeedReaderUnitTest, InitialCountersAreZero) {
    PostgresFeedReader reader(cfg_, "host=127.0.0.1 port=9 dbname=no", output_);
    EXPECT_EQ(reader.ticks_published(), 0U);
    EXPECT_EQ(reader.ticks_dropped(), 0U);
}

TEST_F(PostgresFeedReaderUnitTest, SourceNameIsPostgresFeedReader) {
    PostgresFeedReader reader(cfg_, "host=127.0.0.1 port=9 dbname=no", output_);
    EXPECT_EQ(reader.source_name(), "PostgresFeedReader");
}

TEST_F(PostgresFeedReaderUnitTest, StopHonoursInterruptibleSleepAndReturnsQuickly) {
    // With an unreachable port, run() catches the connection exception and enters
    // interruptible_sleep(5000ms, 100ms). stop() sets the stop_token; the next
    // 100ms slice check exits the sleep so join() completes well within 2s.
    PostgresFeedReader reader(cfg_, "host=127.0.0.1 port=9 dbname=no", output_);
    reader.start();

    std::this_thread::sleep_for(200ms);  // let it fail and enter the retry sleep

    const auto shutdown = std::async(std::launch::async, [&reader] { reader.stop(); });
    EXPECT_EQ(shutdown.wait_for(2s), std::future_status::ready)
        << "stop() must interrupt the 5s retry delay and complete within 2s";
}

TEST_F(PostgresFeedReaderUnitTest, IsRunningTransitionsCorrectly) {
    PostgresFeedReader reader(cfg_, "host=127.0.0.1 port=9 dbname=no", output_);
    EXPECT_FALSE(reader.is_running());
    reader.start();
    ASSERT_TRUE(wait_until([&reader] { return reader.is_running(); }, 500ms));
    reader.stop();
    ASSERT_TRUE(wait_until([&reader] { return !reader.is_running(); }, 2s));
}
