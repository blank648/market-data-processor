// Copyright (c) 2026 Market Data Processor Project. All rights reserved.
// SPDX-License-Identifier: MIT

#include "book/OrderBook.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <ranges>
#include <string>

namespace mdp {

OrderBook::OrderBook(std::string_view symbol) {
    std::memset(symbol_, 0, sizeof(symbol_));
    const std::string safe_symbol(symbol);
    std::strncpy(symbol_, safe_symbol.c_str(), sizeof(symbol_) - 1);
    symbol_[sizeof(symbol_) - 1] = '\0';
}

void OrderBook::apply(const BookDelta& delta) noexcept {
    // Hot path: called at high frequency; keep logic branch-simple and allocation-free.
    // noexcept is critical here: throwing in the hot path would terminate the process.
    ++sequence_;
    ++updates_applied_;

    // [CROSSED BOOK GUARD] In production, crossed books indicate feed error.
    // We reject rather than correct to avoid masking upstream bugs.
    if (delta.volume > 0) {
        if (delta.side == OrderSide::BID && !asks_.empty() && delta.price >= asks_.begin()->first) {
            const double best_ask = asks_.begin()->first;
            std::fprintf(stderr,
                         "[BOOK WARN] Crossed book rejected for %.8s: "
                         "bid %.6f >= best ask %.6f\n",
                         symbol_, delta.price, best_ask);
            return;
        }

        if (delta.side == OrderSide::ASK && !bids_.empty() && delta.price <= bids_.rbegin()->first) {
            const double best_bid = bids_.rbegin()->first;
            std::fprintf(stderr,
                         "[BOOK WARN] Crossed book rejected for %.8s: "
                         "ask %.6f <= best bid %.6f\n",
                         symbol_, delta.price, best_bid);
            return;
        }
    }

    if (delta.side == OrderSide::BID) {
        if (delta.volume == 0) {
            levels_removed_ += bids_.erase(delta.price);
            return;
        }
        bids_[delta.price] = delta.volume;
        return;
    }

    if (delta.volume == 0) {
        levels_removed_ += asks_.erase(delta.price);
        return;
    }

    asks_[delta.price] = delta.volume;
}

TopOfBook OrderBook::top_of_book() const noexcept {
    TopOfBook tob{};
    tob.symbol = symbol();

    if (!bids_.empty()) {
        const auto it = bids_.rbegin();
        tob.best_bid = it->first;
        tob.bid_volume = it->second;
    }

    if (!asks_.empty()) {
        const auto it = asks_.begin();
        tob.best_ask = it->first;
        tob.ask_volume = it->second;
    }

    if (tob.best_bid > 0.0 && tob.best_ask > 0.0) {
        tob.spread = tob.best_ask - tob.best_bid;
    }

    tob.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    return tob;
}

BookSnapshot OrderBook::snapshot() const {
    // Snapshot path is diagnostic/test oriented and may allocate vector storage.
    BookSnapshot snap{};
    std::strncpy(snap.symbol, symbol_, sizeof(snap.symbol) - 1);
    snap.symbol[sizeof(snap.symbol) - 1] = '\0';
    snap.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    snap.sequence = sequence_;

    snap.bids.reserve(bids_.size());
    for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
        snap.bids.push_back(PriceLevel{it->first, it->second});
    }

    snap.asks.reserve(asks_.size());
    std::ranges::transform(asks_, std::back_inserter(snap.asks),
                           [](const auto& kv) { return PriceLevel{kv.first, kv.second}; });

    return snap;
}

std::size_t OrderBook::bid_levels() const noexcept {
    return bids_.size();
}

std::size_t OrderBook::ask_levels() const noexcept {
    return asks_.size();
}

void OrderBook::clear() noexcept {
    bids_.clear();
    asks_.clear();
    sequence_ = 0;
    updates_applied_ = 0;
    levels_removed_ = 0;
}

uint64_t OrderBook::updates_applied() const noexcept {
    return updates_applied_;
}

uint64_t OrderBook::levels_removed() const noexcept {
    return levels_removed_;
}

std::string_view OrderBook::symbol() const noexcept {
    return std::string_view(symbol_);
}

}  // namespace mdp


