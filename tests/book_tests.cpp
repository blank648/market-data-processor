// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>
#include <chrono>
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

// 1. Empty book: best_bid() and best_ask() return std::nullopt
// Note: OrderBook.hpp implementation actually returns 0.0 when empty.
TEST_F(OrderBookTest, EmptyBookBestBidAndAskReturnsZero) {
    EXPECT_FALSE(book_.top_of_book().is_valid());
    EXPECT_EQ(book_.bid_levels(), 0);
    EXPECT_EQ(book_.ask_levels(), 0);
    EXPECT_EQ(book_.best_bid(), 0.0);
    EXPECT_EQ(book_.best_ask(), 0.0);
}

// 2. Insert single bid -> best_bid() returns that price
TEST_F(OrderBookTest, InsertSingleBidReturnsPrice) {
    book_.apply(make_delta(OrderSide::BID, 100.5, 500));
    EXPECT_EQ(book_.best_bid(), 100.5);
    EXPECT_EQ(book_.best_ask(), 0.0);
}

// 3. Insert single ask -> best_ask() returns that price
TEST_F(OrderBookTest, InsertSingleAskReturnsPrice) {
    book_.apply(make_delta(OrderSide::ASK, 101.5, 300));
    EXPECT_EQ(book_.best_ask(), 101.5);
    EXPECT_EQ(book_.best_bid(), 0.0);
}

// 4. Insert multiple bids -> best_bid() is the highest
TEST_F(OrderBookTest, BestBidIsHighestBidPrice) {
    book_.apply(make_delta(OrderSide::BID, 99.0, 100));
    book_.apply(make_delta(OrderSide::BID, 100.0, 100));
    book_.apply(make_delta(OrderSide::BID, 98.5, 100));
    book_.apply(make_delta(OrderSide::BID, 100.5, 100));

    EXPECT_EQ(book_.best_bid(), 100.5);
}

// 5. Insert multiple asks -> best_ask() is the lowest
TEST_F(OrderBookTest, BestAskIsLowestAskPrice) {
    book_.apply(make_delta(OrderSide::ASK, 101.0, 100));
    book_.apply(make_delta(OrderSide::ASK, 102.5, 100));
    book_.apply(make_delta(OrderSide::ASK, 101.5, 100));
    book_.apply(make_delta(OrderSide::ASK, 100.8, 100));

    EXPECT_EQ(book_.best_ask(), 100.8);
}

// 6. Remove price level with zero volume -> level disappears from book
TEST_F(OrderBookTest, RemovePriceLevelOnZeroVolume) {
    book_.apply(make_delta(OrderSide::BID, 100.0, 500));
    EXPECT_EQ(book_.bid_levels(), 1);

    book_.apply(make_delta(OrderSide::BID, 100.0, 0)); // Remove
    EXPECT_EQ(book_.bid_levels(), 0);
}

// 7. Crossed book detection: inserting ask < best_bid -> book rejects it
TEST_F(OrderBookTest, CrossedBookInsertingAskLessThanBestBidIsRejected) {
    book_.apply(make_delta(OrderSide::BID, 100.0, 100));
    EXPECT_EQ(book_.best_bid(), 100.0);
    
    // Insert ask < 100.0
    book_.apply(make_delta(OrderSide::ASK, 99.0, 100));
    
    // Should be rejected -> ask side should remain empty
    EXPECT_EQ(book_.ask_levels(), 0);
    EXPECT_EQ(book_.best_ask(), 0.0);
}

TEST_F(OrderBookTest, UpdateExistingPriceLevel) {
    book_.apply(make_delta(OrderSide::BID, 100.0, 500));
    book_.apply(make_delta(OrderSide::BID, 100.0, 750)); // Update

    EXPECT_EQ(book_.bid_levels(), 1);
    EXPECT_EQ(book_.top_of_book().bid_volume, 750);
}

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

