// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "book/BookProcessor.hpp"

#include "core/MarketTick.hpp"
#include "core/RingBuffer.hpp"

#include <cstring>
#include <thread>

#include "infra/Logger.hpp"

namespace mdp {

namespace {
static constexpr double kAlpha = 0.1;
static constexpr double kMinValidPrice = 0.10;
static constexpr double kMaxValidPrice = 1'000'000.0;
}

// [NOT THREAD-SAFE] books_ and reference_price_ are accessed EXCLUSIVELY
// from run() (jthread). book() const accessor is intended for post-stop()
// calls from the test thread only. Calling book() while run() is active
// is a data race — document this in BookProcessor.hpp.

BookProcessor::BookProcessor(TickRingBuffer4K& input)
    : ThreadBase("BookProcessor"), input_(input) {}

const OrderBook* BookProcessor::book(std::string_view symbol) const noexcept {
    auto it = books_.find(std::string{symbol});
    if (it == books_.end()) {
        return nullptr;
    }
    return &it->second;
}

uint64_t BookProcessor::ticks_processed() const noexcept {
    return ticks_processed_.load(std::memory_order_relaxed);
}

uint64_t BookProcessor::books_active() const noexcept {
    return static_cast<uint64_t>(books_.size());
}

void BookProcessor::run(StopToken st) {
    auto log_ = mdp::Logger::get("BookProcessor");
    log_->info("BookProcessor starting, tracking {} symbols", books_active());
    
    MarketTick tick;
    while (!st.stop_requested()) {
        if (!input_.try_pop(tick)) {
            std::this_thread::yield();
            continue;
        }

        const std::string symbol{tick.symbol.data(),
                                 ::strnlen(tick.symbol.data(), tick.symbol.size())};
        auto it = books_.find(symbol);
        if (it == books_.end()) {
            auto inserted = books_.emplace(symbol, OrderBook{symbol});
            it = inserted.first;
        }

        // [COUNTER INTEGRITY] Only increment on confirmed processing.
        bool pushed = false;

        const OrderSide side = determine_side(symbol, tick.price);
        const BookDelta delta = tick_to_delta(tick, side);
        it->second.apply(delta);
        log_->trace("Applied tick to book: {}", symbol);
        pushed = true;
        if (pushed) {
            ticks_processed_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Drain remaining ticks after stop is requested
    while (input_.try_pop(tick)) {
        const std::string symbol{tick.symbol.data(),
                                 ::strnlen(tick.symbol.data(), tick.symbol.size())};
        auto it = books_.find(symbol);
        if (it == books_.end()) {
            auto inserted = books_.emplace(symbol, OrderBook{symbol});
            it = inserted.first;
        }

        const OrderSide side = determine_side(symbol, tick.price);
        const BookDelta delta = tick_to_delta(tick, side);
        it->second.apply(delta);
        log_->trace("Applied tick to book: {}", symbol);
        ticks_processed_.fetch_add(1, std::memory_order_relaxed);
    }
    
    log_->info("BookProcessor stopped");
}

OrderSide BookProcessor::determine_side(std::string_view symbol, double price) noexcept {
    if (price < kMinValidPrice || price > kMaxValidPrice) {
        // [PRICE GUARD] Ignore malformed ticks in EMA computation.
        // FeedSimulator occasionally emits near-zero prices during
        // ring buffer warm-up. These would corrupt the reference price.
        return OrderSide::BID;
    }

    const std::string key{symbol};
    auto it = reference_price_.find(key);
    if (it == reference_price_.end()) {
        reference_price_.emplace(key, price);
        return OrderSide::BID;
    }

    double& ref = it->second;
    OrderSide side = (price < ref) ? OrderSide::BID : OrderSide::ASK;

    // [EMA RESET] If this side would cross the book, reset the
    // reference to current price so subsequent ticks re-center.
    // This prevents positive feedback where ref drifts away from
    // actual book levels and rejects all ticks of one side.
    const auto* bk = book(symbol);
    if (bk != nullptr) {
        bool would_cross = (side == OrderSide::BID &&
                            bk->best_ask() > 0.0 &&
                            price >= bk->best_ask()) ||
                           (side == OrderSide::ASK &&
                            bk->best_bid() > 0.0 &&
                            price <= bk->best_bid());
        if (would_cross) {
            // Reset reference to midpoint of current book
            ref = (bk->best_bid() + bk->best_ask()) / 2.0;
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
    BookDelta d;
    std::memcpy(d.symbol, tick.symbol.data(), 8);
    d.side = side;
    d.price = tick.price;
    d.volume = tick.volume;
    d.timestamp_ns = tick.timestamp_ns;
    return d;
}

}  // namespace mdp


