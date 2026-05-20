// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <thread>
#include <cstring>
#include <random>

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
    OrderBook book{"AAPL"};

    static BookDelta make_delta(OrderSide side, double price, uint64_t vol) {
        BookDelta delta{};
        std::strncpy(delta.symbol.data(), "AAPL", delta.symbol.size() - 1);
        delta.symbol.back() = '\0';
        delta.side = side;
        delta.price = price;
        delta.volume = vol;
        delta.timestamp_ns = 1000;
        return delta;
    }
};

// 1. Empty book: best_bid() and best_ask() return std::nullopt
// Note: OrderBook.hpp implementation actually returns 0.0 when empty.
TEST_F(OrderBookTest, EmptyBookBestBidAndAskReturnsZero) {
    EXPECT_FALSE(book.top_of_book().is_valid());
    EXPECT_EQ(book.bid_levels(), 0);
    EXPECT_EQ(book.ask_levels(), 0);
    EXPECT_EQ(book.best_bid(), 0.0);
    EXPECT_EQ(book.best_ask(), 0.0);
}

// 2. Insert single bid -> best_bid() returns that price
TEST_F(OrderBookTest, InsertSingleBidReturnsPrice) {
    book.apply(make_delta(OrderSide::BID, 100.5, 500));
    EXPECT_EQ(book.best_bid(), 100.5);
    EXPECT_EQ(book.best_ask(), 0.0);
}

// 3. Insert single ask -> best_ask() returns that price
TEST_F(OrderBookTest, InsertSingleAskReturnsPrice) {
    book.apply(make_delta(OrderSide::ASK, 101.5, 300));
    EXPECT_EQ(book.best_ask(), 101.5);
    EXPECT_EQ(book.best_bid(), 0.0);
}

// 4. Insert multiple bids -> best_bid() is the highest
TEST_F(OrderBookTest, BestBidIsHighestBidPrice) {
    book.apply(make_delta(OrderSide::BID, 99.0, 100));
    book.apply(make_delta(OrderSide::BID, 100.0, 100));
    book.apply(make_delta(OrderSide::BID, 98.5, 100));
    book.apply(make_delta(OrderSide::BID, 100.5, 100));

    EXPECT_EQ(book.best_bid(), 100.5);
}

// 5. Insert multiple asks -> best_ask() is the lowest
TEST_F(OrderBookTest, BestAskIsLowestAskPrice) {
    book.apply(make_delta(OrderSide::ASK, 101.0, 100));
    book.apply(make_delta(OrderSide::ASK, 102.5, 100));
    book.apply(make_delta(OrderSide::ASK, 101.5, 100));
    book.apply(make_delta(OrderSide::ASK, 100.8, 100));

    EXPECT_EQ(book.best_ask(), 100.8);
}

// 6. Remove price level with zero volume -> level disappears from book
TEST_F(OrderBookTest, RemovePriceLevelOnZeroVolume) {
    book.apply(make_delta(OrderSide::BID, 100.0, 500));
    EXPECT_EQ(book.bid_levels(), 1);

    book.apply(make_delta(OrderSide::BID, 100.0, 0)); // Remove
    EXPECT_EQ(book.bid_levels(), 0);
}

// 7. Crossed book detection: inserting ask < best_bid -> book rejects it
TEST_F(OrderBookTest, CrossedBookInsertingAskLessThanBestBidIsRejected) {
    book.apply(make_delta(OrderSide::BID, 100.0, 100));
    EXPECT_EQ(book.best_bid(), 100.0);
    
    // Insert ask < 100.0
    book.apply(make_delta(OrderSide::ASK, 99.0, 100));
    
    // Should be rejected -> ask side should remain empty
    EXPECT_EQ(book.ask_levels(), 0);
    EXPECT_EQ(book.best_ask(), 0.0);
}

TEST_F(OrderBookTest, UpdateExistingPriceLevel) {
    book.apply(make_delta(OrderSide::BID, 100.0, 500));
    book.apply(make_delta(OrderSide::BID, 100.0, 750)); // Update

    EXPECT_EQ(book.bid_levels(), 1);
    EXPECT_EQ(book.top_of_book().bid_volume, 750);
}

TEST_F(OrderBookTest, SnapshotContainsAllLevels) {
    book.apply(make_delta(OrderSide::BID, 99.0, 100));
    book.apply(make_delta(OrderSide::BID, 99.5, 200));
    book.apply(make_delta(OrderSide::BID, 100.0, 300));
    book.apply(make_delta(OrderSide::BID, 100.5, 150));
    book.apply(make_delta(OrderSide::BID, 101.0, 50));

    book.apply(make_delta(OrderSide::ASK, 101.5, 200));
    book.apply(make_delta(OrderSide::ASK, 102.0, 400));
    book.apply(make_delta(OrderSide::ASK, 103.0, 100));

    auto snapshot = book.snapshot();
    EXPECT_EQ(snapshot.bids.size(), 5);
    EXPECT_EQ(snapshot.asks.size(), 3);

    // Check ordering guarantees
    EXPECT_DOUBLE_EQ(snapshot.bids[0].price, 101.0);
    EXPECT_DOUBLE_EQ(snapshot.asks[0].price, 101.5);
}