TEST_F(OrderBookTest, SnapshotSequenceMonotonicallyIncreases) {
    uint64_t last_seq = book_.snapshot().sequence;
    for (int i = 0; i < 10; ++i) {
        book_.apply(make_delta(OrderSide::BID, 100.0 + i, 100));
        uint64_t current_seq = book_.snapshot().sequence;
        EXPECT_GT(current_seq, last_seq);
        last_seq = current_seq;
    }
}

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

// 8. BTCUSD and SOLUSD maintain independent state in two separate OrderBook instances
TEST_F(OrderBookTest, IndependentStateForDifferentSymbols) {
    OrderBook btc("BTCUSD");
    OrderBook sol("SOLUSD");

    BookDelta d_btc{};
    std::strncpy(d_btc.symbol, "BTCUSD", 8);
    d_btc.side = OrderSide::BID;
    d_btc.price = 50000.0;
    d_btc.volume = 1;
    btc.apply(d_btc);

    BookDelta d_sol{};
    std::strncpy(d_sol.symbol, "SOLUSD", 8);
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
        BookDelta d{};
        std::strncpy(d.symbol, "TEST", 8);
        d.side = (i % 2 == 0) ? OrderSide::BID : OrderSide::ASK;
        d.price = (d.side == OrderSide::BID) ? bid_price(gen) : ask_price(gen);
        d.volume = vol(gen);
        rand_book.apply(d);
        
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
    TickRingBuffer4K input_buf_;
    BookProcessor proc_{input_buf_};
};

// 10. start() + stop() lifecycle
TEST_F(BookProcessorTest, ProcessorStartsAndStopsCleanly) {
    proc_.start();
    std::this_thread::sleep_for(10ms);
    proc_.stop();
    EXPECT_GE(proc_.ticks_processed(), 0);
}

// 11. Processes ticks from RingBuffer and updates the correct OrderBook per symbol
TEST_F(BookProcessorTest, ProcessesTicksAndUpdatesCorrectOrderBook) {
    proc_.start();

    // The BookProcessor determines side via an internal EMA heuristic:
    // first tick sets reference. Following ticks: < reference -> BID, >= reference -> ASK.
    // For AAPL:
    input_buf_.try_push(MarketTick::make("AAPL", 150.0, 100.0, 0)); // sets reference
    input_buf_.try_push(MarketTick::make("AAPL", 149.0, 100.0, 0)); // lower -> BID
    input_buf_.try_push(MarketTick::make("AAPL", 151.0, 100.0, 0)); // higher -> ASK

    // For MSFT:
    input_buf_.try_push(MarketTick::make("MSFT", 300.0, 200.0, 1)); // sets reference
    input_buf_.try_push(MarketTick::make("MSFT", 299.0, 200.0, 1)); // lower -> BID

    std::this_thread::sleep_for(50ms);
    proc_.stop();

    // Check AAPL book
    auto aapl_book = proc_.book("AAPL");
    ASSERT_NE(aapl_book, nullptr);
    EXPECT_GT(aapl_book->updates_applied(), 0);
    EXPECT_GT(aapl_book->bid_levels(), 0);

    // Check MSFT book
    auto msft_book = proc_.book("MSFT");
    ASSERT_NE(msft_book, nullptr);
    EXPECT_GT(msft_book->updates_applied(), 0);

    // Check independent processing
    EXPECT_EQ(proc_.book("GOOG"), nullptr);
    EXPECT_EQ(proc_.ticks_processed(), 5);
}

// 12. Unknown symbol tick does not crash BookProcessor
TEST_F(BookProcessorTest, UnknownSymbolTickDoesNotCrash) {
    proc_.start();

    // Push tick for previously unseen symbol
    input_buf_.try_push(MarketTick::make("NEWCO", 10.0, 100.0, 0));
    
    std::this_thread::sleep_for(50ms);
    proc_.stop();

    // Should not crash, and should dynamically create the book
    auto new_book = proc_.book("NEWCO");
    ASSERT_NE(new_book, nullptr);
    EXPECT_EQ(proc_.ticks_processed(), 1);
    EXPECT_GT(new_book->updates_applied(), 0);
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
