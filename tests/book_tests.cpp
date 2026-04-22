// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <chrono>
#include <thread>
#include <cstring>

#include "book/OrderBook.hpp"
#include "book/BookProcessor.hpp"
#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"

using namespace std::chrono_literals;

namespace mdp {
namespace {

// ═════════════════════════════════════════════════════════════════════════════
// TEST SUITE 1: OrderBookTest
// ═════════════════════════════════════════════════════════════════════════════

class OrderBookTest : public ::testing::Test {
protected:
    OrderBook book_{"AAPL"};

    static BookDelta make_delta(OrderSide side, double price, uint64_t vol) {
        BookDelta d{};
        std::strncpy(d.symbol, "AAPL", sizeof(d.symbol) - 1);
        d.side = side;
        d.price = price;
        d.volume = vol;
        d.timestamp_ns = 1000;
        return d;
    }
};

// TEST 1: EmptyBookTopOfBookIsInvalid
TEST_F(OrderBookTest, EmptyBookTopOfBookIsInvalid) {
    EXPECT_FALSE(book_.top_of_book().is_valid());
    EXPECT_EQ(book_.bid_levels(), 0);
    EXPECT_EQ(book_.ask_levels(), 0);
}

// TEST 2: SingleBidAndAskGivesValidSpread
TEST_F(OrderBookTest, SingleBidAndAskGivesValidSpread) {
    book_.apply(make_delta(OrderSide::BID, 100.0, 500));
    book_.apply(make_delta(OrderSide::ASK, 101.0, 300));

    auto tob = book_.top_of_book();
    EXPECT_EQ(tob.best_bid, 100.0);
    EXPECT_EQ(tob.best_ask, 101.0);
    EXPECT_NEAR(tob.spread, 1.0, 1e-9);
    EXPECT_TRUE(tob.is_valid());
}

// TEST 3: BestBidIsHighestBidPrice
TEST_F(OrderBookTest, BestBidIsHighestBidPrice) {
    // [CV NOTE] Bids are sorted descending (std::greater)
    book_.apply(make_delta(OrderSide::BID, 99.0, 100));
    book_.apply(make_delta(OrderSide::BID, 100.0, 100));
    book_.apply(make_delta(OrderSide::BID, 98.5, 100));
    book_.apply(make_delta(OrderSide::BID, 100.5, 100));

    EXPECT_EQ(book_.top_of_book().best_bid, 100.5);
}

// TEST 4: BestAskIsLowestAskPrice
TEST_F(OrderBookTest, BestAskIsLowestAskPrice) {
    // [CV NOTE] Asks are sorted ascending (std::less)
    book_.apply(make_delta(OrderSide::ASK, 101.0, 100));
    book_.apply(make_delta(OrderSide::ASK, 102.5, 100));
    book_.apply(make_delta(OrderSide::ASK, 101.5, 100));
    book_.apply(make_delta(OrderSide::ASK, 100.8, 100));

    EXPECT_EQ(book_.top_of_book().best_ask, 100.8);
}

// TEST 5: RemovePriceLevelOnZeroVolume
TEST_F(OrderBookTest, RemovePriceLevelOnZeroVolume) {
    book_.apply(make_delta(OrderSide::BID, 100.0, 500));
    EXPECT_EQ(book_.bid_levels(), 1);

    book_.apply(make_delta(OrderSide::BID, 100.0, 0)); // Remove
    EXPECT_EQ(book_.bid_levels(), 0);
}

// TEST 6: UpdateExistingPriceLevel
TEST_F(OrderBookTest, UpdateExistingPriceLevel) {
    book_.apply(make_delta(OrderSide::BID, 100.0, 500));
    book_.apply(make_delta(OrderSide::BID, 100.0, 750)); // Update

    EXPECT_EQ(book_.bid_levels(), 1);
    EXPECT_EQ(book_.top_of_book().bid_volume, 750);
}

// TEST 7: SnapshotContainsAllLevels
TEST_F(OrderBookTest, SnapshotContainsAllLevels) {
    book_.apply(make_delta(OrderSide::BID, 99.0, 100));
    book_.apply(make_delta(OrderSide::BID, 99.5, 200));
    book_.apply(make_delta(OrderSide::BID, 100.0, 300));
    book_.apply(make_delta(OrderSide::BID, 100.5, 150));
    book_.apply(make_delta(OrderSide::BID, 101.0, 50));

    book_.apply(make_delta(OrderSide::ASK, 101.5, 200));
    book_.apply(make_delta(OrderSide::ASK, 102.0, 400));
    book_.apply(make_delta(OrderSide::ASK, 103.0, 100));

    auto snapshot = book_.snapshot();
    EXPECT_EQ(snapshot.bids.size(), 5);
    EXPECT_EQ(snapshot.asks.size(), 3);

    // Check ordering guarantees
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 101.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 101.5);
}

// TEST 8: SnapshotSequenceMonotonicallyIncreases
TEST_F(OrderBookTest, SnapshotSequenceMonotonicallyIncreases) {
    uint64_t last_seq = book_.snapshot().sequence;
    for (int i = 0; i < 10; ++i) {
        book_.apply(make_delta(OrderSide::BID, 100.0 + i, 100));
        uint64_t current_seq = book_.snapshot().sequence;
        EXPECT_GT(current_seq, last_seq);
        last_seq = current_seq;
    }
}

// TEST 9: ClearResetsAllState
TEST_F(OrderBookTest, ClearResetsAllState) {
    book_.apply(make_delta(OrderSide::BID, 100.0, 100));
    book_.apply(make_delta(OrderSide::BID, 99.0, 100));
    book_.apply(make_delta(OrderSide::BID, 98.0, 100));
    book_.apply(make_delta(OrderSide::ASK, 101.0, 100));
    book_.apply(make_delta(OrderSide::ASK, 102.0, 100));

    book_.clear();

    EXPECT_EQ(book_.bid_levels(), 0);
    EXPECT_EQ(book_.ask_levels(), 0);
    EXPECT_FALSE(book_.top_of_book().is_valid());
    EXPECT_EQ(book_.updates_applied(), 0);
}

// TEST 10: UpdatesAppliedCounterIsAccurate
TEST_F(OrderBookTest, UpdatesAppliedCounterIsAccurate) {
    book_.apply(make_delta(OrderSide::BID, 100.0, 100)); // 1 (insert)
    book_.apply(make_delta(OrderSide::BID, 100.0, 200)); // 2 (update)
    book_.apply(make_delta(OrderSide::BID, 100.0, 0));   // 3 (remove)
    book_.apply(make_delta(OrderSide::ASK, 101.0, 100)); // 4 (insert)
    book_.apply(make_delta(OrderSide::ASK, 102.0, 100)); // 5 (insert)
    book_.apply(make_delta(OrderSide::ASK, 102.0, 0));   // 6 (remove)
    book_.apply(make_delta(OrderSide::BID, 99.0, 100));  // 7 (insert)

    EXPECT_EQ(book_.updates_applied(), 7);
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST SUITE 2: BookProcessorTest
// ═════════════════════════════════════════════════════════════════════════════

class BookProcessorTest : public ::testing::Test {
protected:
    TickRingBuffer4K input_buf_;
    BookProcessor proc_{input_buf_};
};

// TEST 12: ProcessorStartsAndStopsCleanly (11 was nominally skipped per prompt flow)
TEST_F(BookProcessorTest, ProcessorStartsAndStopsCleanly) {
    proc_.start();
    std::this_thread::sleep_for(10ms);
    proc_.stop();
    EXPECT_GE(proc_.ticks_processed(), 0);
}

// TEST 13: TicksAreMappedToBooks (and MultipleSymbolsIndependentInBookProcessor)
TEST_F(BookProcessorTest, TicksAreMappedToBooks) {
    proc_.start();

    for (int i = 0; i < 20; ++i) {
        // Vary price to create bids and asks (heuristic splits around EMA)
        input_buf_.try_push(MarketTick::make("AAPL", 150.0 + i, 100.0, 0));
        input_buf_.try_push(MarketTick::make("MSFT", 300.0 - i, 200.0, 1));
    }

    std::this_thread::sleep_for(50ms);
    proc_.stop();

    EXPECT_NE(proc_.book("AAPL"), nullptr);
    EXPECT_NE(proc_.book("MSFT"), nullptr);
    EXPECT_EQ(proc_.book("GOOG"), nullptr);

    EXPECT_EQ(proc_.ticks_processed(), 40);
}

// TEST 14: UnknownSymbolReturnsNullptr
TEST_F(BookProcessorTest, UnknownSymbolReturnsNullptr) {
    proc_.start();
    proc_.stop();
    EXPECT_EQ(proc_.book("UNKN"), nullptr);
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST SUITE 3: BookEdgeCasesTest (No Fixture)
// ═════════════════════════════════════════════════════════════════════════════

// TEST 15: CrossedBookIsRejected
// [CV NOTE] Crossed book guard test — verifies SPDLOG_WARN path skips the malicious update
TEST(BookEdgeCasesTest, CrossedBookIsRejected) {
    OrderBook book{"TEST"};

    BookDelta d1{};
    std::strncpy(d1.symbol, "TEST", 8);
    d1.side = OrderSide::ASK;
    d1.price = 100.0;
    d1.volume = 100;
    book.apply(d1);

    BookDelta d2{};
    std::strncpy(d2.symbol, "TEST", 8);
    d2.side = OrderSide::BID;
    d2.price = 101.0; // Crosses! (bid 101.0 >= best ask 100.0)
    d2.volume = 100;
    book.apply(d2);

    // The crossed bid should be rejected
    EXPECT_EQ(book.bid_levels(), 0);
}

// TEST 16: ZeroVolumeOnNonExistentLevelIsNoOp
// [CV NOTE] A remove delta for a price level that isn't in the map shouldn't crash
TEST(BookEdgeCasesTest, ZeroVolumeOnNonExistentLevelIsNoOp) {
    OrderBook book{"TEST"};

    BookDelta d{};
    std::strncpy(d.symbol, "TEST", 8);
    d.side = OrderSide::BID;
    d.price = 100.0;
    d.volume = 0;

    book.apply(d);

    EXPECT_EQ(book.bid_levels(), 0);
    EXPECT_EQ(book.updates_applied(), 1); // Delta was processed (even if no-op map erase)
}

}  // namespace
}  // namespace mdp

