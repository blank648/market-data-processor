// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "book/BookProcessor.hpp"

#include <cstring>
#include <thread>

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"
#include "infra/Logger.hpp"

namespace mdp {

namespace {
constexpr double kAlpha = 0.1;
constexpr double kMinValidPrice = 0.10;
constexpr double kMaxValidPrice = 1'000'000.0;
}  // namespace

// [NOT THREAD-SAFE] books_ and reference_price_ are accessed EXCLUSIVELY
// from run() (jthread). book() const accessor is intended for post-stop()
// calls from the test thread only. Calling book() while run() is active
// is a data race — document this in BookProcessor.hpp.

BookProcessor::BookProcessor(TickRingBuffer4K& input)
    : ThreadBase("BookProcessor"), input_(input) {}

BookProcessor::BookProcessor(TickRingBuffer4K& input, SnapshotRingBuffer4K& db_queue)
    : ThreadBase("BookProcessor"), input_(input), db_queue_(&db_queue) {}

BookProcessor::BookProcessor(TickRingBuffer4K& input, SnapshotRingBuffer4K& db_queue,
                             SnapshotRingBuffer4K& signal_queue)
    : ThreadBase("BookProcessor"),
      input_(input),
      db_queue_(&db_queue),
      signal_queue_(&signal_queue) {}

const OrderBook* BookProcessor::book(std::string_view symbol) const noexcept {
    auto iter = books_.find(std::string{symbol});
    if (iter == books_.end()) {
        return nullptr;
    }
    return &iter->second;
}

uint64_t BookProcessor::ticks_processed() const noexcept {
    return ticks_processed_.load(std::memory_order_relaxed);
}

uint64_t BookProcessor::books_active() const noexcept {
    return static_cast<uint64_t>(books_.size());
}

uint64_t BookProcessor::snapshots_dropped() const noexcept {
    return snapshots_dropped_.load(std::memory_order_relaxed);
}

void BookProcessor::run(StopToken stop_token) {
    auto log_ = mdp::Logger::get("BookProcessor");
    log_->info("BookProcessor starting, tracking {} symbols", books_active());

    MarketTick tick{};
    while (!stop_token.stop_requested()) {
        if (!input_.try_pop(tick)) {
            std::this_thread::yield();
            continue;
        }
        process_tick(tick);
    }

    // Drain remaining ticks after stop is requested
    while (input_.try_pop(tick)) {
        process_tick(tick);
    }

    log_->info("BookProcessor stopped");
}

void BookProcessor::process_tick(const MarketTick& tick) {
    auto log_ = mdp::Logger::get("BookProcessor");
    const std::string symbol{tick.symbol.data(), ::strnlen(tick.symbol.data(), tick.symbol.size())};
    auto iter = books_.find(symbol);
    if (iter == books_.end()) {
        auto inserted = books_.emplace(symbol, OrderBook{symbol});
        iter = inserted.first;
    }

    const OrderSide side = determine_side(symbol, tick.price);
    const BookDelta delta = tick_to_delta(tick, side);

    TopOfBook old_top = iter->second.top_of_book();
    iter->second.apply(delta);
    TopOfBook new_top = iter->second.top_of_book();

    log_->trace("Applied tick to book: {}", symbol);
    ticks_processed_.fetch_add(1, std::memory_order_relaxed);

    if ((db_queue_ != nullptr || signal_queue_ != nullptr) &&
        (old_top.best_bid != new_top.best_bid || old_top.best_ask != new_top.best_ask)) {
        MarketSnapshot snap{};
        snap.symbol = tick.symbol;
        // Mid-price representation. If one side is empty, use the other.
        if (new_top.best_bid > 0 && new_top.best_ask > 0) {
            snap.price = (new_top.best_bid + new_top.best_ask) / 2.0;
        } else if (new_top.best_bid > 0) {
            snap.price = new_top.best_bid;
        } else {
            snap.price = new_top.best_ask;
        }
        snap.volume = static_cast<double>(new_top.bid_volume + new_top.ask_volume);
        snap.timestamp_ns = static_cast<int64_t>(tick.timestamp_ns);
        snap.sequence = static_cast<int64_t>(iter->second.updates_applied());

        if (db_queue_ != nullptr) {
            if (!db_queue_->try_push(snap)) {
                const auto dropped = snapshots_dropped_.fetch_add(1, std::memory_order_relaxed) + 1;
                log_->warn("Snapshot dropped for {} — db_queue full (total dropped: {})", symbol, dropped);
            }
        }
        if (signal_queue_ != nullptr) {
            if (!signal_queue_->try_push(snap)) {
                log_->warn("Snapshot dropped for {} — signal_queue full", symbol);
            }
        }
    }
}

OrderSide BookProcessor::determine_side(std::string_view symbol, double price) noexcept {
    if (price < kMinValidPrice || price > kMaxValidPrice) {
        // [PRICE GUARD] Ignore malformed ticks in EMA computation.
        // FeedSimulator occasionally emits near-zero prices during
        // ring buffer warm-up. These would corrupt the reference price.
        return OrderSide::BID;
    }

    const std::string key{symbol};
    auto iter = reference_price_.find(key);
    if (iter == reference_price_.end()) {
        reference_price_.emplace(key, price);
        return OrderSide::BID;
    }

    double& ref = iter->second;
    OrderSide side = (price < ref) ? OrderSide::BID : OrderSide::ASK;

    // [EMA RESET] If this side would cross the book, reset the
    // reference to current price so subsequent ticks re-center.
    // This prevents positive feedback where ref drifts away from
    // actual book levels and rejects all ticks of one side.
    const auto* book_ptr = book(symbol);
    if (book_ptr != nullptr) {
        bool would_cross =
            (side == OrderSide::BID && book_ptr->best_ask() > 0.0 &&
             price >= book_ptr->best_ask()) ||
            (side == OrderSide::ASK && book_ptr->best_bid() > 0.0 && price <= book_ptr->best_bid());
        if (would_cross) {
            // Reset reference to midpoint of current book
            ref = (book_ptr->best_bid() + book_ptr->best_ask()) / 2.0;
            // Re-classify against the reset reference
            side = (price < ref) ? OrderSide::BID : OrderSide::ASK;
        }
    }

    ref = (kAlpha * price) + ((1.0 - kAlpha) * ref);

    // [SIDE HEURISTIC] Real market data includes explicit side field.
    // We derive side from price relative to EMA as a simulation approximation.
    // This is noted in README as a known simplification.
    return side;
}

BookDelta BookProcessor::tick_to_delta(const MarketTick& tick, OrderSide side) noexcept {
    BookDelta delta{};
    delta.symbol = tick.symbol;
    delta.side = side;
    delta.price = tick.price;
    delta.volume = static_cast<uint64_t>(tick.volume);
    delta.timestamp_ns = tick.timestamp_ns;
    return delta;
}

}  // namespace mdp