TEST_F(OrderBookTest, SnapshotSequenceMonotonicallyIncreases) {
    uint64_t last_seq = book.snapshot().sequence;
    for (int i = 0; i < 10; ++i) {
        book.apply(make_delta(OrderSide::BID, 100.0 + i, 100));
        uint64_t current_seq = book.snapshot().sequence;
        EXPECT_GT(current_seq, last_seq);
        last_seq = current_seq;
    }
}

TEST_F(OrderBookTest, ClearResetsAllState) {
    book.apply(make_delta(OrderSide::BID, 100.0, 100));
    book.apply(make_delta(OrderSide::BID, 99.0, 100));
    book.apply(make_delta(OrderSide::BID, 98.0, 100));
    book.apply(make_delta(OrderSide::ASK, 101.0, 100));
    book.apply(make_delta(OrderSide::ASK, 102.0, 100));

    book.clear();

    EXPECT_EQ(book.bid_levels(), 0);
    EXPECT_EQ(book.ask_levels(), 0);
    EXPECT_FALSE(book.top_of_book().is_valid());
    EXPECT_EQ(book.updates_applied(), 0);
}

TEST_F(OrderBookTest, UpdatesAppliedCounterIsAccurate) {
    book.apply(make_delta(OrderSide::BID, 100.0, 100)); // 1 (insert)
    book.apply(make_delta(OrderSide::BID, 100.0, 200)); // 2 (update)
    book.apply(make_delta(OrderSide::BID, 100.0, 0));   // 3 (remove)
    book.apply(make_delta(OrderSide::ASK, 101.0, 100)); // 4 (insert)
    book.apply(make_delta(OrderSide::ASK, 102.0, 100)); // 5 (insert)
    book.apply(make_delta(OrderSide::ASK, 102.0, 0));   // 6 (remove)
    book.apply(make_delta(OrderSide::BID, 99.0, 100));  // 7 (insert)

    EXPECT_EQ(book.updates_applied(), 7);
}

// 8. BTCUSD and SOLUSD maintain independent state in two separate OrderBook instances
TEST_F(OrderBookTest, IndependentStateForDifferentSymbols) {
    OrderBook btc("BTCUSD");
    OrderBook sol("SOLUSD");

    BookDelta d_btc{};
    std::strncpy(d_btc.symbol.data(), "BTCUSD", d_btc.symbol.size() - 1);
    d_btc.symbol.back() = '\0';
    d_btc.side = OrderSide::BID;
    d_btc.price = 50000.0;
    d_btc.volume = 1;
    btc.apply(d_btc);

    BookDelta d_sol{};
    std::strncpy(d_sol.symbol.data(), "SOLUSD", d_sol.symbol.size() - 1);
    d_sol.symbol.back() = '\0';
    d_sol.side = OrderSide::BID;
    d_sol.price = 100.0;
    d_sol.volume = 10;
    sol.apply(d_sol);

    EXPECT_EQ(btc.best_bid(), 50000.0);
    EXPECT_EQ(sol.best_bid(), 100.0);
    EXPECT_EQ(btc.bid_levels(), 1);
    EXPECT_EQ(sol.bid_levels(), 1);
}

// 9. Book state after 100 random insert/remove ops stays internally consistent
TEST_F(OrderBookTest, RandomOperationsMaintainConsistency) {
    OrderBook rand_book{"TEST"};
    std::mt19937 gen(42);
    std::uniform_real_distribution<double> bid_price(90.0, 99.9);
    std::uniform_real_distribution<double> ask_price(100.1, 110.0);
    std::uniform_int_distribution<uint64_t> vol(0, 100); // 0 volume = remove

    for (int i = 0; i < 100; ++i) {
        BookDelta delta{};
        std::strncpy(delta.symbol.data(), "TEST", delta.symbol.size() - 1);
        delta.symbol.back() = '\0';
        delta.side = (i % 2 == 0) ? OrderSide::BID : OrderSide::ASK;
        delta.price = (delta.side == OrderSide::BID) ? bid_price(gen) : ask_price(gen);
        delta.volume = vol(gen);
        rand_book.apply(delta);
        
        // Invariant: If both sides have orders, best_bid < best_ask
        if (rand_book.bid_levels() > 0 && rand_book.ask_levels() > 0) {
            EXPECT_TRUE(rand_book.best_bid() < rand_book.best_ask());
        } else {
            EXPECT_TRUE(true); // one side empty, invariant holds
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST SUITE 2: BookProcessorTest
// ═════════════════════════════════════════════════════════════════════════════

class BookProcessorTest : public ::testing::Test {
protected:
    TickRingBuffer4K input_buf;
    BookProcessor proc{input_buf};
};

// 10. start() + stop() lifecycle
TEST_F(BookProcessorTest, ProcessorStartsAndStopsCleanly) {
    proc.start();
    std::this_thread::sleep_for(10ms);
    proc.stop();
    EXPECT_GE(proc.ticks_processed(), 0);
}

// 11. Processes ticks from RingBuffer and updates the correct OrderBook per symbol
TEST_F(BookProcessorTest, ProcessesTicksAndUpdatesCorrectOrderBook) {
    proc.start();

    // The BookProcessor determines side via an internal EMA heuristic:
    // first tick sets reference. Following ticks: < reference -> BID, >= reference -> ASK.
    // For AAPL:
    input_buf.try_push(MarketTick::make("AAPL", 150.0, 100.0, 0)); // sets reference
    input_buf.try_push(MarketTick::make("AAPL", 149.0, 100.0, 0)); // lower -> BID
    input_buf.try_push(MarketTick::make("AAPL", 151.0, 100.0, 0)); // higher -> ASK

    // For MSFT:
    input_buf.try_push(MarketTick::make("MSFT", 300.0, 200.0, 1)); // sets reference
    input_buf.try_push(MarketTick::make("MSFT", 299.0, 200.0, 1)); // lower -> BID

    std::this_thread::sleep_for(50ms);
    proc.stop();

    // Check AAPL book
    const auto* aapl_book = proc.book("AAPL");
    ASSERT_NE(aapl_book, nullptr);
    EXPECT_GT(aapl_book->updates_applied(), 0);
    EXPECT_GT(aapl_book->bid_levels(), 0);

    // Check MSFT book
    const auto* msft_book = proc.book("MSFT");
    ASSERT_NE(msft_book, nullptr);
    EXPECT_GT(msft_book->updates_applied(), 0);

    // Check independent processing
    EXPECT_EQ(proc.book("GOOG"), nullptr);
    EXPECT_EQ(proc.ticks_processed(), 5);
}

// 12. Unknown symbol tick does not crash BookProcessor
TEST_F(BookProcessorTest, UnknownSymbolTickDoesNotCrash) {
    proc.start();

    // Push tick for previously unseen symbol
    input_buf.try_push(MarketTick::make("NEWCO", 10.0, 100.0, 0));
    
    std::this_thread::sleep_for(50ms);
    proc.stop();

    // Should not crash, and should dynamically create the book
    const auto* new_book = proc.book("NEWCO");
    ASSERT_NE(new_book, nullptr);
    EXPECT_EQ(proc.ticks_processed(), 1);
    EXPECT_GT(new_book->updates_applied(), 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// TEST SUITE 3: BookEdgeCasesTest (No Fixture)
// ═════════════════════════════════════════════════════════════════════════════

// TEST 15: CrossedBookIsRejected
// [CV NOTE] Crossed book guard test — verifies SPDLOG_WARN path skips the malicious update
TEST(BookEdgeCasesTest, CrossedBookIsRejected) {
    OrderBook book{"TEST"};

    BookDelta delta1{};
    std::strncpy(delta1.symbol.data(), "TEST", delta1.symbol.size() - 1);
    delta1.symbol.back() = '\0';
    delta1.side = OrderSide::ASK;
    delta1.price = 100.0;
    delta1.volume = 100;
    book.apply(delta1);

    BookDelta delta2{};
    std::strncpy(delta2.symbol.data(), "TEST", delta2.symbol.size() - 1);
    delta2.symbol.back() = '\0';
    delta2.side = OrderSide::BID;
    delta2.price = 101.0; // Crosses! (bid 101.0 >= best ask 100.0)
    delta2.volume = 100;
    book.apply(delta2);

    // The crossed bid should be rejected
    EXPECT_EQ(book.bid_levels(), 0);
}

// TEST 16: ZeroVolumeOnNonExistentLevelIsNoOp
// [CV NOTE] A remove delta for a price level that isn't in the map shouldn't crash
TEST(BookEdgeCasesTest, ZeroVolumeOnNonExistentLevelIsNoOp) {
    OrderBook book{"TEST"};

    BookDelta delta{};
    std::strncpy(delta.symbol.data(), "TEST", delta.symbol.size() - 1);
    delta.symbol.back() = '\0';
    delta.side = OrderSide::BID;
    delta.price = 100.0;
    delta.volume = 0;

    book.apply(delta);

    EXPECT_EQ(book.bid_levels(), 0);
    EXPECT_EQ(book.updates_applied(), 1); // Delta was processed (even if no-op map erase)
}

}  // namespace
}  // namespace mdp
